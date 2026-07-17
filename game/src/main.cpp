//
// Golden-path Vulkan (MoltenVK) + Volk + VMA + GLFW live-link game.
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>

using std::optional;

#ifndef WITH_DEBUG_UI
#define WITH_DEBUG_UI 1
#endif

#if defined(__APPLE__)
	#define VK_USE_PLATFORM_METAL_EXT
#endif

#define VK_NO_PROTOTYPES
#define VOLK_IMPLEMENTATION
#include "volk/volk.h"

#include "vma/vk_mem_alloc.h"

#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"

#define HANDMADE_MATH_IMPLEMENTATION
#include "handmade_math/HandmadeMath.h"

// Simple Template Wrapper around stb_ds array
#define STB_DS_IMPLEMENTATION
#include "core/stretchy_buffer.h"

// Command line argument parsing
#include "cxxopts/cxxopts.hpp"

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	#define IMGUI_IMPLEMENTATION
	#include "../../game_old/src/extern/imgui/misc/single_file/imgui_single_file.h"
	#include "extern/imgui/backends/imgui_impl_glfw.cpp"
	#include "extern/imgui/backends/imgui_impl_vulkan.cpp"
#endif

// Generated flatbuffer schema (from ../compiled_schemas/cpp)
#include "blender_live_link_generated.h"

#include "core/types.h"
#include "core/timings.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/gpu_buffer.h"
#include "game_object/game_object.h"
#include "state/state.h"
#include "core/benchmark.h"
#include "render/geometry_pass.h"
#include "render/shadow_depth_pass.h"
#include "render/shadow_blur_pass.h"
#include "render/shadow_cascade_debug_pass.h"
#include "render/ssao_pass.h"
#include "render/blur_pass.h"
#include "render/screen_space_shadows_pass.h"
#include "render/fog_pass.h"
#include "render/dof_combine_pass.h"
#include "render/wire_overlay_pass.h"
#include "render/temporal_aa_pass.h"
#include "render/fxaa_pass.h"
#include "render/gpu_skinning.h"
#include "render/tessellation.h"
#include "render/lighting_capture.h"
#include "render/gi.h"
#include "render/gi_debug_pass.h"
#include "render/lighting_pass.h"
#include "render/tonemapping_pass.h"
#include "render/sky_pass.h"
#include "render/copy_to_swapchain_pass.h"

void handle_resize(bool in_force);
void rewind_skinned_animations();

#include "render/imgui_layer.h"

static GI_Scene gi_scene;

// Key with no binding, used as the "no modifier" sentinel in the event macros
// (GLFW key 0 is unassigned; is_key_pressed(0) is always false, so we special
// case it below)
#define KEYCODE_NONE 0

// Macro to define an event and run code in __VA_ARGS__ when it triggers
#define DEFINE_EVENT_TWO_KEYS(key_1, key_2, ...)\
	{\
		static bool was_key_pressed = false;\
		const bool is_key_1_pressed = is_key_pressed(key_1);\
		const bool is_key_2_pressed = (key_2 == KEYCODE_NONE) ? true : is_key_pressed(key_2);\
		if (is_key_1_pressed && is_key_2_pressed && !was_key_pressed)\
		{\
			{__VA_ARGS__}\
		}\
		was_key_pressed = is_key_1_pressed && is_key_2_pressed;\
	}

// Same as above but no mod key
#define DEFINE_EVENT_ONE_KEY(key, ...) DEFINE_EVENT_TWO_KEYS(key, KEYCODE_NONE, __VA_ARGS__)

// Macro to handle toggleable inputs. uses event macro above but just sets variable instead of passing in __VA_ARGS__
#define DEFINE_TOGGLE_TWO_KEYS(var_name, key_1, key_2)\
	DEFINE_EVENT_TWO_KEYS(key_1, key_2,\
		var_name = !var_name;\
	);

// Same as above but no mod key
#define DEFINE_TOGGLE_ONE_KEY(var_name, key)\
	DEFINE_TOGGLE_TWO_KEYS(var_name, key, KEYCODE_NONE)

Camera& get_active_camera()
{
	if (state.scene.camera_control_id && !state.debug_camera.active)
	{
		Object& camera_control_object = state.scene.objects[*state.scene.camera_control_id];
		return camera_control_object.camera_control.camera;
	}
	return state.debug_camera.camera;
}

// Flatbuffer helper conversion functions
namespace flatbuffer_helpers
{
	HMM_Vec3 to_hmm_vec3(const Blender::LiveLink::Vec3* in_flatbuffers_vector)
	{
		assert(in_flatbuffers_vector);
		return HMM_V3(
			in_flatbuffers_vector->x(),
			in_flatbuffers_vector->y(),
			in_flatbuffers_vector->z()
		);
	}

	HMM_Vec4 to_hmm_vec4(const Blender::LiveLink::Vec3* in_flatbuffers_vector, f32 in_w)
	{
		assert(in_flatbuffers_vector);
		return HMM_V4(
			in_flatbuffers_vector->x(),
			in_flatbuffers_vector->y(),
			in_flatbuffers_vector->z(),
			in_w
		);
	}

	HMM_Vec4 to_hmm_vec4(const Blender::LiveLink::Vec4* in_flatbuffers_vector)
	{
		assert(in_flatbuffers_vector);
		return HMM_V4(
			in_flatbuffers_vector->x(),
			in_flatbuffers_vector->y(),
			in_flatbuffers_vector->z(),
			in_flatbuffers_vector->w()
		);
	}

	HMM_Quat to_hmm_quat(const Blender::LiveLink::Quat* in_flatbuffers_quat)
	{
		assert(in_flatbuffers_quat);
		return HMM_Q(
			in_flatbuffers_quat->x(),
			in_flatbuffers_quat->y(),
			in_flatbuffers_quat->z(),
			in_flatbuffers_quat->w()
		);
	}

	// Column-major passthrough: flatbuffer stores 16 floats column-major,
	// HMM Elements[col][row] is column-major, GLSL mat4 is column-major —
	// no transpose anywhere (port of game/src/main.cpp:221-239)
	HMM_Mat4 to_hmm_mat4(const Blender::LiveLink::Matrix* in_flatbuffers_matrix)
	{
		HMM_Mat4 out_matrix = HMM_M4D(1.0f);
		if (!in_flatbuffers_matrix || !in_flatbuffers_matrix->elements() || in_flatbuffers_matrix->elements()->size() < 16)
		{
			return out_matrix;
		}

		auto elements = in_flatbuffers_matrix->elements();
		for (i32 col = 0; col < 4; ++col)
		{
			for (i32 row = 0; row < 4; ++row)
			{
				out_matrix.Elements[col][row] = elements->Get(col * 4 + row);
			}
		}

		return out_matrix;
	}
}

char* copy_flatbuffer_string(const flatbuffers::String* in_string)
{
	if (!in_string)
	{
		return nullptr;
	}

	const char* source = in_string->c_str();
	const size_t length = strlen(source);
	char* result = (char*) malloc(length + 1);
	memcpy(result, source, length + 1);
	return result;
}

// Parses a size-prefixed flatbuffer Update and sends results to the main
// thread via channels. Runs on the live link thread — GPU resources are only
// described here (lazy GpuBuffer), never created.
// Parses the complete live-link payload used by game: content resources,
// objects/components, deletes, reset, and import statistics.
void parse_flatbuffer_data(StretchyBuffer<u8>& flatbuffer_data)
{
	if (flatbuffer_data.length() == 0)
	{
		return;
	}

	// Interpret Flatbuffer data
	auto* update = Blender::LiveLink::GetSizePrefixedUpdate(flatbuffer_data.data());
	assert(update);

	// Everything in this Update is packaged into one SceneUpdate message and
	// registered/applied on the main thread at drain time
	SceneUpdate scene_update;
	scene_update.stats.byte_count = (u64) flatbuffer_data.length();
	scene_update.stats.generation_seconds = update->generation_seconds();
	scene_update.stats.reset = update->reset();

	// process images from update (pixels copied here — the flatbuffer memory
	// dies with this function; the drain frees them after GPU upload)
	if (auto images = update->images())
	{
		scene_update.stats.image_count = (i32) images->size();
		for (u32 idx = 0; idx < images->size(); ++idx)
		{
			auto image = images->Get(idx);
			assert(image);

			auto image_data = image->data();
			if (image_data) { scene_update.stats.image_byte_count += image_data->size(); }
			const i32 width = image->width();
			const i32 height = image->height();
			if (!image_data || width <= 0 || height <= 0)
			{
				printf("\tSkipping malformed image UID: %i\n", image->unique_id());
				continue;
			}

			const u64 expected_size = (u64) width * (u64) height * 4;
			if (image_data->size() != expected_size)
			{
				printf("\tSkipping image UID %i: data size %u != %llu\n",
					image->unique_id(), image_data->size(), (unsigned long long) expected_size);
				continue;
			}

			u8* pixels = (u8*) malloc(expected_size);
			memcpy(pixels, image_data->data(), expected_size);

			scene_update.images.add((PendingImage) {
				.unique_id = image->unique_id(),
				.width = width,
				.height = height,
				.pixels = pixels,
			});
		}
	}

	// process materials from update (raw image ids; resolved at drain after
	// this update's images register)
	if (auto materials = update->materials())
	{
		scene_update.stats.material_count = (i32) materials->size();
		for (u32 idx = 0; idx < materials->size(); ++idx)
		{
			auto material = materials->Get(idx);
			assert(material);

			PendingMaterial pending_material = {
				.unique_id = material->unique_id(),
				.metallic = material->metallic(),
				.roughness = material->roughness(),
				.emission_strength = material->emission_strength(),
				.base_color_image_id = material->base_color_image_id(),
				.emission_color_image_id = material->emission_color_image_id(),
				.metallic_image_id = material->metallic_image_id(),
				.roughness_image_id = material->roughness_image_id(),
			};
			if (auto base_color = material->base_color())
			{
				pending_material.base_color = flatbuffer_helpers::to_hmm_vec4(base_color);
			}
			if (auto emission_color = material->emission_color())
			{
				pending_material.emission_color = flatbuffer_helpers::to_hmm_vec4(emission_color);
			}

			scene_update.materials.add(pending_material);
		}
	}

	// process objects from update
	if (auto objects = update->objects())
	{
		scene_update.stats.object_count = (i32) objects->size();
		for (u32 idx = 0; idx < objects->size(); ++idx)
		{
			auto object = objects->Get(idx);
			if (auto object_name = object->name())
			{
				printf("\tObject Name: %s\n", object_name->c_str());
			}

			int unique_id = object->unique_id();
			bool visibility = object->visibility();

			auto object_location = object->location();
			auto object_scale = object->scale();
			auto object_rotation = object->rotation();
			if (!object_location || !object_scale || !object_rotation)
			{
				printf("\tDropping malformed object UID: %i\n", unique_id);
				scene_update.stats.malformed_object_count += 1;
				continue;
			}

			HMM_Vec4 location	= flatbuffer_helpers::to_hmm_vec4(object_location, 1.0f);
			HMM_Vec3 scale		= flatbuffer_helpers::to_hmm_vec3(object_scale);
			HMM_Quat rotation	= flatbuffer_helpers::to_hmm_quat(object_rotation);

			Object game_object = object_create(
				unique_id,
				copy_flatbuffer_string(object->name()),
				visibility,
				location,
				rotation,
				scale
			);

			if (auto object_mesh = object->mesh())
			{
				u32 num_vertices = 0;
				Vertex* vertices = nullptr;

				auto flatbuffer_positions = object_mesh->positions();
				auto flatbuffer_normals = object_mesh->normals();
				auto flatbuffer_texcoords = object_mesh->texcoords();
				const bool has_valid_vertex_streams =
					flatbuffer_positions &&
					flatbuffer_normals &&
					flatbuffer_texcoords &&
					(flatbuffer_positions->size() % 3) == 0 &&
					flatbuffer_normals->size() >= flatbuffer_positions->size() &&
					flatbuffer_texcoords->size() >= (flatbuffer_positions->size() / 3) * 2;
				if (has_valid_vertex_streams)
				{
					num_vertices = flatbuffer_positions->size() / 3;
					vertices = (Vertex*) malloc(sizeof(Vertex) * num_vertices);
					for (u32 vertex_idx = 0; vertex_idx < num_vertices; ++vertex_idx)
					{
						vertices[vertex_idx] = {
							.position = {
								.X = flatbuffer_positions->Get(vertex_idx * 3 + 0),
								.Y = flatbuffer_positions->Get(vertex_idx * 3 + 1),
								.Z = flatbuffer_positions->Get(vertex_idx * 3 + 2),
								.W = 1.0,
							},
							.normal = {
								.X = flatbuffer_normals->Get(vertex_idx * 3 + 0),
								.Y = flatbuffer_normals->Get(vertex_idx * 3 + 1),
								.Z = flatbuffer_normals->Get(vertex_idx * 3 + 2),
								.W = 0.0,
							},
							.texcoord = {
								.X = flatbuffer_texcoords->Get(vertex_idx * 2 + 0),
								.Y = flatbuffer_texcoords->Get(vertex_idx * 2 + 1),
							},
						};
					}
				}
				else
				{
					printf("\tDropping malformed mesh vertex streams on object UID: %i\n", unique_id);
				}

				// Check for skinning data (port of game/src/main.cpp:816-864)
				SkinnedVertex* skinned_vertices = nullptr;
				u32 skin_matrix_count = 0;
				i32 armature_id = object_mesh->armature_id();
				auto flatbuffer_joint_indices = object_mesh->joint_indices();
				auto flatbuffer_joint_weights = object_mesh->joint_weights();
				if (armature_id > 0 && flatbuffer_joint_indices && flatbuffer_joint_weights)
				{
					const u32 num_joint_indices = flatbuffer_joint_indices->size();
					const u32 num_joint_weights = flatbuffer_joint_weights->size();
					u32 num_skinned_vertices = num_joint_indices / 4;
					if (num_vertices == 0 ||
						num_joint_indices != num_joint_weights ||
						(num_joint_indices % 4) != 0 ||
						num_vertices != num_skinned_vertices)
					{
						printf("\tDropping malformed skinning data on object UID: %i\n", unique_id);
					}
					else
					{
						i32 max_joint_index = 0;
						skinned_vertices = (SkinnedVertex*) malloc(sizeof(SkinnedVertex) * num_vertices);
						for (u32 vertex_idx = 0; vertex_idx < num_vertices; ++vertex_idx)
						{
							for (u32 influence_idx = 0; influence_idx < 4; ++influence_idx)
							{
								max_joint_index = MAX(max_joint_index, flatbuffer_joint_indices->Get(vertex_idx * 4 + influence_idx));
							}

							skinned_vertices[vertex_idx] = {
								.joint_indices = {
									.X = (f32) flatbuffer_joint_indices->Get(vertex_idx * 4 + 0),
									.Y = (f32) flatbuffer_joint_indices->Get(vertex_idx * 4 + 1),
									.Z = (f32) flatbuffer_joint_indices->Get(vertex_idx * 4 + 2),
									.W = (f32) flatbuffer_joint_indices->Get(vertex_idx * 4 + 3),
								},
								.joint_weights = {
									.X = flatbuffer_joint_weights->Get(vertex_idx * 4 + 0),
									.Y = flatbuffer_joint_weights->Get(vertex_idx * 4 + 1),
									.Z = flatbuffer_joint_weights->Get(vertex_idx * 4 + 2),
									.W = flatbuffer_joint_weights->Get(vertex_idx * 4 + 3),
								},
							};
						}
						skin_matrix_count = (u32) max_joint_index + 1;
					}
				}

				u32 num_indices = 0;
				u32* indices = nullptr;
				if (auto flatbuffer_indices = object_mesh->indices())
				{
					if ((flatbuffer_indices->size() % 3) != 0)
					{
						printf("\tDropping malformed triangle index stream on object UID: %i\n", unique_id);
					}
					else
					{
						num_indices = flatbuffer_indices->size();
						indices = (u32*) malloc(sizeof(u32) * num_indices);
						for (u32 indices_idx = 0; indices_idx < num_indices; ++indices_idx)
						{
							indices[indices_idx] = flatbuffer_indices->Get(indices_idx);
						}
					}
				}

				// Material ids stay raw here; resolve_mesh_material_indices
				// maps them to indices at drain (game/ resolves at parse, but
				// game registers materials on the main thread)
				u32 num_material_indices = 0;
				i32* material_indices = nullptr;
				if (auto flatbuffer_material_ids = object_mesh->material_ids())
				{
					num_material_indices = flatbuffer_material_ids->size();
					material_indices = (i32*) malloc(sizeof(i32) * num_material_indices);
					for (u32 material_id_idx = 0; material_id_idx < num_material_indices; ++material_id_idx)
					{
						material_indices[material_id_idx] = flatbuffer_material_ids->Get(material_id_idx);
					}
				}

				// Set Mesh Data on Game Object
				if (num_vertices > 0 && num_indices > 0)
				{
					const MeshInitData mesh_init_data = {
						.num_indices = num_indices,
						.indices = indices,
						.num_vertices = num_vertices,
						.vertices = vertices,
						.num_material_indices = num_material_indices,
						.material_indices = material_indices,
						.skinned_vertices = skinned_vertices,
						.skin_matrix_count = skin_matrix_count,
						.armature_id = armature_id,
						.mesh_to_armature = flatbuffer_helpers::to_hmm_mat4(object_mesh->mesh_to_armature()),
						.armature_to_mesh = flatbuffer_helpers::to_hmm_mat4(object_mesh->armature_to_mesh()),
					};
					game_object.mesh = make_mesh(mesh_init_data);
					game_object.has_mesh = true;
				}
				else
				{
					free(indices);
					free(vertices);
					free(material_indices);
					free(skinned_vertices);
				}
			}

			// Armature: bones + animation clips (port of game/src/main.cpp:936-1017)
			if (auto object_armature = object->armature())
			{
				game_object.has_armature = true;
				game_object.armature = {};

				if (auto flatbuffer_bones = object_armature->bones())
				{
					game_object.armature.bone_count = flatbuffer_bones->size();
					game_object.armature.bones = (ArmatureBone*) calloc(game_object.armature.bone_count, sizeof(ArmatureBone));

					for (u32 bone_idx = 0; bone_idx < game_object.armature.bone_count; ++bone_idx)
					{
						auto flatbuffer_bone = flatbuffer_bones->Get(bone_idx);
						if (!flatbuffer_bone)
						{
							continue;
						}

						game_object.armature.bones[bone_idx] = {
							.name = copy_flatbuffer_string(flatbuffer_bone->name()),
							.parent_index = flatbuffer_bone->parent_index(),
							.inverse_bind_matrix = flatbuffer_helpers::to_hmm_mat4(flatbuffer_bone->inverse_bind_matrix()),
						};
					}
				}

				if (auto flatbuffer_animations = object_armature->animations())
				{
					game_object.armature.animation_count = flatbuffer_animations->size();
					game_object.armature.animations = (AnimationClip*) calloc(game_object.armature.animation_count, sizeof(AnimationClip));

					for (u32 animation_idx = 0; animation_idx < game_object.armature.animation_count; ++animation_idx)
					{
						auto flatbuffer_animation = flatbuffer_animations->Get(animation_idx);
						if (!flatbuffer_animation)
						{
							continue;
						}

						AnimationClip& animation = game_object.armature.animations[animation_idx];
						animation.name = copy_flatbuffer_string(flatbuffer_animation->name());
						animation.frame_rate = flatbuffer_animation->frame_rate();
						animation.duration_seconds = flatbuffer_animation->duration_seconds();
						animation.frame_count = flatbuffer_animation->frame_count();
						animation.bone_count = flatbuffer_animation->bone_count();

						const i32 matrix_count = MAX(0, animation.frame_count * animation.bone_count);
						scene_update.stats.animation_matrix_count += matrix_count;
						if (matrix_count > 0)
						{
							animation.skin_matrices = (HMM_Mat4*) malloc(sizeof(HMM_Mat4) * matrix_count);
							for (i32 matrix_idx = 0; matrix_idx < matrix_count; ++matrix_idx)
							{
								animation.skin_matrices[matrix_idx] = HMM_M4D(1.0f);
							}

							if (auto flatbuffer_skin_matrices = flatbuffer_animation->skin_matrices())
							{
								const i32 available_float_count = (i32) flatbuffer_skin_matrices->size();
								for (i32 matrix_idx = 0; matrix_idx < matrix_count; ++matrix_idx)
								{
									const i32 base_float_idx = matrix_idx * 16;
									if (base_float_idx + 15 >= available_float_count)
									{
										break;
									}

									for (i32 col = 0; col < 4; ++col)
									{
										for (i32 row = 0; row < 4; ++row)
										{
											animation.skin_matrices[matrix_idx].Elements[col][row] = flatbuffer_skin_matrices->Get(base_float_idx + col * 4 + row);
										}
									}
								}
							}
						}
					}
				}
			}

			if (auto object_light = object->light())
			{
				LightType light_type = (LightType) object_light->type();

				game_object.has_light = true;
				game_object.light = (Light){
					.type = light_type,
					.color = flatbuffer_helpers::to_hmm_vec3(object_light->color()),
				};

				switch (game_object.light.type)
				{
					case LightType::Point:
					{
						auto point_light = object_light->point_light();
						assert(point_light);
						game_object.light.point = (PointLight) {
							.power = point_light->power(),
						};
						break;
					}
					case LightType::Spot:
					{
						auto spot_light = object_light->spot_light();
						assert(spot_light);
						game_object.light.spot = (SpotLight) {
							.power = spot_light->power(),
							.beam_angle = spot_light->beam_angle(),
							.edge_blend = spot_light->edge_blend(),
						};
						break;
					}
					case LightType::Sun:
					{
						auto sun_light = object_light->sun_light();
						assert(sun_light);
						game_object.light.sun = (SunLight) {
							.power = sun_light->power(),
							.cast_shadows = sun_light->cast_shadows(),
						};
						break;
					}
					case LightType::Area:
					{
						break;
					}
					default:
						printf("\t\tUnsupported Light Type\n");
						exit(0);
				}
			}

			if (auto object_rigid_body = object->rigid_body())
			{
				game_object.has_rigid_body = true;
				game_object.rigid_body = (RigidBody) {
					.is_dynamic = object_rigid_body->is_dynamic(),
					.mass = object_rigid_body->mass(),
					.jolt_body = nullptr,	// created at drain on the main thread
				};
			}

			// Custom gameplay components we've specified on our blender objects
			if (auto object_components = object->components())
			{
				u32 num_components = object_components->size();
				for (u32 component_idx = 0; component_idx < num_components; ++component_idx)
				{
					auto component_container = object_components->Get(component_idx);
					if (!component_container) { continue; }
					auto component = component_container->value();
					if (!component) { continue; }

					auto component_type = component_container->value_type();
					switch (component_type)
					{
						case Blender::LiveLink::GameplayComponent_GameplayComponentCharacter:
						{
							using Blender::LiveLink::GameplayComponentCharacter;
							const GameplayComponentCharacter* character_component = reinterpret_cast<const GameplayComponentCharacter*>(component);

							// Settings only here — the Jolt character is
							// created at drain on the main thread
							game_object.has_character = true;
							game_object.character = (Character) {
								.settings = {
									.initial_location = game_object.current_transform.location,
									.initial_rotation = game_object.current_transform.rotation,
									.player_controlled = character_component->player_controlled(),
									.move_speed = character_component->move_speed(),
									.jump_speed = character_component->jump_speed(),
								},
							};
							break;
						}
						case Blender::LiveLink::GameplayComponent_GameplayComponentFogController:
						{
							using Blender::LiveLink::GameplayComponentFogController;
							const GameplayComponentFogController* fog_component = reinterpret_cast<const GameplayComponentFogController*>(component);

							game_object.has_fog_controller = true;
							const HMM_Vec3 fog_color = fog_component->fog_color()
								? flatbuffer_helpers::to_hmm_vec3(fog_component->fog_color())
								: HMM_V3(0.55f, 0.65f, 0.75f);
							game_object.fog_controller = (FogController) {
								.enabled = fog_component->enabled(),
								.density = fog_component->density(),
								.base_height = fog_component->base_height(),
								.scale_height = fog_component->scale_height(),
								.max_distance = fog_component->max_distance(),
								.ceiling_enabled = fog_component->ceiling_enabled(),
								.ceiling_height = fog_component->ceiling_height(),
								.ceiling_fade = fog_component->ceiling_fade(),
								.fog_color = fog_color,
								.ambient_intensity = fog_component->ambient_intensity(),
								.sun_intensity = fog_component->sun_intensity(),
								.anisotropy = fog_component->anisotropy(),
							};
							break;
						}
						case Blender::LiveLink::GameplayComponent_GameplayComponentCameraControl:
						{
							using Blender::LiveLink::GameplayComponentCameraControl;
							const GameplayComponentCameraControl* cam_control_component = reinterpret_cast<const GameplayComponentCameraControl*>(component);

							const f32 cam_control_follow_distance = cam_control_component->follow_distance();
							const f32 cam_control_follow_speed = cam_control_component->follow_speed();

							HMM_Vec3 initial_location = camera_control_get_desired_location(
								game_object.current_transform.location.XYZ,
								quat_forward(game_object.current_transform.rotation),
								cam_control_follow_distance
							);
							HMM_Vec3 object_location = game_object.current_transform.location.XYZ;
							HMM_Vec3 initial_direction = HMM_NormV3(object_location - initial_location);

							CameraControlSettings cam_control_settings = {
								.initial_location = initial_location,
								.initial_direction = initial_direction,
								.follow_distance = cam_control_follow_distance,
								.follow_speed = cam_control_follow_speed,
							};
							object_add_camera_control(game_object, cam_control_settings);

							state.scene.camera_control_id = game_object.unique_id;
							break;
						}
						default:
							// Character, fog, etc. return with their systems
							break;
					}
				}
			}

			scene_update.objects.add(game_object);
		}

		state.runtime.blender_data_loaded = true;
	}

	if (auto deleted_object_uids = update->deleted_object_uids())
	{
		scene_update.stats.deleted_object_count = (i32) deleted_object_uids->size();
		for (i32 deleted_object_uid : *deleted_object_uids)
		{
			scene_update.deleted_object_uids.add(deleted_object_uid);
		}
	}

	if (update->reset())
	{
		scene_update.reset = true;
	}

	for (const Object& object : scene_update.objects)
	{
		if (object.has_mesh)
		{
			scene_update.stats.mesh_count++;
			scene_update.stats.mesh_vertex_count += (i32) object.mesh.vertex_count;
			scene_update.stats.mesh_index_count += (i32) object.mesh.index_count;
			if (object.mesh.has_skinned_vertices) { scene_update.stats.skinned_mesh_count++; }
		}
		if (object.has_light) { scene_update.stats.light_count++; }
		if (object.has_armature)
		{
			scene_update.stats.armature_count++;
			scene_update.stats.animation_count += (i32) object.armature.animation_count;
		}
	}

	// Send the whole update to the main thread
	state.live_link.scene_updates.send(scene_update);
}

// Live Link Function. Runs on its own thread
void live_link_thread_function()
{
	socket_lib_init();

	// Init socket we'll use to talk to blender
	struct addrinfo hints, *res;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	const char* HOST = "127.0.0.1";

	// getaddrinfo can transiently fail (system resolver hiccups) — retry
	// instead of dereferencing a null result
	res = nullptr;
	while (state.runtime.game_running)
	{
		const int getaddrinfo_result = getaddrinfo(HOST, state.live_link.port.c_str(), &hints, &res);
		if (getaddrinfo_result == 0 && res != nullptr)
		{
			break;
		}

		printf("live link: getaddrinfo failed (%s), retrying\n", gai_strerror(getaddrinfo_result));
		res = nullptr;
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	if (res == nullptr)
	{
		return;	// shutting down before the resolver ever succeeded
	}

	// make a socket
	state.live_link.blender_socket = socket_open(res->ai_family, res->ai_socktype, res->ai_protocol);

	// Allow us to reuse address and port
	socket_set_reuse_addr_and_port(state.live_link.blender_socket, true);

	// bind our socket
	SOCKET_OP(bind(state.live_link.blender_socket, res->ai_addr, res->ai_addrlen));

	const i32 backlog = 1;
	SOCKET_OP(listen(state.live_link.blender_socket, backlog));

	// accept connections from blender
	struct sockaddr_storage their_addr;
	socklen_t addr_size = sizeof their_addr;
	do
	{
		state.live_link.connection_socket = accept(state.live_link.blender_socket, (struct sockaddr *) &their_addr, &addr_size);
	}
	while (!socket_is_valid(state.live_link.connection_socket) && state.runtime.game_running);

	// set recv timeout
	struct timeval recv_timeout = {
		.tv_sec = 1,
		.tv_usec = 0
	};
	socket_set_recv_timeout(state.live_link.connection_socket, recv_timeout);

	// infinite recv loop
	while (state.runtime.game_running)
	{
		StretchyBuffer<u8> flatbuffer_data;

		int current_bytes_read = 0;
		int total_bytes_read = 0;
		int packets_read = 0;
		optional<flatbuffers::uoffset_t> flatbuffer_size;
		do
		{
			const size_t buffer_len = 4096;
			u8 buffer[buffer_len];
			const int flags = 0;
			current_bytes_read = socket_recv(state.live_link.connection_socket, buffer, buffer_len, flags);

			// Less than zero is an error
			if (current_bytes_read < 0)
			{
				int last_error = socket_get_last_error();
				if (	last_error == socket_error_again()
					||	last_error == socket_error_would_block()
					||	last_error == socket_error_timed_out())
				{
					current_bytes_read = 0;
					continue;
				}
				else
				{
					printf("recv_error: %i\n", last_error);
					exit(0);
				}
			}

			// No bytes read this iteration. Try again
			if (current_bytes_read == 0)
			{
				continue;
			}

			// current_bytes_read > 0, we've got data!
			if (current_bytes_read > 0)
			{
				// Flatbuffer size will be prefixed to flatbuffer data. Set it when we encounter it
				if (!flatbuffer_size)
				{
					assert(current_bytes_read >= sizeof(flatbuffers::uoffset_t));
					flatbuffer_size = *(flatbuffers::uoffset_t*)(buffer);
				}

				total_bytes_read += current_bytes_read;
				i32 next_idx = flatbuffer_data.length();
				flatbuffer_data.add_uninitialized(current_bytes_read);
				memcpy(&flatbuffer_data[next_idx], buffer, current_bytes_read);
				++packets_read;
			}
		}
		while (state.runtime.game_running && (current_bytes_read == 0 || (flatbuffer_size && total_bytes_read < flatbuffer_size.value())));

		printf("We've got some data! Data Length: %td Packets Read: %i\n", flatbuffer_data.length(), packets_read);

		parse_flatbuffer_data(flatbuffer_data);
	}

	printf("Shutting down sockets\n");

	socket_close(state.live_link.connection_socket);
	socket_close(state.live_link.blender_socket);

	socket_lib_quit();
}

// Registers one image (main thread): creates + uploads the GPU image backing
// a bindless array slot (port of game/src/main.cpp:264-309). Takes ownership
// of pending.pixels (frees in all paths).
void register_image(const PendingImage& in_pending)
{
	if (state.images.id_to_index.contains(in_pending.unique_id))
	{
		free(in_pending.pixels);
		return;
	}

	if (state.images.items.length() >= MAX_BINDLESS_IMAGES)
	{
		printf("Exceeded MAX_BINDLESS_IMAGES (%i); skipping image UID %i\n", MAX_BINDLESS_IMAGES, in_pending.unique_id);
		free(in_pending.pixels);
		return;
	}

	// UNORM (not SRGB): the addon sends linear-encoded bytes (Blender
	// image.pixels are scene-linear floats * 255) — game/ parity. Banding in
	// darks is the known tradeoff until the addon sends sRGB-encoded data.
	GpuImage image = gpu_image_create_from_data(
		&state.vk,
		(u32) in_pending.width,
		(u32) in_pending.height,
		VK_FORMAT_R8G8B8A8_UNORM,
		in_pending.pixels,
		(u64) in_pending.width * (u64) in_pending.height * 4
	);
	free(in_pending.pixels);

	state.images.id_to_index[in_pending.unique_id] = (i32) state.images.items.length();
	state.images.items.add(image);
}

// Images are only destroyed on scene reset (no individual removal — game/
// parity). The deletion queue is buffers-only, so idle the device first.
void reset_images()
{
	if (state.images.items.length() > 0)
	{
		VK_CHECK(vulkan_device_wait_idle(&state.vk));
		for (GpuImage& image : state.images.items)
		{
			ImGuiLayer::unregister_texture(image.view);
			for (VkImageView layer_view : image.layer_views)
			{
				ImGuiLayer::unregister_texture(layer_view);
			}
			gpu_image_destroy(state.vk.allocator, state.vk.device, image);
		}
	}
	state.images.items.reset();
	state.images.id_to_index.clear();
}

// Registers one material (main thread). Returns true when it was newly
// appended (port of game/src/main.cpp:320-384). Updates to an already
// registered id are ignored until a scene reset — game/ parity.
bool register_material(const PendingMaterial& in_pending)
{
	if (state.materials.id_to_index.contains(in_pending.unique_id))
	{
		return false;
	}

	if (state.materials.items.length() >= MAX_MATERIALS)
	{
		printf("Exceeded MAX_MATERIALS (%i)\n", MAX_MATERIALS);
		exit(0);
	}

	Material material = {
		.base_color = in_pending.base_color,
		.emission_color = in_pending.emission_color,
		.metallic = in_pending.metallic,
		.roughness = in_pending.roughness,
		.emission_strength = in_pending.emission_strength,
		.base_color_image_index = -1,
		.emission_color_image_index = -1,
		.metallic_image_index = -1,
		.roughness_image_index = -1,
	};

	// Resolve image ids -> bindless indices (images in the same update were
	// registered just before materials; id 0 = no image, game/'s `> 0` guard)
	auto resolve_image_index = [](i32 in_image_id) -> i32
	{
		if (in_image_id <= 0)
		{
			return -1;
		}
		auto found = state.images.id_to_index.find(in_image_id);
		assert(found != state.images.id_to_index.end());
		return found->second;
	};
	material.base_color_image_index = resolve_image_index(in_pending.base_color_image_id);
	material.emission_color_image_index = resolve_image_index(in_pending.emission_color_image_id);
	material.metallic_image_index = resolve_image_index(in_pending.metallic_image_id);
	material.roughness_image_index = resolve_image_index(in_pending.roughness_image_id);

	state.materials.id_to_index[in_pending.unique_id] = (i32) state.materials.items.length();
	state.materials.items.add(material);
	return true;
}

void reset_materials()
{
	state.materials.id_to_index.clear();
	state.materials.items.reset();
	// GPU buffer deliberately kept alive (bound every frame); stale contents
	// are unreachable once id_to_index is empty
}

// Maps a mesh's raw material IDS (from the wire) to registered indices
void resolve_mesh_material_indices(Mesh& in_mesh)
{
	for (u32 material_idx = 0; material_idx < in_mesh.material_indices_count; ++material_idx)
	{
		const i32 material_id = in_mesh.material_indices[material_idx];
		in_mesh.material_indices[material_idx] = -1;

		auto found = state.materials.id_to_index.find(material_id);
		if (found == state.materials.id_to_index.end())
		{
			printf("\tFailed to find material with id: %i\n", material_id);
			continue;
		}
		in_mesh.material_indices[material_idx] = found->second;
	}
}

// Drains SceneUpdate messages on the main thread. GPU buffer destruction for
// replaced/deleted objects routes through the deletion queue, so this is safe
// while frames are in flight. Per-message order mirrors game/'s parse order:
// images -> materials -> objects -> deleted -> reset.
void live_link_drain_channels()
{
	while (optional<SceneUpdate> received_update = state.live_link.scene_updates.receive())
	{
		SceneUpdate& scene_update = *received_update;
		State::DataOrientedState::LiveLinkImportStats import_stats;
		static_cast<SceneUpdate::ImportStats&>(import_stats) = scene_update.stats;
		import_stats.update_index = state.data_oriented.last_import.update_index + 1;
		state.data_oriented.last_import = import_stats;
		state.data_oriented.import_history.add(import_stats);
		state.data_oriented.selected_import_history_index = (i32) state.data_oriented.import_history.length() - 1;

		// Images (before materials — register_material resolves image ids)
		for (const PendingImage& pending_image : scene_update.images)
		{
			register_image(pending_image);
		}

		// Materials
		{
			bool materials_updated = false;
			for (const PendingMaterial& pending_material : scene_update.materials)
			{
				materials_updated = register_material(pending_material) || materials_updated;
			}
			if (materials_updated)
			{
				update_materials_buffer(state);
			}
		}

		// Updated objects
		for (Object& updated_object : scene_update.objects)
		{
			state.data_oriented.frame.live_link_updated_objects += 1;
			i32 updated_object_uid = updated_object.unique_id;

			printf("Updating Object. UID: %i\n", updated_object_uid);

			if (updated_object.has_mesh)
			{
				resolve_mesh_material_indices(updated_object.mesh);
			}

			bool gi_scene_geometry_changed = object_contributes_to_gi_scene(updated_object);

			// Cleanup old object
			if (state.scene.objects.contains(updated_object_uid))
			{
				gi_scene_geometry_changed = gi_scene_geometry_changed
					|| object_contributes_to_gi_scene(state.scene.objects[updated_object_uid]);
				object_cleanup(state.scene.objects[updated_object_uid]);
			}

			// Create the Jolt body on the drained copy BEFORE the map insert
			// (the JPH::Body* copies into the map — game/ main.cpp:2010)
			if (updated_object.has_rigid_body)
			{
				object_add_jolt_body(updated_object);
			}

			// Finalize the character here too (parse only fills settings —
			// Jolt creation is main-thread; deviation from game/)
			if (updated_object.has_character)
			{
				updated_object.character = character_create(jolt_state, updated_object.character.settings);
				if (updated_object.character.settings.player_controlled)
				{
					state.scene.player_character_id = updated_object_uid;
				}
			}

			if (updated_object.has_light)
			{
				mark_lighting_dirty(state);
			}

			state.scene.objects[updated_object_uid] = updated_object;
			scene_mark_indexes_dirty(state);
			if (gi_scene_geometry_changed)
			{
				state.gi.layout_dirty = true;
			}
		}

		// Deleted objects
		for (i32 deleted_object_uid : scene_update.deleted_object_uids)
		{
			state.data_oriented.frame.live_link_deleted_objects += 1;
			if (state.scene.objects.contains(deleted_object_uid))
			{
				printf("Removing object. UID: %i\n", deleted_object_uid);
				if (state.scene.objects[deleted_object_uid].has_light)
				{
					mark_lighting_dirty(state);
				}
				const bool gi_scene_geometry_changed = object_contributes_to_gi_scene(state.scene.objects[deleted_object_uid]);
				object_cleanup(state.scene.objects[deleted_object_uid]);
				state.scene.objects.erase(deleted_object_uid);
				scene_mark_indexes_dirty(state);
				if (gi_scene_geometry_changed)
				{
					state.gi.layout_dirty = true;
				}

				if (state.scene.camera_control_id == deleted_object_uid)
				{
					state.scene.camera_control_id.reset();
				}
				if (state.scene.player_character_id == deleted_object_uid)
				{
					state.scene.player_character_id.reset();
				}
			}
		}

		// Reset
		if (scene_update.reset)
		{
			state.data_oriented.frame.live_link_reset_count += 1;
			state.runtime.blender_data_loaded = false;

			for (auto& [unique_id, object] : state.scene.objects)
			{
				object_cleanup(object);
			}

			state.scene.objects.clear();
			state.scene.camera_control_id.reset();
			state.scene.primary_sun_id.reset();
			state.scene.player_character_id.reset();
			state.fog.active_fog_controller_id.reset();
			state.fog.active = false;
			mark_lighting_dirty(state);
			scene_reset_indexes(state);
			reset_materials();
			reset_images();
			state.gi.layout_dirty = true;
		}
	}
}

// Derives the internal render size from the window size and resolution
// percentage (port of game/src/main.cpp:1356-1370)
void update_render_resolution()
{
	state.window.resolution_percentage = CLAMP(
		state.window.resolution_percentage,
		MIN_RENDER_RESOLUTION_PERCENTAGE,
		MAX_RENDER_RESOLUTION_PERCENTAGE
	);

	const i32 source_width = state.window.width > 0 ? state.window.width : 1920;
	const i32 source_height = state.window.height > 0 ? state.window.height : 1080;
	const f32 scale = (f32) state.window.resolution_percentage / 100.0f;

	state.window.render_width = MAX(1, (i32)(source_width * scale + 0.5f));
	state.window.render_height = MAX(1, (i32)(source_height * scale + 0.5f));
}

// Resizes all pass targets when the framebuffer size changed (port of
// game/src/main.cpp:1372-1400). Swapchain-type passes track the window;
// everything else tracks the scaled render resolution.
void handle_resize(bool in_force = false)
{
	const i32 framebuffer_width = (i32) state.vk.swapchain_extent.width;
	const i32 framebuffer_height = (i32) state.vk.swapchain_extent.height;

	if (!in_force && framebuffer_width == state.window.width && framebuffer_height == state.window.height)
	{
		return;
	}

	state.window.width = framebuffer_width;
	state.window.height = framebuffer_height;
	update_render_resolution();

	// Pass targets are destroyed immediately (the deletion queue handles
	// buffers only) — idle covers render-scale changes; the window-resize
	// path already idled inside recreate_swapchain
	VK_CHECK(vulkan_device_wait_idle(&state.vk));
	ImGuiLayer::handle_swapchain_recreated(&state.vk);
	ImGuiLayer::clear_textures();

	for (i32 pass_index = 0; pass_index < (i32) ERenderPass::COUNT; ++pass_index)
	{
		RenderPassEntry& entry = state.render_passes.passes[pass_index];
		if (entry.final_pass().desc.type == ERenderPassType::Swapchain)
		{
			entry.handle_resize(state.window.width, state.window.height);
		}
		else
		{
			entry.handle_resize(state.window.render_width, state.window.render_height);
		}
	}

	// The TAA history targets were just recreated
	state.temporal_aa.history_valid = false;
}

// Camera-relative WASD player movement, Shift sprint, Space jump
// (port of game/src/main.cpp:1811-1880). Disabled while the debug camera is
// active or no player-controlled character exists.
void update_player_character_control(f32 in_delta_time)
{
	if (!state.scene.player_character_id || state.debug_camera.active)
	{
		return;
	}

	if (!state.scene.objects.contains(*state.scene.player_character_id))
	{
		return;
	}

	const Camera& camera = get_active_camera();
	const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));

	Object& player_character_object = state.scene.objects[*state.scene.player_character_id];
	Character& player_character_state = player_character_object.character;

	HMM_Vec3 projected_cam_forward = vec3_plane_projection(camera.forward, UnitVectors::Up);
	HMM_Vec3 projected_cam_right = vec3_plane_projection(camera_right, UnitVectors::Up);

	if (HMM_LenSqrV3(projected_cam_forward) > 1.0e-6f)
	{
		projected_cam_forward = HMM_NormV3(projected_cam_forward);
	}
	else
	{
		projected_cam_forward = UnitVectors::Forward;
	}

	if (HMM_LenSqrV3(projected_cam_right) > 1.0e-6f)
	{
		projected_cam_right = HMM_NormV3(projected_cam_right);
	}
	else
	{
		projected_cam_right = HMM_NormV3(HMM_Cross(projected_cam_forward, UnitVectors::Up));
	}

	HMM_Vec3 move_direction = HMM_V3(0, 0, 0);
	if (is_key_pressed(GLFW_KEY_W))
	{
		move_direction += projected_cam_forward;
	}
	if (is_key_pressed(GLFW_KEY_S))
	{
		move_direction -= projected_cam_forward;
	}
	if (is_key_pressed(GLFW_KEY_D))
	{
		move_direction += projected_cam_right;
	}
	if (is_key_pressed(GLFW_KEY_A))
	{
		move_direction -= projected_cam_right;
	}

	if (HMM_LenSqrV3(move_direction) > 1.0f)
	{
		move_direction = HMM_NormV3(move_direction);
	}

	if (is_key_pressed(GLFW_KEY_LEFT_SHIFT))
	{
		move_direction *= 3.0f;
	}

	const bool jump = is_key_pressed(GLFW_KEY_SPACE);
	character_move(player_character_state, move_direction, jump, in_delta_time);
}

// Copies Jolt body transforms back into object transforms every frame
// (unconditional — a no-op while paused since bodies don't move;
// port of game/src/main.cpp:1800-1809)
void update_physics_backed_object_transforms()
{
	JPH::BodyInterface& body_interface = jolt_state.physics_system.GetBodyInterface();
	for (auto& [unique_id, object] : state.scene.objects)
	{
		object_copy_physics_transform(object, body_interface);
	}
}

// ---- Skinned animation (ports of game/src/main.cpp:445-575) ----

AnimationClip* armature_get_active_animation(Armature& in_armature)
{
	if (in_armature.animation_count == 0 || !in_armature.animations)
	{
		return nullptr;
	}

	in_armature.active_animation_index = CLAMP(in_armature.active_animation_index, 0, (i32) in_armature.animation_count - 1);
	return &in_armature.animations[in_armature.active_animation_index];
}

void mesh_reset_skin_matrices(Mesh& in_mesh)
{
	for (u32 matrix_idx = 0; matrix_idx < in_mesh.skin_matrix_count; ++matrix_idx)
	{
		in_mesh.skin_matrices[matrix_idx] = HMM_M4D(1.0f);
	}
}

void rewind_skinned_animations()
{
	scene_ensure_indexes(state);
	for (i32 armature_object_id : state.scene.indexes.armature_object_ids)
	{
		auto found = state.scene.objects.find(armature_object_id);
		if (found == state.scene.objects.end())
		{
			continue;
		}

		Armature& armature = found->second.armature;
		armature.playback_time = 0.0f;
		armature.current_frame = 0;
	}
}

// Advances armature playback, computes each skinned mesh's final skin
// matrices (armature_to_mesh * clip * mesh_to_armature), and packs them into
// the shared per-frame arena (mesh.skin_matrix_arena_offset). Runs before
// begin_frame — the arena ring makes the upload safe vs frames in flight.
void update_skinned_animations(f32 in_delta_time)
{
	scene_ensure_indexes(state);
	state.data_oriented.frame.animation_armature_candidates += (i32) state.scene.indexes.armature_object_ids.length();

	state.skin_matrices.items.clear();

	// Phase A: advance armature playback
	for (i32 armature_object_id : state.scene.indexes.armature_object_ids)
	{
		auto found = state.scene.objects.find(armature_object_id);
		if (found == state.scene.objects.end())
		{
			continue;
		}

		Armature& armature = found->second.armature;
		AnimationClip* animation = armature_get_active_animation(armature);
		if (!animation || animation->frame_count <= 0)
		{
			continue;
		}

		if (state.runtime.is_simulating && state.animation.is_playing && state.animation.playback_rate > 0.0f)
		{
			armature.playback_time += in_delta_time * state.animation.playback_rate;

			f32 duration = animation->duration_seconds;
			if (duration <= 0.0f && animation->frame_rate > 0.0f)
			{
				duration = (f32) animation->frame_count / animation->frame_rate;
			}
			if (duration > 0.0f)
			{
				armature.playback_time = fmodf(armature.playback_time, duration);
			}
		}

		if (animation->frame_rate > 0.0f)
		{
			armature.current_frame = CLAMP((i32)(armature.playback_time * animation->frame_rate), 0, animation->frame_count - 1);
		}
		state.data_oriented.frame.animation_armatures_updated += 1;
	}

	// Phase B: compute + pack per-mesh skin matrices
	state.data_oriented.frame.animation_skinned_mesh_candidates += (i32) state.scene.indexes.skinned_mesh_object_ids.length();
	for (i32 skinned_object_id : state.scene.indexes.skinned_mesh_object_ids)
	{
		auto found = state.scene.objects.find(skinned_object_id);
		if (found == state.scene.objects.end())
		{
			continue;
		}

		Mesh& mesh = found->second.mesh;
		mesh.skin_matrix_arena_offset = -1;
		if (!mesh.has_skinned_vertices || mesh.skin_matrix_count == 0 || !mesh.skin_matrices)
		{
			continue;
		}

		mesh_reset_skin_matrices(mesh);

		// The armature is a separate scene object referenced by id
		auto armature_found = state.scene.objects.find(mesh.armature_id);
		if (armature_found != state.scene.objects.end() && armature_found->second.has_armature)
		{
			Armature& armature = armature_found->second.armature;
			AnimationClip* animation = armature_get_active_animation(armature);
			if (animation && animation->skin_matrices && animation->frame_count > 0 && animation->bone_count > 0)
			{
				const i32 frame_idx = CLAMP(armature.current_frame, 0, animation->frame_count - 1);
				const i32 bone_count = MIN(animation->bone_count, (i32) mesh.skin_matrix_count);
				for (i32 bone_idx = 0; bone_idx < bone_count; ++bone_idx)
				{
					const HMM_Mat4& clip_matrix = animation->skin_matrices[frame_idx * animation->bone_count + bone_idx];
					mesh.skin_matrices[bone_idx] = HMM_MulM4(
						mesh.armature_to_mesh,
						HMM_MulM4(clip_matrix, mesh.mesh_to_armature)
					);
				}
			}
		}

		mesh.skin_matrix_arena_offset = (i32) state.skin_matrices.items.length();
		for (u32 matrix_idx = 0; matrix_idx < mesh.skin_matrix_count; ++matrix_idx)
		{
			state.skin_matrices.items.add(mesh.skin_matrices[matrix_idx]);
		}
		state.data_oriented.frame.animation_skin_matrix_uploads += 1;
	}

	skin_matrix_arena_upload(state);
}

// Picks the active fog controller: lowest-uid enabled+visible controller
// (selection half of game/src/main.cpp:608-666; the fs_params packing is
// Phase 3 with the fog render pass)
void refresh_active_fog_controller()
{
	const std::optional<i32> previous_id = state.fog.active_fog_controller_id;

	state.fog.active_fog_controller_id.reset();
	state.fog.active = false;

	i32 selected_uid = std::numeric_limits<i32>::max();
	for (auto& [unique_id, object] : state.scene.objects)
	{
		if (!object.has_fog_controller || !object.fog_controller.enabled || !object.visibility)
		{
			continue;
		}

		if (unique_id < selected_uid)
		{
			selected_uid = unique_id;
			state.fog.active_fog_controller_id = unique_id;
			state.fog.active = true;
		}
	}

	if (state.fog.active_fog_controller_id != previous_id)
	{
		if (state.fog.active_fog_controller_id)
		{
			printf("Active fog controller: UID %i\n", *state.fog.active_fog_controller_id);
		}
		else
		{
			printf("Active fog controller: none\n");
		}
	}
}

// Keeps state.scene.primary_sun_id pointing at a valid sun object, rescanning
// the light index when the cached id goes stale (port of game/src/main.cpp:577-606)
void refresh_primary_sun_id()
{
	if (state.scene.primary_sun_id)
	{
		auto found = state.scene.objects.find(*state.scene.primary_sun_id);
		if (found != state.scene.objects.end() && object_is_sun_light(found->second))
		{
			return;
		}
		state.scene.primary_sun_id.reset();
	}

	scene_ensure_indexes(state);
	i32 selected_uid = std::numeric_limits<i32>::max();
	for (i32 light_object_id : state.scene.indexes.light_object_ids)
	{
		auto found = state.scene.objects.find(light_object_id);
		if (found != state.scene.objects.end() && object_is_sun_light(found->second))
		{
			selected_uid = MIN(selected_uid, light_object_id);
		}
	}
	if (selected_uid != std::numeric_limits<i32>::max())
	{
		state.scene.primary_sun_id = selected_uid;
	}
}

bool ray_sphere_intersect(
	const HMM_Vec3& in_ray_origin,
	const HMM_Vec3& in_ray_direction,
	const HMM_Vec3& in_sphere_center,
	const f32 in_sphere_radius,
	f32& out_t)
{
	const HMM_Vec3 oc = in_ray_origin - in_sphere_center;
	const f32 b = HMM_DotV3(oc, in_ray_direction);
	const f32 c = HMM_DotV3(oc, oc) - in_sphere_radius * in_sphere_radius;
	const f32 discriminant = b * b - c;
	if (discriminant < 0.0f)
	{
		return false;
	}

	const f32 sqrt_discriminant = HMM_SqrtF(discriminant);
	f32 t = -b - sqrt_discriminant;
	if (t < 0.0f) { t = -b + sqrt_discriminant; }
	if (t < 0.0f) { return false; }
	out_t = t;
	return true;
}

void pick_isolated_gi_probe()
{
	i32 logical_width = 0;
	i32 logical_height = 0;
	glfwGetWindowSize(state.window.handle, &logical_width, &logical_height);
	const f32 width = (f32) logical_width;
	const f32 height = (f32) logical_height;
	if (width <= 0.0f || height <= 0.0f)
	{
		return;
	}

	const f32 fov = HMM_AngleDeg(60.0f);
	const f32 ndc_x = 2.0f * state.input.mouse_position.X / width - 1.0f;
	const f32 ndc_y = 1.0f - 2.0f * state.input.mouse_position.Y / height;
	const Camera& camera = get_active_camera();
	const HMM_Vec3 camera_forward = HMM_NormV3(camera.forward);
	const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera_forward, camera.up));
	const HMM_Vec3 camera_up = HMM_NormV3(HMM_Cross(camera_right, camera_forward));
	const f32 tan_half_fov = HMM_TanF(fov * 0.5f);
	const HMM_Vec3 ray_direction = HMM_NormV3(
		camera_forward
		+ camera_right * (ndc_x * (width / height) * tan_half_fov)
		+ camera_up * (ndc_y * tan_half_fov));

	i32 closest_probe_index = -1;
	f32 closest_t = std::numeric_limits<f32>::max();
	for (i32 probe_index = 0; probe_index < gi_scene.non_fallback_probe_count; ++probe_index)
	{
		f32 t = 0.0f;
		if (ray_sphere_intersect(
			camera.location,
			ray_direction,
			gi_scene_probe_position_from_index(gi_scene, probe_index),
			gi_scene_debug_probe_radius_for_probe(gi_scene, probe_index),
			t) && t < closest_t)
		{
			closest_t = t;
			closest_probe_index = probe_index;
		}
	}
	if (closest_probe_index >= 0)
	{
		state.gi.isolated_probe_index = closest_probe_index;
	}
}

void key_callback(GLFWwindow* in_window, i32 in_key, i32 in_scancode, i32 in_action, i32 in_mods)
{
	#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	if (ImGuiLayer::initialized)
	{
		ImGui_ImplGlfw_KeyCallback(in_window, in_key, in_scancode, in_action, in_mods);
	}
	#endif
	if (in_key < 0 || in_key > GLFW_KEY_LAST)
	{
		return;
	}

	if (in_action == GLFW_PRESS)
	{
		if (in_key == GLFW_KEY_ESCAPE)
		{
			if (in_mods & GLFW_MOD_SHIFT)
			{
				set_mouse_locked(false);
			}
			else
			{
				glfwSetWindowShouldClose(in_window, GLFW_TRUE);
			}
		}

		state.input.keycodes[in_key] = true;
	}
	else if (in_action == GLFW_RELEASE)
	{
		state.input.keycodes[in_key] = false;
	}
}

void mouse_button_callback(GLFWwindow* in_window, i32 in_button, i32 in_action, i32 in_mods)
{
	#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	if (ImGuiLayer::initialized)
	{
		ImGui_ImplGlfw_MouseButtonCallback(in_window, in_button, in_action, in_mods);
		if (ImGui::GetIO().WantCaptureMouse) { return; }
	}
	#endif
	// Lock Mouse on left click into game space
	if (in_button == GLFW_MOUSE_BUTTON_LEFT && in_action == GLFW_PRESS)
	{
		if (state.gi.probe_isolation_enable && state.gi.show_probes)
		{
			pick_isolated_gi_probe();
			return;
		}
		if (!is_mouse_locked())
		{
			set_mouse_locked(true);
		}
	}
}

void cursor_position_callback(GLFWwindow* in_window, f64 in_x, f64 in_y)
{
	#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	if (ImGuiLayer::initialized)
	{
		ImGui_ImplGlfw_CursorPosCallback(in_window, in_x, in_y);
	}
	#endif
	static f64 last_x = 0.0;
	static f64 last_y = 0.0;
	static bool has_last_position = false;

	if (has_last_position)
	{
		state.input.mouse_delta.X += (f32)(in_x - last_x);
		state.input.mouse_delta.Y += (f32)(in_y - last_y);
	}

	last_x = in_x;
	last_y = in_y;
	state.input.mouse_position = HMM_V2((f32) in_x, (f32) in_y);
	has_last_position = true;
}

void char_callback(GLFWwindow* in_window, u32 in_codepoint)
{
	#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	if (ImGuiLayer::initialized) { ImGui_ImplGlfw_CharCallback(in_window, in_codepoint); }
	#endif
}

void scroll_callback(GLFWwindow* in_window, f64 in_x_offset, f64 in_y_offset)
{
	#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	if (ImGuiLayer::initialized) { ImGui_ImplGlfw_ScrollCallback(in_window, in_x_offset, in_y_offset); }
	#endif
}

void cursor_enter_callback(GLFWwindow* in_window, i32 in_entered)
{
	#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	if (ImGuiLayer::initialized) { ImGui_ImplGlfw_CursorEnterCallback(in_window, in_entered); }
	#endif
}

void window_focus_callback(GLFWwindow* in_window, i32 in_focused)
{
	#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	if (ImGuiLayer::initialized) { ImGui_ImplGlfw_WindowFocusCallback(in_window, in_focused); }
	#endif
}

void framebuffer_size_callback(GLFWwindow* in_window, i32 in_width, i32 in_height)
{
	// The swapchain owns the authoritative framebuffer extent. Keep the old
	// values here so handle_resize() can detect the change after recreation
	// and resize all scaled offscreen targets exactly once.
	state.vk.needs_resize = true;
}

void glfw_error_callback(i32 in_error, const char* in_description)
{
	fprintf(stderr, "GLFW error %i: %s\n", in_error, in_description ? in_description : "(no description)");
}

void update_debug_camera(f32 in_delta_time)
{
	if (!state.debug_camera.active || is_key_pressed(GLFW_KEY_LEFT_CONTROL))
	{
		return;
	}

	Camera& camera = state.debug_camera.camera;
	const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));

	f32 move_speed = 10.0f * in_delta_time;
	if (is_key_pressed(GLFW_KEY_LEFT_SHIFT))
	{
		move_speed *= 5.0f;
	}

	if (is_key_pressed(GLFW_KEY_W) || is_key_pressed(GLFW_KEY_UP))
	{
		camera.location += camera.forward * move_speed;
	}
	if (is_key_pressed(GLFW_KEY_S) || is_key_pressed(GLFW_KEY_DOWN))
	{
		camera.location -= camera.forward * move_speed;
	}
	if (is_key_pressed(GLFW_KEY_D) || is_key_pressed(GLFW_KEY_RIGHT))
	{
		camera.location += camera_right * move_speed;
	}
	if (is_key_pressed(GLFW_KEY_A) || is_key_pressed(GLFW_KEY_LEFT))
	{
		camera.location -= camera_right * move_speed;
	}
	if (is_key_pressed(GLFW_KEY_E))
	{
		camera.location += camera.up * move_speed;
	}
	if (is_key_pressed(GLFW_KEY_Q))
	{
		camera.location -= camera.up * move_speed;
	}

	if (is_mouse_locked())
	{
		const f32 look_speed = 1.0f * in_delta_time;
		const HMM_Vec2 mouse_delta = get_mouse_delta();

		camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera.up, -mouse_delta.X * look_speed));
		camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera_right, -mouse_delta.Y * look_speed));
	}
}

// Player camera control: mouse-orbit + follow the controlled object
// (port of game/src/main.cpp:2725-2763)
void update_camera_control(f32 in_delta_time)
{
	if (!state.scene.camera_control_id || state.debug_camera.active)
	{
		return;
	}

	Object& camera_control_object = state.scene.objects[*state.scene.camera_control_id];
	CameraControl& camera_control = camera_control_object.camera_control;
	Camera& camera = camera_control.camera;

	if (is_mouse_locked())
	{
		//FCS TODO: Add max angle property (angle above XY plane) that we can rotate camera
		//FCS TODO: Add rotation speed property to camera control component
		const f32 look_speed = 1.0f * in_delta_time;
		const HMM_Vec2 mouse_delta = get_mouse_delta();

		// Get current target at old forward vector
		const HMM_Vec3 camera_old_target = camera.location + camera.forward * camera_control.follow_distance;

		const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));
		camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera.up, -mouse_delta.X * look_speed));
		camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera_right, -mouse_delta.Y * look_speed));

		// Use old target and new forward vector to get our rotated desired location
		camera_control.camera.location = camera_control_get_desired_location(
			camera_old_target,
			camera.forward,
			camera_control.follow_distance
		);
	}

	HMM_Vec3 desired_location = camera_control_get_desired_location(
		camera_control_object.current_transform.location.XYZ,
		camera.forward,
		camera_control.follow_distance
	);
	camera_control.camera.location = HMM_LerpV3(
		camera.location,
		HMM_Clamp(0.0f, camera_control.follow_speed * in_delta_time, 1.0f),
		desired_location
	);
}

void frame(f32 in_delta_time)
{
	CPU_TIMING_FRAME("Frame");
	data_oriented_begin_frame(state);

	state.debug_ui.immediate_frame_time_ms = in_delta_time * 1000.0f;
	state.debug_ui.immediate_fps = in_delta_time > 0.0f ? 1.0f / in_delta_time : 0.0f;

	f64 immediate_cpu_time_ms = 0.0;
	state.debug_ui.immediate_cpu_time_valid = cpu_timings_get_latest_frame_total_ms(immediate_cpu_time_ms);
	if (state.debug_ui.immediate_cpu_time_valid)
	{
		state.debug_ui.immediate_cpu_time_ms = (f32) immediate_cpu_time_ms;
		state.debug_ui.cpu_time_sample_sum_ms += immediate_cpu_time_ms;
		state.debug_ui.cpu_time_sample_count += 1;
		if (!state.debug_ui.cpu_time_valid)
		{
			state.debug_ui.cpu_time_ms = (f32) immediate_cpu_time_ms;
			state.debug_ui.cpu_time_valid = true;
		}
	}

	f64 immediate_gpu_time_ms = 0.0;
	bool immediate_gpu_time_pending = false;
	state.debug_ui.immediate_gpu_time_valid = gpu_timings_get_latest_completed_frame_total_ms(immediate_gpu_time_ms, immediate_gpu_time_pending);
	state.debug_ui.immediate_gpu_time_pending = !state.debug_ui.immediate_gpu_time_valid && immediate_gpu_time_pending;
	if (state.debug_ui.immediate_gpu_time_valid)
	{
		state.debug_ui.immediate_gpu_time_ms = (f32) immediate_gpu_time_ms;
		state.debug_ui.gpu_time_sample_sum_ms += immediate_gpu_time_ms;
		state.debug_ui.gpu_time_sample_count += 1;
		if (!state.debug_ui.gpu_time_valid)
		{
			state.debug_ui.gpu_time_ms = (f32) immediate_gpu_time_ms;
			state.debug_ui.gpu_time_valid = true;
			state.debug_ui.gpu_time_pending = false;
		}
	}
	else if (state.debug_ui.immediate_gpu_time_pending)
	{
		state.debug_ui.gpu_time_pending = !state.debug_ui.gpu_time_valid;
	}
	else
	{
		state.debug_ui.gpu_time_valid = false;
		state.debug_ui.gpu_time_pending = false;
	}

	state.debug_ui.stats_sample_elapsed += in_delta_time;
	state.debug_ui.stats_sample_count += 1;
	if (state.debug_ui.fps == 0.0f && in_delta_time > 0.0f)
	{
		state.debug_ui.frame_time_ms = in_delta_time * 1000.0f;
		state.debug_ui.fps = 1.0f / in_delta_time;
	}
	if (state.debug_ui.stats_sample_elapsed >= 0.25)
	{
		const f64 average_delta_time = state.debug_ui.stats_sample_elapsed / state.debug_ui.stats_sample_count;
		state.debug_ui.frame_time_ms = (f32) (average_delta_time * 1000.0);
		state.debug_ui.fps = (f32) (state.debug_ui.stats_sample_count / state.debug_ui.stats_sample_elapsed);
		if (state.debug_ui.cpu_time_sample_count > 0)
		{
			state.debug_ui.cpu_time_ms = (f32) (state.debug_ui.cpu_time_sample_sum_ms / state.debug_ui.cpu_time_sample_count);
			state.debug_ui.cpu_time_valid = true;
		}
		if (state.debug_ui.gpu_time_sample_count > 0)
		{
			state.debug_ui.gpu_time_ms = (f32) (state.debug_ui.gpu_time_sample_sum_ms / state.debug_ui.gpu_time_sample_count);
			state.debug_ui.gpu_time_valid = true;
			state.debug_ui.gpu_time_pending = false;
		}
		else if (!state.debug_ui.gpu_time_valid)
		{
			state.debug_ui.gpu_time_pending = state.debug_ui.immediate_gpu_time_pending;
		}
		state.debug_ui.stats_sample_elapsed = 0.0;
		state.debug_ui.stats_sample_count = 0;
		state.debug_ui.cpu_time_sample_sum_ms = 0.0;
		state.debug_ui.cpu_time_sample_count = 0;
		state.debug_ui.gpu_time_sample_sum_ms = 0.0;
		state.debug_ui.gpu_time_sample_count = 0;
	}
	ImGuiLayer::begin_frame();

	{
		CPU_TIMING_SCOPE("Live Link");
		live_link_drain_channels();
	}

	{
		CPU_TIMING_SCOPE("Camera + Controls");
		DEFINE_TOGGLE_TWO_KEYS(state.debug_ui.visible, GLFW_KEY_I, GLFW_KEY_LEFT_CONTROL);
		#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
		const bool ui_captures_keyboard = ImGui::GetIO().WantCaptureKeyboard;
		const bool ui_captures_mouse = ImGui::GetIO().WantCaptureMouse;
		#else
		const bool ui_captures_keyboard = false;
		const bool ui_captures_mouse = false;
		#endif

		// Space + Left Control toggle simulation (physics + animation playback)
		if (!ui_captures_keyboard)
		{
			DEFINE_TOGGLE_TWO_KEYS(state.runtime.is_simulating, GLFW_KEY_SPACE, GLFW_KEY_LEFT_CONTROL);

		// D + Left Control toggle debug camera
		DEFINE_EVENT_TWO_KEYS(GLFW_KEY_D, GLFW_KEY_LEFT_CONTROL,
			if (!state.debug_camera.active) state.debug_camera.camera = get_active_camera();
			state.debug_camera.active = !state.debug_camera.active;
		);

		// R + Left Control: restore initial transforms (BEFORE body reset —
		// the body rebuilds from current_transform), reset physics bodies,
		// rewind animations
		DEFINE_EVENT_TWO_KEYS(GLFW_KEY_R, GLFW_KEY_LEFT_CONTROL,
			for (auto& [reset_uid, reset_object] : state.scene.objects)
			{
				reset_object.current_transform = reset_object.initial_transform;
				if (reset_object.has_rigid_body && reset_object.rigid_body.jolt_body != nullptr)
				{
					object_reset_jolt_body(reset_object);
				}
			}
			rewind_skinned_animations();
		);
		}

		if (!ui_captures_mouse && !ui_captures_keyboard)
		{
			update_debug_camera(in_delta_time);
			update_camera_control(in_delta_time);
		}
	}

	{
		CPU_TIMING_SCOPE("Simulation");
		if (state.runtime.is_simulating)
		{
			update_player_character_control(in_delta_time);
			jolt_update(in_delta_time);
		}
	}

	{
		CPU_TIMING_SCOPE("Object Transforms");
		update_physics_backed_object_transforms();
		scene_ensure_indexes(state);
		refresh_primary_sun_id();
		refresh_active_fog_controller();
		build_render_object_snapshot(state);
		pack_lights(state);
		upload_lights(state);
	}

	{
		CPU_TIMING_SCOPE("Skinned Animation");
		update_skinned_animations(in_delta_time);
	}

	CPU_TIMING_SCOPE("Rendering");

	if (!vulkan_context_begin_frame(&state.vk))
	{
		#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
		ImGui::EndFrame();
		#endif
		reset_mouse_delta();
		return;
	}

	// Window size changes (begin_frame already recreated the swapchain)
	handle_resize();
	ImGuiLayer::draw_controls(state, gi_scene);

	// View + Projection matrix setup (TAA jitters the projection; the
	// unjittered previous VP is not kept — game/ reprojects with the
	// jittered one, main.cpp:3526)
	const Camera& camera = get_active_camera();
	const f32 fov = HMM_AngleDeg(60.0f);
	const f32 aspect_ratio = (f32) state.window.render_width / (f32) state.window.render_height;
	HMM_Mat4 projection_matrix = mat4_perspective(fov, aspect_ratio);
	if (state.temporal_aa.enable)
	{
		state.temporal_aa.jitter_phase = (i32) (state.vk.frame_number & 1);
		state.temporal_aa.current_jitter_pixels = TemporalAAPass::get_decima_jitter_pixels(state.temporal_aa.jitter_phase);
		projection_matrix = TemporalAAPass::apply_projection_jitter(
			projection_matrix,
			state.temporal_aa.current_jitter_pixels,
			HMM_V2((f32) state.window.render_width, (f32) state.window.render_height)
		);
	}
	else
	{
		state.temporal_aa.current_jitter_pixels = HMM_V2(0.0f, 0.0f);
	}

	const HMM_Vec3 target = camera.location + camera.forward * 10;
	const HMM_Mat4 view_matrix = HMM_LookAt_RH(camera.location, target, camera.up);
	const HMM_Mat4 view_projection_matrix = HMM_MulM4(projection_matrix, view_matrix);

	// Deform source vertices first, then plan/emit tessellation. GI captures
	// and all raster passes below consume the resulting MeshRenderView.
	GpuSkinning::update(&state.vk, state,
		state.tessellation.enabled || state.wireframe.shaded_wireframe);
	Tessellation::update(&state.vk, state, camera, fov);

	// Per-frame UBO. Sun sourced from the scene's primary sun; the hardcoded
	// fallback only drives the sky bake now (deviation from game/'s black
	// fallback) — geometry lighting comes solely from the light buffers, so
	// sunless scenes render unlit meshes under a daytime sky.
	PerFrameData per_frame_data = {
		.view = view_matrix,
		.projection = projection_matrix,
		.view_projection = view_projection_matrix,
		.inv_view_projection = HMM_InvGeneralM4(view_projection_matrix),
		.camera_position = HMM_V4V(camera.location, 1.0f),
		.camera_forward = HMM_V4V(camera.forward, 0.0f),
		.sun_direction = HMM_V4V(-HMM_NormV3(HMM_V3(0.3f, 0.5f, 0.8f)), 0.0f),
		.sun_color = HMM_V4(1.0f, 1.0f, 1.0f, 1.0f),
	};

	if (state.scene.primary_sun_id)
	{
		Object& sun_object = state.scene.objects[*state.scene.primary_sun_id];
		const HMM_Vec3 sun_direction = HMM_NormV3(HMM_RotateV3Q(HMM_V3(0.0f, 0.0f, -1.0f), sun_object.current_transform.rotation));
		per_frame_data.sun_direction = HMM_V4V(sun_direction, 0.0f);
		per_frame_data.sun_color = HMM_V4V(sun_object.light.color * sun_object.light.sun.power, 1.0f);
	}

	// Descriptor writes happen before any of this frame's binds are recorded
	RenderPass& geometry_render_pass = get_render_pass(ERenderPass::Geometry);
	RenderPass& lighting_render_pass = get_render_pass(ERenderPass::Lighting);
	frame_data_update(
		&state.vk,
		per_frame_data,
		get_render_object_snapshot_buffer(state).get_gpu_buffer(),
		state.materials.buffer.get_gpu_buffer(),
		get_skin_matrix_arena_buffer(state).get_gpu_buffer(),
		state.images.items.data(),
		(i32) state.images.items.length()
	);
	RenderPass& tonemapping_render_pass = get_render_pass(ERenderPass::Tonemapping);
	// Height fog runs when an enabled fog controller exists; downstream
	// passes read its output instead of the lighting target
	RenderPass& fog_render_pass = get_render_pass(ERenderPass::Fog);
	const bool fog_render_active = state.fog.active && state.fog.debug_active
		&& state.fog.active_fog_controller_id.has_value()
		&& state.scene.objects.contains(*state.fog.active_fog_controller_id);
	if (fog_render_active)
	{
		const FogController& fog_controller = state.scene.objects[*state.fog.active_fog_controller_id].fog_controller;
		FogFsParams fog_fs_params = {
			.camera_position = camera.location,
			.fog_base_height = fog_controller.base_height,
			.fog_color = fog_controller.fog_color,
			.density = fog_controller.density,
			.scale_height = fog_controller.scale_height,
			.max_distance = fog_controller.max_distance,
			.ceiling_enabled = fog_controller.ceiling_enabled ? 1 : 0,
			.ceiling_height = fog_controller.ceiling_height,
			.ceiling_fade = fog_controller.ceiling_fade,
			.ambient_intensity = fog_controller.ambient_intensity,
			.sun_intensity = fog_controller.sun_intensity,
			.anisotropy = fog_controller.anisotropy,
			// game/ parity: no real sun means no sun in-scatter (the
			// PerFrameData fallback sun only drives the sky)
			.sun_direction = state.scene.primary_sun_id ? per_frame_data.sun_direction.XYZ : HMM_V3(0.0f, 0.0f, -1.0f),
			.sun_color = state.scene.primary_sun_id ? per_frame_data.sun_color.XYZ : HMM_V3(0.0f, 0.0f, 0.0f),
		};
		fog_pass_update(
			&state.vk,
			fog_fs_params,
			lighting_render_pass.get_color_output(0).view,
			geometry_render_pass.get_color_output(1).view
		);
	}

	VkImageView post_fog_scene_color_view = fog_render_active
		? fog_render_pass.get_color_output(0).view
		: lighting_render_pass.get_color_output(0).view;

	// DOF gathers from the post-fog color; tonemapping reads whichever pass
	// ran last
	RenderPass& dof_combine_render_pass = get_render_pass(ERenderPass::DofCombine);
	if (state.dof.enable)
	{
		DofCombineFsParams dof_combine_fs_params = {
			.cam_pos = HMM_V4V(camera.location, 1.0f),
			.cam_forward = HMM_V4V(camera.forward, 0.0f),
			.screen_size = HMM_V2((f32) state.window.render_width, (f32) state.window.render_height),
			.focus_distance = state.dof.focus_distance,
			.focus_range = state.dof.focus_range,
			.max_coc_radius = state.dof.max_coc_radius,
			.foreground_blur_scale = state.dof.foreground_blur_scale,
			.background_blur_scale = state.dof.background_blur_scale,
			.debug_mode = state.dof.debug_show_coc ? 1 : 0,
		};
		dof_combine_pass_update(
			&state.vk,
			dof_combine_fs_params,
			post_fog_scene_color_view,
			geometry_render_pass.get_color_output(1).view
		);
	}

	VkImageView post_dof_scene_color_view = state.dof.enable
		? dof_combine_render_pass.get_color_output(0).view
		: post_fog_scene_color_view;

	// Shaded wireframe copies the post-DOF color and blends wires on top
	RenderPass& wire_overlay_render_pass = get_render_pass(ERenderPass::WireOverlay);
	if (state.wireframe.shaded_wireframe)
	{
		WireOverlayMeshFsParams wire_fs_params = {
			.color = state.wireframe.color,
			.camera_position = HMM_V4V(camera.location, 1.0f),
			.camera_forward = HMM_V4V(HMM_NormV3(camera.forward), 0.0f),
			.screen_size = HMM_V2((f32) state.window.render_width, (f32) state.window.render_height),
			.width = state.wireframe.width,
			.softness = state.wireframe.softness,
			.opacity = state.wireframe.opacity,
			.visibility_tolerance = state.wireframe.visibility_tolerance,
		};
		WireOverlayPass::update(
			&state.vk,
			wire_fs_params,
			post_dof_scene_color_view,
			geometry_render_pass.get_color_output(1).view
		);
	}

	VkImageView post_wire_scene_color_view = state.wireframe.shaded_wireframe
		? wire_overlay_render_pass.get_color_output(0).view
		: post_dof_scene_color_view;

	// TAA ping-pong: write into set history_index, read the other set's
	// history output
	RenderPassEntry& temporal_aa_entry = get_render_pass_entry(ERenderPass::TemporalAA);
	const i32 temporal_aa_output_index = state.temporal_aa.history_index;
	const i32 temporal_aa_previous_index = (temporal_aa_output_index + 1) % 2;
	const auto get_temporal_aa_pass = [&](i32 in_set_idx) -> RenderPass& {
		return in_set_idx == 0 ? temporal_aa_entry.intermediate_pass() : temporal_aa_entry.final_pass();
	};
	if (state.temporal_aa.enable)
	{
		TemporalAaFsParams temporal_aa_fs_params = {
			.previous_view_projection = state.temporal_aa.previous_view_projection,
			.screen_size = HMM_V2((f32) state.window.render_width, (f32) state.window.render_height),
			.sharpen_axis = (state.temporal_aa.jitter_phase & 1) == 1 ? HMM_V2(1.0f, 0.0f) : HMM_V2(0.0f, 1.0f),
			.blend_alpha = state.temporal_aa.blend_alpha,
			.sharpen_strength = state.temporal_aa.sharpen_strength,
			.rejection_threshold = state.temporal_aa.rejection_threshold,
			.history_valid = state.temporal_aa.history_valid ? 1 : 0,
			.debug_mode = state.temporal_aa.debug_mode,
		};
		TemporalAAPass::update(
			&state.vk,
			temporal_aa_fs_params,
			post_wire_scene_color_view,
			geometry_render_pass.get_color_output(1).view,
			get_temporal_aa_pass(temporal_aa_previous_index).get_color_output(1).view
		);
	}

	VkImageView pre_tonemap_scene_color_view = state.temporal_aa.enable
		? get_temporal_aa_pass(temporal_aa_output_index).get_color_output(0).view
		: post_wire_scene_color_view;
	tonemapping_pass_update(&state.vk, pre_tonemap_scene_color_view, frame_data.linear_sampler);

	// FXAA reads the tonemapped LDR target; the copy pass presents whichever
	// ran last
	RenderPass& fxaa_render_pass = get_render_pass(ERenderPass::FXAA);
	const bool fxaa_active = state.temporal_aa.enable_fxaa;
	if (fxaa_active)
	{
		FXAAPass::update(&state.vk, tonemapping_render_pass.get_color_output(0).view);
	}
	frame_data_write_copy_input(
		&state.vk,
		state.images.enable_debug_fullscreen && state.images.items.length() > 0
			? state.images.items[CLAMP(state.images.debug_index, 0, (i32) state.images.items.length() - 1)].view
			: (fxaa_active
				? fxaa_render_pass.get_color_output(0).view
				: tonemapping_render_pass.get_color_output(0).view)
	);
	sky_pass_update(&state.vk);

	// Re-bake the octahedral sky when the sun moved (records before the
	// geometry pass, which samples it for the composite). The atmosphere
	// wants the direction toward the sun — the negated light-travel
	// direction (game/ sky_pass.h:96)
	sky_pass_bake_if_needed(&state.vk, -per_frame_data.sun_direction.XYZ);

	// Incrementally capture and project GI probes after GPU skinning and the
	// sky bake, before the main pass chain samples the probe atlas.
	{
		CPU_TIMING_SCOPE("GI Scene Update");
		gi_scene_update(&state.vk, gi_scene, state);
	}

	// Cascade matrices are CPU-side inputs to both the shadow draw and the
	// lighting shader's receiver reprojection — compute before the fs_params
	// upload below. The centered-squares anchor tracks the camera unless the
	// shadow map is frozen (game/ main.cpp:2990).
	if (!state.shadow.depth_freeze)
	{
		state.shadow.centered_square_center = camera.location
			+ HMM_NormV3(camera.forward) * state.shadow.centered_square_lookahead_distance;
	}
	const bool shadow_map_updated = ShadowDepthPass::compute_cascade_matrices(state, camera);
	state.shadow.force_recapture = false;

	// Lighting fs_params: direct, shadow, post-occlusion, and probe GI state.
	LightingFsParams lighting_fs_params = {};
	lighting_fs_params.view_position = camera.location;
	lighting_fs_params.view_forward = camera.forward;
	lighting_fs_params.num_point_lights = (i32) state.lighting.point_lights.length();
	lighting_fs_params.num_spot_lights = (i32) state.lighting.spot_lights.length();
	lighting_fs_params.num_sun_lights = (i32) state.lighting.sun_lights.length();
	lighting_fs_params.direct_lighting_enable = state.lighting.direct_enable ? 1 : 0;
	lighting_fs_params.ssao_enable = state.ssao.enable ? 1 : 0;
	lighting_fs_params.gi_enable = state.gi.enable ? 1 : 0;
	lighting_fs_params.gi_probe_occlusion = state.gi.probe_occlusion ? 1 : 0;
	lighting_fs_params.probe_occlusion_mode = (i32) state.gi.probe_occlusion_mode;
	lighting_fs_params.probe_radiance_mode = (i32) state.gi.probe_radiance_mode;
	lighting_fs_params.gi_intensity = state.gi.intensity;
	lighting_fs_params.atlas_total_size = GI_Scene::atlas_total_size;
	lighting_fs_params.atlas_entry_size = GI_Scene::atlas_entry_size;
	lighting_fs_params.gi_fallback_probe_index = gi_scene.fallback_probe_index;
	lighting_fs_params.gi_octree_node_count = (i32) gi_scene.octree_nodes.length();
	lighting_fs_params.isolated_probe_index = state.gi.probe_isolation_enable
		? (state.gi.isolated_probe_index >= 0 ? state.gi.isolated_probe_index : -2)
		: -1;
	lighting_fs_params.shadow_bias = state.shadow.shadow_bias;
	lighting_fs_params.shadow_map_texel_size = HMM_V2(
		1.0f / (f32) ShadowDepthPass::ShadowMapResolution,
		1.0f / (f32) ShadowDepthPass::ShadowMapResolution
	);
	if (ShadowDepthPass::has_valid_shadow_map)
	{
		lighting_fs_params.shadow_map_enable = 1;
		lighting_fs_params.shadow_num_cascades = ShadowDepthPass::get_active_cascade_count(state);
		lighting_fs_params.shadow_cascade_placement_mode = (i32) state.shadow.cascade_placement_mode;
		lighting_fs_params.shadow_debug_show_cascade_selection = state.shadow.debug_show_cascade_selection ? 1 : 0;
		lighting_fs_params.shadow_cascade_distances = HMM_V4(
			ShadowDepthPass::cascade_distances[0],
			ShadowDepthPass::cascade_distances[1],
			ShadowDepthPass::cascade_distances[2],
			ShadowDepthPass::cascade_distances[3]
		);
		for (i32 cascade_idx = 0; cascade_idx < MAX_SHADOW_CASCADES; ++cascade_idx)
		{
			lighting_fs_params.shadow_view_projections[cascade_idx] = ShadowDepthPass::shadow_view_projections[cascade_idx];
		}
	}

	RenderPass& shadow_render_pass = get_render_pass(ERenderPass::ShadowDepth);
	RenderPassEntry& shadow_blur_entry = get_render_pass_entry(ERenderPass::ShadowBlur);
	RenderPass& ssao_render_pass = get_render_pass(ERenderPass::SSAO);
	RenderPassEntry& ssao_blur_entry = get_render_pass_entry(ERenderPass::SSAO_Blur);
	RenderPassEntry& screen_space_shadows_entry = get_render_pass_entry(ERenderPass::ScreenSpaceShadows);

	// Screen-space contact shadows trace toward the shadow-casting sun
	Object* screen_space_shadow_sun = state.shadow.rendering_enable && state.shadow.screen_space.enable
		? ShadowDepthPass::get_valid_shadow_sun(state)
		: nullptr;
	const bool screen_space_shadows_valid = screen_space_shadow_sun != nullptr;
	if (screen_space_shadows_valid)
	{
		lighting_fs_params.screen_space_shadows_enable = 1;
		lighting_fs_params.screen_space_shadow_intensity = state.shadow.screen_space.intensity;
	}
	// Lighting samples the blurred moments (soft penumbra) unless the blur is
	// disabled — game/'s default is blurred
	VkImageView shadow_moments_view = state.shadow.blur_enable
		? shadow_blur_entry.final_pass().get_color_output(0).view
		: shadow_render_pass.get_color_output(0).view;
	lighting_pass_update(
		&state.vk,
		lighting_fs_params,
		geometry_render_pass.color_outputs.data(),
		shadow_moments_view,
		ssao_blur_entry.final_pass().get_color_output(0).view,
		screen_space_shadows_entry.final_pass().get_color_output(0).view,
		state.lighting.point_buffers[state.lighting.buffer_index].get_gpu_buffer(),
		state.lighting.spot_buffers[state.lighting.buffer_index].get_gpu_buffer(),
		state.lighting.sun_buffers[state.lighting.buffer_index].get_gpu_buffer(),
		gi_scene.probes_buffer.get_gpu_buffer(),
		gi_scene.cells_buffer.get_gpu_buffer(),
		gi_scene_get_octahedral_lighting_view(gi_scene),
		gi_scene_get_octahedral_depth_view(gi_scene),
		gi_scene.sh9_coefficients_buffer.get_gpu_buffer(),
		gi_scene.sg9_lobes_buffer.get_gpu_buffer(),
		gi_scene.octree_nodes_buffer.get_gpu_buffer()
	);

	ssao_pass_update(
		&state.vk,
		HMM_V2((f32) ssao_render_pass.current_width, (f32) ssao_render_pass.current_height),
		view_matrix,
		projection_matrix,
		state.ssao.enable,
		geometry_render_pass.get_color_output(1).view,	// world position
		geometry_render_pass.get_color_output(2).view,	// world normal
		ssao_render_pass.get_color_output(0).view,
		ssao_blur_entry.intermediate_pass().get_color_output(0).view
	);

	{
		RenderPass& screen_space_trace_pass = screen_space_shadows_entry.intermediate_pass();
		const HMM_Vec3 screen_space_sun_dir = screen_space_shadows_valid
			? HMM_NormV3(HMM_RotateV3Q(HMM_V3(0.0f, 0.0f, -1.0f), screen_space_shadow_sun->current_transform.rotation))
			: HMM_V3(0.0f, 0.0f, -1.0f);
		ScreenSpaceShadowsPass::update(
			&state.vk,
			state,
			HMM_V2((f32) screen_space_trace_pass.current_width, (f32) screen_space_trace_pass.current_height),
			view_matrix,
			projection_matrix,
			screen_space_sun_dir,
			screen_space_shadows_valid,
			geometry_render_pass.get_color_output(1).view,	// world position
			geometry_render_pass.get_color_output(2).view,	// world normal
			screen_space_trace_pass.get_color_output(0).view
		);
	}

	// Shadow cascades: one moments slice per active cascade. Skipped entirely
	// without a valid shadow sun, and under depth_freeze the stale map keeps
	// being sampled with its frozen matrices; the transition below still runs
	// so the (cleared or stale) moments image is legal to have bound — the
	// lighting shader only samples it when shadow_map_enable is set.
	if (shadow_map_updated)
	{
		shadow_render_pass.set_pass_count_override(ShadowDepthPass::get_active_cascade_count(state));
		shadow_render_pass.execute(&state.vk, [&](i32 in_cascade_idx)
		{
			ShadowDepthPass::render_cascade(&state.vk, state, in_cascade_idx);
		});
		shadow_render_pass.set_pass_count_override(-1);
	}
	gpu_image_transition(
		state.vk.command_buffers[state.vk.frame_index],
		shadow_render_pass.get_color_output(0),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	);

	// Separable blur over the moments — the source of EVSM's soft penumbra.
	// Runs only when the shadow map re-rendered (frozen maps keep their old
	// blur). The final target's transition runs even when the blur is skipped
	// so the (possibly stale/cleared) image bound to the lighting set stays
	// legal.
	if (shadow_map_updated && state.shadow.blur_enable)
	{
		ShadowBlurPass::execute_separable(
			&state.vk,
			shadow_blur_entry,
			ShadowDepthPass::get_active_cascade_count(state)
		);
	}
	gpu_image_transition(
		state.vk.command_buffers[state.vk.frame_index],
		shadow_blur_entry.final_pass().get_color_output(0),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	);

	state.shadow.debug_cascade_index = CLAMP(
		state.shadow.debug_cascade_index,
		0,
		MAX(0, ShadowDepthPass::get_active_cascade_count(state) - 1)
	);
	RenderPass& shadow_debug_pass = get_render_pass(ERenderPass::ShadowCascadeDebug);
	shadow_debug_pass.execute(&state.vk, [&](i32)
	{
		ShadowCascadeDebugPass::render(
			&state.vk,
			shadow_moments_view,
			frame_data.linear_sampler,
			state.shadow.debug_cascade_index,
			state.shadow.debug_view_mode
		);
	});
	gpu_image_transition(
		state.vk.command_buffers[state.vk.frame_index],
		shadow_debug_pass.get_color_output(0),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	);

	// Geometry: scene meshes -> G-buffer at render resolution, camera-frustum
	// culled on the CPU (game/ parity; skinned meshes bypass the frustum test)
	geometry_render_pass.execute(&state.vk, [&](i32)
	{
		geometry_pass_bind(&state.vk);

		if (state.render_objects.valid)
		{
			CullResult cull_result = cull_objects(state, view_projection_matrix,
				state.tessellation.enabled ? state.tessellation.bounds_padding : 0.0f);
			for (i32 mesh_object_id : cull_result.object_ids)
			{
				auto found = state.scene.objects.find(mesh_object_id);
				if (found == state.scene.objects.end())
				{
					continue;
				}

				Object& object = found->second;
				if (object.render_object_index >= 0)
				{
					geometry_pass_draw_mesh(&state.vk, object.mesh, object.render_object_index, state.animation.skinning_debug_view);
					state.data_oriented.frame.draw_calls += 1;
					state.data_oriented.frame.draw_mesh_count += 1;
				}
			}
		}

		// Sky composite fills the background at the far plane
		GIDebugPass::draw(&state.vk, gi_scene, state, view_projection_matrix);

		// Sky composite fills the background at the far plane
		if (state.sky.rendering_enable)
		{
			sky_pass_draw_composite(&state.vk);
		}
	});

	// Lighting reads the whole G-buffer (transitions before execute —
	// barriers are illegal inside dynamic rendering)
	for (i32 gbuffer_idx = 0; gbuffer_idx < Render::GBUFFER_OUTPUT_COUNT; ++gbuffer_idx)
	{
		gpu_image_transition(
			state.vk.command_buffers[state.vk.frame_index],
			geometry_render_pass.get_color_output(gbuffer_idx),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		);
	}
	// SSAO reads G-buffer position/normal (already SHADER_READ_ONLY), then
	// its raw output is blurred; lighting samples the blurred result
	ssao_render_pass.execute(&state.vk, [&](i32)
	{
		ssao_pass_draw(&state.vk);
	});
	gpu_image_transition(
		state.vk.command_buffers[state.vk.frame_index],
		ssao_render_pass.get_color_output(0),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	);
	BlurPass::execute_separable(
		&state.vk,
		ssao_blur_entry,
		ssao_pass.blur_horizontal_sets[state.vk.frame_index],
		ssao_pass.blur_vertical_sets[state.vk.frame_index],
		4
	);

	// Contact shadows trace + filter (skipped without a shadow sun; the
	// transition keeps the bound mask image legal — the lighting shader only
	// samples it when screen_space_shadows_enable is set)
	if (screen_space_shadows_valid)
	{
		ScreenSpaceShadowsPass::execute(&state.vk, screen_space_shadows_entry);
	}
	gpu_image_transition(
		state.vk.command_buffers[state.vk.frame_index],
		screen_space_shadows_entry.final_pass().get_color_output(0),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	);

	lighting_render_pass.execute(&state.vk, [&](i32)
	{
		lighting_pass_draw(&state.vk);
	});

	// Fog reads the lit scene + G-buffer position; tonemapping then reads the
	// post-fog color (or the lighting output directly when fog is off)
	gpu_image_transition(
		state.vk.command_buffers[state.vk.frame_index],
		lighting_render_pass.get_color_output(0),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	);
	if (fog_render_active)
	{
		fog_render_pass.execute(&state.vk, [&](i32)
		{
			fog_pass_draw(&state.vk);
		});
		gpu_image_transition(
			state.vk.command_buffers[state.vk.frame_index],
			fog_render_pass.get_color_output(0),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		);
	}
	if (state.dof.enable)
	{
		dof_combine_render_pass.execute(&state.vk, [&](i32)
		{
			dof_combine_pass_draw(&state.vk);
		});
		gpu_image_transition(
			state.vk.command_buffers[state.vk.frame_index],
			dof_combine_render_pass.get_color_output(0),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		);
	}
	if (state.wireframe.shaded_wireframe)
	{
		wire_overlay_render_pass.execute(&state.vk, [&](i32)
		{
			WireOverlayPass::draw(&state.vk, state, view_projection_matrix);
		});
		gpu_image_transition(
			state.vk.command_buffers[state.vk.frame_index],
			wire_overlay_render_pass.get_color_output(0),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		);
	}

	// TAA: both ping-pong sets get an unconditional transition so the bound
	// history descriptor stays legal even on the first frames / when TAA is
	// toggled; the shader ignores history until history_valid is set
	for (i32 set_idx = 0; set_idx < 2; ++set_idx)
	{
		for (i32 output_idx = 0; output_idx < 2; ++output_idx)
		{
			gpu_image_transition(
				state.vk.command_buffers[state.vk.frame_index],
				get_temporal_aa_pass(set_idx).get_color_output(output_idx),
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			);
		}
	}
	if (state.temporal_aa.enable)
	{
		RenderPass& temporal_aa_target_pass = get_temporal_aa_pass(temporal_aa_output_index);
		temporal_aa_target_pass.execute(&state.vk, [&](i32)
		{
			TemporalAAPass::draw(&state.vk);
		});
		for (i32 output_idx = 0; output_idx < 2; ++output_idx)
		{
			gpu_image_transition(
				state.vk.command_buffers[state.vk.frame_index],
				temporal_aa_target_pass.get_color_output(output_idx),
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
			);
		}

		state.temporal_aa.previous_view_projection = view_projection_matrix;
		state.temporal_aa.history_valid = true;
		state.temporal_aa.history_index = temporal_aa_previous_index;
	}

	tonemapping_render_pass.execute(&state.vk, [&](i32)
	{
		tonemapping_pass_draw(&state.vk, state.tonemapping.exposure_bias);
	});

	// FXAA filters the tonemapped LDR target; copy presents whichever ran last
	gpu_image_transition(
		state.vk.command_buffers[state.vk.frame_index],
		tonemapping_render_pass.get_color_output(0),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	);
	if (fxaa_active)
	{
		fxaa_render_pass.execute(&state.vk, [&](i32)
		{
			FXAAPass::draw(&state.vk, HMM_V2((f32) state.window.render_width, (f32) state.window.render_height));
		});
		gpu_image_transition(
			state.vk.command_buffers[state.vk.frame_index],
			fxaa_render_pass.get_color_output(0),
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		);
	}
	get_render_pass(ERenderPass::CopyToSwapchain).execute(&state.vk, [&](i32)
	{
		copy_to_swapchain_pass_draw(&state.vk);
	});
	ImGuiLayer::render(&state.vk);

	// Debug frame dump for automated visual verification
	static const char* screenshot_path = getenv("GAME2_SCREENSHOT");
	static const char* screenshot_frame_env = getenv("GAME2_SCREENSHOT_FRAME");
	static const u64 screenshot_frame = screenshot_frame_env ? strtoull(screenshot_frame_env, nullptr, 10) : 60;
	if (screenshot_path && state.vk.frame_number == screenshot_frame)
	{
		state.vk.pending_frame_dump = screenshot_path;
	}

	vulkan_context_end_frame(&state.vk);

	reset_mouse_delta();
}

int main(int argc, char** argv)
{
	// Unbuffered stdout so logs survive crashes and external kills
	setvbuf(stdout, nullptr, _IONBF, 0);

	cxxopts::Options options("Game", "Game that uses Blender as its tooling (Vulkan)");

	options.add_options()
		("f,file", "File name", cxxopts::value<std::string>())
		("p,port", "Live link TCP port", cxxopts::value<std::string>()->default_value("65432"))
		("no-live-link", "Do not start the live-link server", cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
		("warmup-frames", "Benchmark warmup frame count", cxxopts::value<u64>()->default_value("300"))
		("benchmark-frames", "Measured frame count; providing this enables benchmark mode", cxxopts::value<u64>())
		("benchmark-output", "Benchmark JSON output path", cxxopts::value<std::string>()->default_value("benchmark.json"))
	;

	// First positional arg can be file to load
	options.parse_positional({"file"});

	auto args = options.parse(argc, argv);

	// If we passed an init file, load it on startup
	if (args.count("file") > 0)
	{
		state.runtime.init_file = args["f"].as<std::string>();
	}
	state.live_link.port = args["port"].as<std::string>();
	const bool no_live_link = args["no-live-link"].as<bool>();
	BenchmarkState benchmark;
	if (args.count("benchmark-frames") > 0)
	{
		benchmark.configure(
			args["warmup-frames"].as<u64>(),
			args["benchmark-frames"].as<u64>(),
			args["benchmark-output"].as<std::string>()
		);
		state.debug_ui.visible = false;
	}

	glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit())
	{
		printf("Failed to initialize GLFW\n");
		return 1;
	}

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWwindow* window = glfwCreateWindow(state.window.width, state.window.height, "Blender Game", nullptr, nullptr);
	if (!window)
	{
		printf("Failed to create GLFW window\n");
		return 1;
	}
	state.window.handle = window;

	glfwSetKeyCallback(window, key_callback);
	glfwSetMouseButtonCallback(window, mouse_button_callback);
	glfwSetCursorPosCallback(window, cursor_position_callback);
	glfwSetCharCallback(window, char_callback);
	glfwSetScrollCallback(window, scroll_callback);
	glfwSetCursorEnterCallback(window, cursor_enter_callback);
	glfwSetWindowFocusCallback(window, window_focus_callback);
	glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

	jolt_init();

	vulkan_context_init(&state.vk, window);
	Render::configure_formats(state.vk);
	ImGuiLayer::init(&state.vk);
	#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	glfwSetMonitorCallback(ImGui_ImplGlfw_MonitorCallback);
	#endif
	frame_data_init(&state.vk);
	geometry_pass_init(&state.vk);
	ShadowDepthPass::init(&state.vk);
	ShadowBlurPass::init(&state.vk);
	ssao_pass_init(&state.vk, frame_data.linear_sampler);
	BlurPass::init(&state.vk, Render::SSAO_FORMAT);
	ScreenSpaceShadowsPass::init(&state.vk, frame_data.linear_sampler);
	fog_pass_init(&state.vk, frame_data.linear_sampler);
	dof_combine_pass_init(&state.vk, frame_data.linear_sampler);
	WireOverlayPass::init(&state.vk, frame_data.linear_sampler);
	TemporalAAPass::init(&state.vk, frame_data.linear_sampler);
	FXAAPass::init(&state.vk, frame_data.linear_sampler);
	GpuSkinning::init(&state.vk);
	Tessellation::init(&state.vk);
	lighting_pass_init(&state.vk, frame_data.linear_sampler);
	tonemapping_pass_init(&state.vk);
	sky_pass_init(&state.vk);
	copy_to_swapchain_pass_init(&state.vk);

	// These buffers must exist even for empty scenes: descriptor set 0
	// bindings 1-3 are written every frame
	render_object_snapshot_ensure_capacity(state, 1);
	init_materials_buffer(state);
	skin_matrix_arena_ensure_capacity(state, 1);
	init_lighting_buffers(state);
	gi_scene_init(&state.vk, gi_scene, state);
	GIDebugPass::init(&state.vk);

	// Render-scale override for testing (percentage, 25..100)
	if (const char* render_scale_env = getenv("GAME2_RENDER_SCALE"))
	{
		state.window.resolution_percentage = (i32) strtol(render_scale_env, nullptr, 10);
	}

	// Headless shadow overrides; the ImGui panel exposes the same controls.
	if (const char* shadow_placement_env = getenv("GAME2_SHADOW_PLACEMENT"))
	{
		state.shadow.cascade_placement_mode = strtol(shadow_placement_env, nullptr, 10) == 1
			? EShadowCascadePlacementMode::CenteredSquares
			: EShadowCascadePlacementMode::Frustum;
	}
	if (getenv("GAME2_SHADOW_CASCADE_DEBUG"))
	{
		state.shadow.debug_show_cascade_selection = true;
	}
	if (getenv("GAME2_HIDE_UI"))
	{
		state.debug_ui.visible = false;
	}
	if (const char* ssao_env = getenv("GAME2_SSAO"))
	{
		state.ssao.enable = strtol(ssao_env, nullptr, 10) != 0;
	}
	if (const char* dof_env = getenv("GAME2_DOF"))
	{
		state.dof.enable = strtol(dof_env, nullptr, 10) != 0;
	}
	if (const char* dof_focus_env = getenv("GAME2_DOF_FOCUS"))
	{
		state.dof.focus_distance = strtof(dof_focus_env, nullptr);
	}
	if (const char* dof_range_env = getenv("GAME2_DOF_RANGE"))
	{
		state.dof.focus_range = strtof(dof_range_env, nullptr);
	}
	if (getenv("GAME2_DOF_DEBUG"))
	{
		state.dof.debug_show_coc = true;
	}
	if (getenv("GAME2_WIREFRAME"))
	{
		state.wireframe.shaded_wireframe = true;
	}
	if (const char* taa_env = getenv("GAME2_TAA"))
	{
		state.temporal_aa.enable = strtol(taa_env, nullptr, 10) != 0;
	}
	if (const char* fxaa_env = getenv("GAME2_FXAA"))
	{
		state.temporal_aa.enable_fxaa = strtol(fxaa_env, nullptr, 10) != 0;
	}
	if (const char* tessellation_env = getenv("GAME2_TESSELLATION"))
	{
		state.tessellation.enabled = strtol(tessellation_env, nullptr, 10) != 0;
	}
	if (const char* tessellation_mode_env = getenv("GAME2_TESSELLATION_MODE"))
	{
		state.tessellation.mode = (ETessellationMode) CLAMP((i32) strtol(tessellation_mode_env, nullptr, 10), 0, 2);
	}
	if (const char* tessellation_factor_env = getenv("GAME2_TESSELLATION_FACTOR"))
	{
		state.tessellation.fixed_factor = CLAMP((i32) strtol(tessellation_factor_env, nullptr, 10), 1, (i32) Tessellation::MAX_FACTOR);
	}
	if (getenv("GAME2_GI_PROBES"))
	{
		state.gi.show_probes = true;
	}
	if (const char* gi_radiance_env = getenv("GAME2_GI_RADIANCE_MODE"))
	{
		state.gi.probe_radiance_mode = (EProbeRadianceMode) CLAMP((i32) strtol(gi_radiance_env, nullptr, 10), 0, 2);
	}
	if (const char* gi_occlusion_env = getenv("GAME2_GI_OCCLUSION_MODE"))
	{
		state.gi.probe_occlusion_mode = (EProbeOcclusionMode) CLAMP((i32) strtol(gi_occlusion_env, nullptr, 10), 0, 1);
	}

	// Register render passes and size their targets
	// Fixed-size cascaded shadow map: one moments image with a layer per
	// cascade. Clear {1,1,0,0} = "fully lit" EVSM moments so unrendered
	// cascades never darken receivers.
	get_render_pass_entry(ERenderPass::ShadowDepth).init_final((RenderPassDesc) {
		.initial_width = ShadowDepthPass::ShadowMapResolution,
		.initial_height = ShadowDepthPass::ShadowMapResolution,
		.pass_count = MAX_SHADOW_CASCADES,
		.num_outputs = 1,
		.outputs = {
			{
				.format = Render::SHADOW_MOMENTS_FORMAT,
				.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.store_op = VK_ATTACHMENT_STORE_OP_STORE,
				.clear_value = {{{ 1.0f, 1.0f, 0.0f, 0.0f }}},
			},
		},
		.depth_output = {
			.format = Render::SCENE_DEPTH_FORMAT,
			.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.clear_value = { .depthStencil = { .depth = Render::DEPTH_CLEAR_VALUE } },
		},
		.resize_with_window = false,
		.type = ERenderPassType::Array,
		.debug_label = "Shadow Depth",
	});

	// Separable moments blur: horizontal (intermediate) -> vertical (final),
	// same layered layout as the shadow map, no depth
	const auto make_shadow_blur_desc = [](const char* in_debug_label)
	{
		return (RenderPassDesc) {
			.initial_width = ShadowDepthPass::ShadowMapResolution,
			.initial_height = ShadowDepthPass::ShadowMapResolution,
			.pass_count = MAX_SHADOW_CASCADES,
			.num_outputs = 1,
			.outputs = {
				{
					.format = Render::SHADOW_MOMENTS_FORMAT,
					.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
					.store_op = VK_ATTACHMENT_STORE_OP_STORE,
					.clear_value = {{{ 1.0f, 1.0f, 0.0f, 0.0f }}},
				},
			},
			.resize_with_window = false,
			.type = ERenderPassType::Array,
			.debug_label = in_debug_label,
		};
	};
	get_render_pass_entry(ERenderPass::ShadowBlur).init_intermediate(make_shadow_blur_desc("Shadow Blur Horizontal"));
	get_render_pass_entry(ERenderPass::ShadowBlur).init_final(make_shadow_blur_desc("Shadow Blur Vertical"));
	get_render_pass_entry(ERenderPass::ShadowCascadeDebug).init_final((RenderPassDesc) {
		.initial_width = ShadowDepthPass::ShadowMapResolution,
		.initial_height = ShadowDepthPass::ShadowMapResolution,
		.num_outputs = 1,
		.outputs = {{
			.format = Render::SCENE_COLOR_FORMAT,
			.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.store_op = VK_ATTACHMENT_STORE_OP_STORE,
			.clear_value = {{{ 0.0f, 0.0f, 0.0f, 1.0f }}},
		}},
		.resize_with_window = false,
		.debug_label = "Shadow Cascade Debug",
	});
	ShadowCascadeDebugPass::init(&state.vk);

	// The blur's input sets are static: both source images are fixed-size and
	// never recreated
	ShadowBlurPass::init_sets(
		&state.vk,
		get_render_pass(ERenderPass::ShadowDepth).get_color_output(0).view,
		get_render_pass_entry(ERenderPass::ShadowBlur).intermediate_pass().get_color_output(0).view,
		frame_data.linear_sampler
	);

	// SSAO at half render resolution; the generic BlurPass smooths it before
	// lighting samples it (game/ main.cpp:1485-1566)
	const auto make_ssao_desc = [](const char* in_debug_label)
	{
		return (RenderPassDesc) {
			.num_outputs = 1,
			.outputs = {
				{
					.format = Render::SSAO_FORMAT,
					.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
					.store_op = VK_ATTACHMENT_STORE_OP_STORE,
					.clear_value = {{{ 1.0f, 1.0f, 1.0f, 1.0f }}},
				},
			},
			.width_scale = 0.5f,
			.height_scale = 0.5f,
			.debug_label = in_debug_label,
		};
	};
	get_render_pass_entry(ERenderPass::SSAO).init_final(make_ssao_desc("SSAO"));
	get_render_pass_entry(ERenderPass::SSAO_Blur).init_intermediate(make_ssao_desc("SSAO Blur Horizontal"));
	get_render_pass_entry(ERenderPass::SSAO_Blur).init_final(make_ssao_desc("SSAO Blur Vertical"));

	// Screen-space contact shadows share the SSAO target shape (half res,
	// R8, clear 1 = fully visible)
	get_render_pass_entry(ERenderPass::ScreenSpaceShadows).init_intermediate(make_ssao_desc("Screen Space Shadows Trace"));
	get_render_pass_entry(ERenderPass::ScreenSpaceShadows).init_final(make_ssao_desc("Screen Space Shadows Filter"));

	get_render_pass_entry(ERenderPass::Geometry).init_final((RenderPassDesc) {
		.num_outputs = Render::GBUFFER_OUTPUT_COUNT,
		.outputs = {
			// 0: base/emission color — sky-blue clear keeps empty scenes
			// readable until the sky pass lands (game/ clears {0,0,0,1})
			{
				.format = Render::GBUFFER_FORMAT,
				.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.store_op = VK_ATTACHMENT_STORE_OP_STORE,
				.clear_value = {{{ 0.1f, 0.2f, 0.4f, 1.0f }}},
			},
			// 1: world position (w=1 valid)
			{
				.format = Render::GBUFFER_FORMAT,
				.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.store_op = VK_ATTACHMENT_STORE_OP_STORE,
				.clear_value = {{{ 0.0f, 0.0f, 0.0f, 0.0f }}},
			},
			// 2: world normal (vec4(0) = no-geometry sentinel)
			{
				.format = Render::GBUFFER_FORMAT,
				.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.store_op = VK_ATTACHMENT_STORE_OP_STORE,
				.clear_value = {{{ 0.0f, 0.0f, 0.0f, 0.0f }}},
			},
			// 3: roughness / metallic / emission strength
			{
				.format = Render::GBUFFER_FORMAT,
				.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.store_op = VK_ATTACHMENT_STORE_OP_STORE,
				.clear_value = {{{ 0.0f, 0.0f, 0.0f, 0.0f }}},
			},
		},
		.depth_output = {
			.format = Render::SCENE_DEPTH_FORMAT,
			.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.clear_value = { .depthStencil = { .depth = Render::DEPTH_CLEAR_VALUE } },
		},
		.type = ERenderPassType::Single,
		.debug_label = "Geometry",
	});

	get_render_pass_entry(ERenderPass::Lighting).init_final((RenderPassDesc) {
		.num_outputs = 1,
		.outputs = {
			{
				.format = Render::SCENE_COLOR_FORMAT,
				.load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.store_op = VK_ATTACHMENT_STORE_OP_STORE,
			},
		},
		.type = ERenderPassType::Single,
		.debug_label = "Lighting",
	});

	get_render_pass_entry(ERenderPass::Fog).init_final((RenderPassDesc) {
		.num_outputs = 1,
		.outputs = {
			{
				.format = Render::SCENE_COLOR_FORMAT,
				.load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.store_op = VK_ATTACHMENT_STORE_OP_STORE,
			},
		},
		.type = ERenderPassType::Single,
		.debug_label = "Fog",
	});

	get_render_pass_entry(ERenderPass::DofCombine).init_final((RenderPassDesc) {
		.num_outputs = 1,
		.outputs = {
			{
				.format = Render::SCENE_COLOR_FORMAT,
				.load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.store_op = VK_ATTACHMENT_STORE_OP_STORE,
			},
		},
		.type = ERenderPassType::Single,
		.debug_label = "DOF Combine",
	});

	get_render_pass_entry(ERenderPass::WireOverlay).init_final((RenderPassDesc) {
		.num_outputs = 1,
		.outputs = {
			{
				.format = Render::SCENE_COLOR_FORMAT,
				.load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.store_op = VK_ATTACHMENT_STORE_OP_STORE,
			},
		},
		.type = ERenderPassType::Single,
		.debug_label = "Wire Overlay",
	});

	// TAA ping-pong: two target sets (intermediate = set 0, final = set 1),
	// each MRT [resolved, history]; the shader reads the other set's history
	const auto make_temporal_aa_desc = [](const char* in_debug_label)
	{
		return (RenderPassDesc) {
			.num_outputs = 2,
			.outputs = {
				{
					.format = Render::SCENE_COLOR_FORMAT,
					.load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
					.store_op = VK_ATTACHMENT_STORE_OP_STORE,
				},
				{
					.format = Render::SCENE_COLOR_FORMAT,
					.load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
					.store_op = VK_ATTACHMENT_STORE_OP_STORE,
				},
			},
			.type = ERenderPassType::Single,
			.debug_label = in_debug_label,
		};
	};
	get_render_pass_entry(ERenderPass::TemporalAA).init_intermediate(make_temporal_aa_desc("Temporal AA (Set 0)"));
	get_render_pass_entry(ERenderPass::TemporalAA).init_final(make_temporal_aa_desc("Temporal AA (Set 1)"));

	get_render_pass_entry(ERenderPass::Tonemapping).init_final((RenderPassDesc) {
		.num_outputs = 1,
		.outputs = {
			{
				.format = state.vk.surface_format.format,
				.load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.store_op = VK_ATTACHMENT_STORE_OP_STORE,
			},
		},
		.type = ERenderPassType::Single,
		.debug_label = "Tonemapping",
	});

	get_render_pass_entry(ERenderPass::FXAA).init_final((RenderPassDesc) {
		.num_outputs = 1,
		.outputs = {
			{
				.format = state.vk.surface_format.format,
				.load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.store_op = VK_ATTACHMENT_STORE_OP_STORE,
			},
		},
		.type = ERenderPassType::Single,
		.debug_label = "FXAA",
	});

	get_render_pass_entry(ERenderPass::CopyToSwapchain).init_final((RenderPassDesc) {
		.type = ERenderPassType::Swapchain,
		.debug_label = "Copy To Swapchain",
	});

	handle_resize(/*in_force=*/ true);

	// If we passed an init file, load it as a flatbuffer Update on startup
	// (port of game/src/main.cpp:1666-1684)
	if (state.runtime.init_file)
	{
		if (FILE* file = fopen(state.runtime.init_file->c_str(), "rb"))
		{
			fseek(file, 0, SEEK_END);
			long file_size = ftell(file);
			rewind(file);

			assert(file_size > 0);

			StretchyBuffer<u8> flatbuffer_data;
			flatbuffer_data.add_uninitialized(file_size);

			size_t bytes_read = fread(flatbuffer_data.data(), 1, file_size, file);
			assert(bytes_read == (size_t) file_size);
			fclose(file);

			parse_flatbuffer_data(flatbuffer_data);
		}
		else
		{
			printf("Failed to open init file: %s\n", state.runtime.init_file->c_str());
		}
	}

	// Start Live Link Server (Blender connects to 127.0.0.1:<port>). Offline
	// benchmark runs use a captured --file update and skip the socket thread.
	if (!no_live_link)
	{
		state.live_link.thread = std::thread(live_link_thread_function);
	}

	f64 last_frame_time = glfwGetTime();
	benchmark.begin(&state.vk);

	while (!glfwWindowShouldClose(window))
	{
		glfwPollEvents();

		const f64 current_time = glfwGetTime();
		const f32 delta_time = (f32)(current_time - last_frame_time);
		last_frame_time = current_time;

		// Debug: exercise swapchain recreation without manual window dragging
		static const bool test_resize = getenv("GAME2_TEST_RESIZE") != nullptr;
		if (test_resize && state.vk.frame_number == 30)
		{
			glfwSetWindowSize(window, 1280, 720);
		}

		const f64 frame_start_time = glfwGetTime();
		frame(delta_time);
		const f64 frame_end_time = glfwGetTime();
		benchmark.after_frame((frame_end_time - frame_start_time) * 1000.0, &state.vk);
		if (benchmark.should_exit())
		{
			glfwSetWindowShouldClose(window, GLFW_TRUE);
		}
	}
	benchmark_finalize(benchmark, &state.vk);

	// Tell live_link_thread we're done running and wait for it to complete.
	// Note: like game/, the thread blocks in accept() until Blender's first
	// connection, so quitting before any connection waits on that accept.
	state.runtime.game_running = false;
	if (!no_live_link)
	{
		state.live_link.thread.join();
	}

	VK_CHECK(vulkan_device_wait_idle(&state.vk));
	ImGuiLayer::shutdown();
	GIDebugPass::shutdown(&state.vk);
	gi_scene_cleanup(&state.vk, gi_scene);

	for (auto& [unique_id, object] : state.scene.objects)
	{
		object_cleanup(object);
	}
	state.scene.objects.clear();

	for (i32 pass_index = 0; pass_index < (i32) ERenderPass::COUNT; ++pass_index)
	{
		state.render_passes.passes[pass_index].cleanup();
	}

	for (i32 buffer_idx = 0; buffer_idx < RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT; ++buffer_idx)
	{
		state.render_objects.buffers[buffer_idx].destroy_gpu_buffer();
		state.skin_matrices.buffers[buffer_idx].destroy_gpu_buffer();
	}
	state.materials.buffer.destroy_gpu_buffer();
	reset_images();

	jolt_shutdown();

	for (i32 buffer_idx = 0; buffer_idx < RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT; ++buffer_idx)
	{
		state.lighting.point_buffers[buffer_idx].destroy_gpu_buffer();
		state.lighting.spot_buffers[buffer_idx].destroy_gpu_buffer();
		state.lighting.sun_buffers[buffer_idx].destroy_gpu_buffer();
	}

	copy_to_swapchain_pass_shutdown(&state.vk);
	sky_pass_shutdown(&state.vk);
	tonemapping_pass_shutdown(&state.vk);
	Tessellation::shutdown(&state.vk);
	GpuSkinning::shutdown(&state.vk);
	FXAAPass::shutdown(&state.vk);
	TemporalAAPass::shutdown(&state.vk);
	WireOverlayPass::shutdown(&state.vk);
	dof_combine_pass_shutdown(&state.vk);
	fog_pass_shutdown(&state.vk);
	lighting_pass_shutdown(&state.vk);
	ScreenSpaceShadowsPass::shutdown(&state.vk);
	BlurPass::shutdown(&state.vk);
	ssao_pass_shutdown(&state.vk);
	ShadowBlurPass::shutdown(&state.vk);
	ShadowCascadeDebugPass::shutdown(&state.vk);
	ShadowDepthPass::shutdown(&state.vk);
	geometry_pass_shutdown(&state.vk);
	frame_data_shutdown(&state.vk);
	vulkan_context_shutdown(&state.vk);

	glfwDestroyWindow(window);
	glfwTerminate();
	return 0;
}
