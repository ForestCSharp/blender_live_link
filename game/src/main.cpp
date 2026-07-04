// C std lib I/O and stdlib
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef WITH_DEBUG_UI
#define WITH_DEBUG_UI 1
#endif

// Simple Template Wrapper around stb_ds array
#define STB_DS_IMPLEMENTATION
#include "core/stretchy_buffer.h"

#include "render/gi.h"

// C++ std lib Optional
#include <optional>
using std::optional;

// C++ std lib Random
#include <random>

// C++ std lib threading
#include <thread>

// C++ std lib atomic
#include <atomic>
using std::atomic;

// C++ std lib string
#include <string>

// C++ std lib algorithms
#include <algorithm>

// C++ std lib numeric limits
#include <limits>

// For C++ std::function
#include <functional>

// Flatbuffers generated file
#include "blender_live_link_generated.h"

// Jolt Physics
#include "physics/physics_system.h"

// Basic Types
#include "core/types.h"
#include "core/timings.h"

// Game Objects we receive from Blender
#include "game_object/game_object.h"

// Gpu Image and Buffer Wrappers
#include "render/gpu_buffer.h"
#include "render/gpu_image.h"

// Render Pass abstraction
#include "render/render_pass.h"

// Ankerl's Segmented Vector and Fast Unordered Hash Math
#include "ankerl/unordered_dense.h"
using ankerl::unordered_dense::map;

// Handmade Math
#define HANDMADE_MATH_IMPLEMENTATION
#include "handmade_math/HandmadeMath.h"

// cxxopts
#include "cxxopts/cxxopts.hpp"

// Sokol
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_log.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_time.h"

// Sokol Debug Text
#include "sokol/util/sokol_debugtext.h"
#define FONT_KC853 (0)
#define FONT_KC854 (1)
#define FONT_Z1013 (2)
#define FONT_CPC   (3)
#define FONT_C64   (4)
#define FONT_ORIC  (5)

#include "render/shader_files.h"

// Render Passes
#include "render/geometry_pass.h"
#include "render/lighting_pass.h"
#include "render/blur_pass.h"
#include "render/wire_overlay_pass.h"
#include "render/temporal_aa_pass.h"
#include "render/fxaa_pass.h"
#include "render/sky_pass.h"
#include "render/shadow_depth_pass.h"
#include "render/shadow_blur_pass.h"
#include "render/shadow_cascade_debug_pass.h"
#include "render/screen_space_shadows_pass.h"

// Wrapper for sockets
#include "network/socket_wrapper.h"

// Thread-Safe Channel
#include "network/channel.h"

// Culling
#include "render/culling.h"

// Global State
#include "state/state.h"
#include "render/geometry_mesh_draw.h"

// Compute tessellation
#include "render/gpu_skinning.h"
#include "render/tessellation.h"

// Macro to define an event and run code in __VA_ARGS__ when it triggers
#define DEFINE_EVENT_TWO_KEYS(key_1, key_2, ...)\
	{\
		static bool was_key_pressed = false;\
		const bool is_key_1_pressed = is_key_pressed(key_1);\
		const bool is_key_2_pressed = (key_2 == SAPP_KEYCODE_INVALID) ? true : is_key_pressed(key_2);\
		if (is_key_1_pressed && is_key_2_pressed && !was_key_pressed)\
		{\
			{__VA_ARGS__}\
		}\
		was_key_pressed = is_key_1_pressed && is_key_2_pressed;\
	}


// Same as above but no mod key
#define DEFINE_EVENT_ONE_KEY(key, ...) DEFINE_EVENT_TWO_KEYS(key, SAPP_KEYCODE_INVALID, __VA_ARGS__)

// Macro to handle toggleable inputs. uses event macro above but just sets variable instead of passing in __VA_ARGS__
#define DEFINE_TOGGLE_TWO_KEYS(var_name, key_1, key_2)\
	DEFINE_EVENT_TWO_KEYS(key_1, key_2,\
		var_name = !var_name;\
	);

// Same as above but no mod key
#define DEFINE_TOGGLE_ONE_KEY(var_name, initial_state, key)\
	DEFINE_TOGGLE_TWO_KEYS(var_name, initial_state, key, SAPP_KEYCODE_INVALID,\
		var_name = !var_name;\
	)

#if WITH_DEBUG_UI

// Only include ImGui when debug UI is enabled
#define IMGUI_IMPLEMENTATION
#define SOKOL_IMGUI_IMPL
#include "imgui/misc/single_file/imgui_single_file.h"
#include "sokol/util/sokol_imgui.h"
#include "ui/cpu_profiler_ui.h"
#include "ui/stats_ui.h"

bool g_show_imgui = true;
// All imgui debug ui should be wrapped in this.
#define DEBUG_UI(...)\
	{\
		if (g_show_imgui)\
		{\
			__VA_ARGS__\
		}\
	}
#else
// when debug ui is disabled, this macro does nothing
#define DEBUG_UI(...)
#endif // WITH_DEBUG_UI

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

static void draw_debug_text(int font_index, const char* title, uint8_t r, uint8_t g, uint8_t b)
{
    sdtx_font(font_index);
    sdtx_color3b(r, g, b);
    sdtx_puts(title);
    sdtx_crlf();
}

bool register_image(const Blender::LiveLink::Image& in_image)
{
	int image_id = in_image.unique_id();

	if (state.images.id_to_index.contains(image_id))
	{
		return false;
	}
	printf("Registering image with ID: %i\n", image_id);

	auto data_vec = in_image.data();
	const i32 image_width = in_image.width();
	const i32 image_height = in_image.height();
	if (!data_vec || image_width <= 0 || image_height <= 0)
	{
		printf("Skipping image ID %i: missing data or invalid dimensions (%i x %i)\n", image_id, image_width, image_height);
		return false;
	}

	const size_t expected_byte_count = (size_t)image_width * (size_t)image_height * 4;
	if (data_vec->size() != expected_byte_count)
	{
		printf(
			"Skipping image ID %i: expected %zu RGBA8 bytes for %i x %i, received %zu\n",
			image_id,
			expected_byte_count,
			image_width,
			image_height,
			(size_t)data_vec->size()
		);
		return false;
	}

	GpuImageDesc image_desc = {
		.type = SG_IMAGETYPE_2D,
		.width = image_width,
		.height = image_height,
		.pixel_format = SG_PIXELFORMAT_RGBA8,
		.data = data_vec->data(),
	};

	const i32 array_index = state.images.items.length();
	state.images.items.add(GpuImage(image_desc));
	state.images.id_to_index[image_id] = array_index;
	return true;
}

void reset_images()
{
	state.images.id_to_index.clear();
	state.images.items.reset();
	state.images.enable_debug_fullscreen = false;
	state.images.debug_index = 0;
}

// Registers a material and adds it to our gpu buffer
bool register_material(const Blender::LiveLink::Material& in_material)
{
	int material_id = in_material.unique_id();
	if (state.materials.id_to_index.contains(material_id))
	{
		return false;
	}

	printf("Registering material with ID: %i\n", material_id);
	if (state.materials.items.length() >= MAX_MATERIALS)
	{
		printf("Error Material Limit Reached: %i\n", MAX_MATERIALS);
		exit(0);
		return false;
	}

	geometry_Material_t new_material = {
		.base_color = flatbuffer_helpers::to_hmm_vec4(in_material.base_color()),
		.emission_color = flatbuffer_helpers::to_hmm_vec4(in_material.emission_color()),
		.metallic = in_material.metallic(),
		.roughness = in_material.roughness(),
		.emission_strength = in_material.emission_strength(),
		.base_color_image_index = -1,
		.emission_color_image_index = -1,
		.metallic_image_index = -1,
		.roughness_image_index = -1,
	};

	int base_color_image_id = in_material.base_color_image_id();
	if (base_color_image_id > 0)
	{
		printf("Found base color image id: %i\n", base_color_image_id);
		assert(state.images.id_to_index.contains(base_color_image_id));
		new_material.base_color_image_index = state.images.id_to_index[base_color_image_id];
	}

	int metallic_image_id = in_material.metallic_image_id();
	if (metallic_image_id > 0)
	{
		printf("Found metallic image id: %i\n", metallic_image_id);
		assert(state.images.id_to_index.contains(metallic_image_id));
		new_material.metallic_image_index = state.images.id_to_index[metallic_image_id];
	}

	int roughness_image_id = in_material.roughness_image_id();
	if (roughness_image_id > 0)
	{
		printf("Found roughness image id: %i\n", roughness_image_id);
		assert(state.images.id_to_index.contains(roughness_image_id));
		new_material.roughness_image_index = state.images.id_to_index[roughness_image_id];
	}

	int emission_color_image_id = in_material.emission_color_image_id();
	if (emission_color_image_id > 0)
	{
		printf("Found roughness image id: %i\n", emission_color_image_id);
		assert(state.images.id_to_index.contains(emission_color_image_id));
		new_material.emission_color_image_index = state.images.id_to_index[emission_color_image_id];
	}

	const i32 array_index = state.materials.items.length();
	state.materials.items.add(new_material);
	state.materials.id_to_index[material_id] = array_index;
	return true;
}

void reset_materials()
{
	state.materials.id_to_index.clear();
	state.materials.items.reset();
	state.materials.buffer.destroy_gpu_buffer();
}

RenderPass& get_render_pass(const ERenderPass in_pass_id)
{
	return state.render_passes.passes[static_cast<int>(in_pass_id)].final_pass();
}

RenderPassEntry& get_render_pass_entry(const ERenderPass in_pass_id)
{
	return state.render_passes.passes[static_cast<int>(in_pass_id)];
}

void invalidate_shadow_cache(const bool force = false)
{
	if (!force && state.shadow.depth_freeze)
	{
		return;
	}

	ShadowDepthPass::has_valid_shadow_map = false;
	ShadowDepthPass::has_valid_shadow_blur = false;
}

void request_shadow_recapture()
{
	invalidate_shadow_cache(true);
	state.shadow.force_recapture = true;
}

void mark_scene_geometry_dirty()
{
	invalidate_shadow_cache();
	state.gi.layout_dirty = true;
	state.gi.is_updating = true;
}

void mark_lighting_dirty()
{
	state.lighting.needs_data_update = true;
}

HMM_Mat4 transform_model_matrix(const Transform& transform)
{
	HMM_Mat4 scale_matrix = HMM_Scale(transform.scale);
	HMM_Mat4 rotation_matrix = HMM_QToM4(transform.rotation);
	HMM_Mat4 translation_matrix = HMM_Translate(transform.location.XYZ);
	return HMM_MulM4(translation_matrix, HMM_MulM4(rotation_matrix, scale_matrix));
}

bool object_is_sun_light(const Object& in_object)
{
	return in_object.has_light && in_object.light.type == LightType::Sun;
}

AnimationClip* armature_get_active_animation(Armature& in_armature)
{
	if (in_armature.animation_count == 0 || !in_armature.animations)
	{
		return nullptr;
	}

	in_armature.active_animation_index = CLAMP(in_armature.active_animation_index, 0, (i32)in_armature.animation_count - 1);
	return &in_armature.animations[in_armature.active_animation_index];
}

void mesh_reset_skin_matrices(Mesh& in_mesh)
{
	if (!in_mesh.has_skinned_vertices || !in_mesh.skin_matrices)
	{
		return;
	}

	for (u32 matrix_idx = 0; matrix_idx < in_mesh.skin_matrix_count; ++matrix_idx)
	{
		in_mesh.skin_matrices[matrix_idx] = HMM_M4D(1.0f);
	}
}

void mesh_upload_skin_matrices(Mesh& in_mesh)
{
	if (!in_mesh.has_skinned_vertices || !in_mesh.skin_matrices || in_mesh.skin_matrix_count == 0)
	{
		return;
	}

	in_mesh.skin_matrix_buffer.update_gpu_buffer((sg_range) {
		.ptr = in_mesh.skin_matrices,
		.size = sizeof(HMM_Mat4) * in_mesh.skin_matrix_count,
	});
}

void rewind_skinned_animations()
{
	scene_ensure_indexes(state);
	for (const i32 unique_id : state.scene.indexes.armature_object_ids)
	{
		if (!state.scene.objects.contains(unique_id))
		{
			continue;
		}

		Object& object = state.scene.objects[unique_id];
		assert(object.has_armature);
		object.armature.playback_time = 0.0f;
		object.armature.current_frame = 0;
	}
}

void update_skinned_animations(f32 delta_time)
{
	state.animation.playback_rate = fmaxf(0.0f, state.animation.playback_rate);
	scene_ensure_indexes(state);
	state.data_oriented.frame.animation_armature_candidates += (i32)state.scene.indexes.armature_object_ids.length();

	for (const i32 unique_id : state.scene.indexes.armature_object_ids)
	{
		if (!state.scene.objects.contains(unique_id))
		{
			continue;
		}

		Object& object = state.scene.objects[unique_id];
		assert(object.has_armature);
		AnimationClip* animation = armature_get_active_animation(object.armature);
		if (!animation || animation->frame_count <= 0 || animation->frame_rate <= 0.0f)
		{
			object.armature.current_frame = 0;
			continue;
		}

		if (state.runtime.is_simulating && state.animation.is_playing && state.animation.playback_rate > 0.0f)
		{
			const f32 duration = animation->duration_seconds > 0.0f
				? animation->duration_seconds
				: (f32)animation->frame_count / animation->frame_rate;
			object.armature.playback_time += delta_time * state.animation.playback_rate;
			if (duration > 0.0f)
			{
				object.armature.playback_time = fmodf(object.armature.playback_time, duration);
			}
		}

		object.armature.current_frame = CLAMP((i32)(object.armature.playback_time * animation->frame_rate), 0, animation->frame_count - 1);
		state.data_oriented.frame.animation_armatures_updated += 1;
	}

	state.data_oriented.frame.animation_skinned_mesh_candidates += (i32)state.scene.indexes.skinned_mesh_object_ids.length();
	for (const i32 unique_id : state.scene.indexes.skinned_mesh_object_ids)
	{
		if (!state.scene.objects.contains(unique_id))
		{
			continue;
		}

		Object& object = state.scene.objects[unique_id];
		assert(object.has_mesh && object.mesh.has_skinned_vertices);
		Mesh& mesh = object.mesh;
		mesh_reset_skin_matrices(mesh);

		if (state.scene.objects.contains(mesh.armature_id))
		{
			Object& armature_object = state.scene.objects[mesh.armature_id];
			if (armature_object.has_armature)
			{
				AnimationClip* animation = armature_get_active_animation(armature_object.armature);
				if (animation && animation->skin_matrices && animation->frame_count > 0 && animation->bone_count > 0)
				{
					const i32 frame_idx = CLAMP(armature_object.armature.current_frame, 0, animation->frame_count - 1);
					const u32 bone_count = (u32)MIN(animation->bone_count, (i32)mesh.skin_matrix_count);
					for (u32 bone_idx = 0; bone_idx < bone_count; ++bone_idx)
					{
						const HMM_Mat4& armature_skin_matrix = animation->skin_matrices[frame_idx * animation->bone_count + bone_idx];
						mesh.skin_matrices[bone_idx] = HMM_MulM4(
							mesh.armature_to_mesh,
							HMM_MulM4(armature_skin_matrix, mesh.mesh_to_armature)
						);
					}
				}
			}
		}

		mesh_upload_skin_matrices(mesh);
		state.data_oriented.frame.animation_skin_matrix_uploads += 1;
	}
}

void refresh_primary_sun_id()
{
	if (state.scene.primary_sun_id.has_value())
	{
		const i32 primary_sun_id = state.scene.primary_sun_id.value();
		if (state.scene.objects.contains(primary_sun_id) && object_is_sun_light(state.scene.objects[primary_sun_id]))
		{
			return;
		}

		state.scene.primary_sun_id.reset();
	}

	scene_ensure_indexes(state);
	for (const i32 unique_id : state.scene.indexes.light_object_ids)
	{
		if (!state.scene.objects.contains(unique_id))
		{
			continue;
		}

		const Object& object = state.scene.objects[unique_id];
		if (object_is_sun_light(object))
		{
			state.scene.primary_sun_id = unique_id;
			printf("Found Primary Sun ID: %i\n", unique_id);
			return;
		}
	}
}

void parse_flatbuffer_data(StretchyBuffer<u8>& flatbuffer_data)
{
	if (flatbuffer_data.length() > 0)
	{
		State::DataOrientedState::LiveLinkImportStats import_stats = {};
		import_stats.update_index = state.data_oriented.last_import.update_index + 1;
		import_stats.byte_count = (u64)flatbuffer_data.length();

		// Interpret Flatbuffer data
		auto* update = Blender::LiveLink::GetSizePrefixedUpdate(flatbuffer_data.data());
		assert(update);
		import_stats.generation_seconds = update->generation_seconds();

		// process images from update
		if (auto images = update->images())
		{
			import_stats.image_count = images->size();
			for (i32 idx = 0; idx < images->size(); ++idx)
			{
				auto image = images->Get(idx);
				assert(image);
				if (auto data = image->data())
				{
					import_stats.image_byte_count += data->size();
				}
				register_image(*image);
			}
		}

		// process materials from update
		if (auto materials = update->materials())
		{
			import_stats.material_count = materials->size();
			bool needs_buffer_update = false;
			for (i32 idx = 0; idx < materials->size(); ++idx)
			{
				auto material = materials->Get(idx);
				assert(material);
				needs_buffer_update = register_material(*material) || needs_buffer_update;
			}

			if (needs_buffer_update)
			{
				update_materials_buffer();
			}
		}

		// process objects from update
		if (auto objects = update->objects())
		{
			import_stats.object_count = objects->size();
			for (i32 idx = 0; idx < objects->size(); ++idx)
			{
				auto object = objects->Get(idx);
				if (auto object_name = object->name())
				{
					printf("\tObject Name: %s\n", object_name->c_str());
				}

				int unique_id = object->unique_id();
				bool visibility = object->visibility();

				auto object_location = object->location();
				if (!object_location)
				{
					import_stats.malformed_object_count += 1;
					continue;
				}

				auto object_scale = object->scale();
				if (!object_scale)
				{
					import_stats.malformed_object_count += 1;
					continue;
				}

				auto object_rotation = object->rotation();
				if (!object_rotation)
				{
					import_stats.malformed_object_count += 1;
					continue;
				}

				HMM_Vec4 location 	= flatbuffer_helpers::to_hmm_vec4(object_location, 1.0f);
				HMM_Vec3 scale 		= flatbuffer_helpers::to_hmm_vec3(object_scale);
				HMM_Quat rotation 	= flatbuffer_helpers::to_hmm_quat(object_rotation);

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
					SkinnedVertex* skinned_vertices = nullptr;
					u32 skin_matrix_count = 0;

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
						import_stats.mesh_count += 1;
						import_stats.mesh_vertex_count += (i32)num_vertices;

						vertices = (Vertex*) malloc(sizeof(Vertex) * num_vertices);
						for (i32 vertex_idx = 0; vertex_idx < num_vertices; ++vertex_idx)
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
						import_stats.malformed_object_count += 1;
					}

					// Check for skinning data
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
							import_stats.malformed_object_count += 1;
						}
						else
						{
							printf("We have skinning data!\n");
							import_stats.skinned_mesh_count += 1;

							i32 max_joint_index = 0;
							skinned_vertices = (SkinnedVertex*) malloc(sizeof(SkinnedVertex) * num_vertices);
							for (i32 vertex_idx = 0; vertex_idx < num_vertices; ++vertex_idx)
							{
								for (i32 influence_idx = 0; influence_idx < 4; ++influence_idx)
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
							import_stats.malformed_object_count += 1;
						}
						else
						{
							num_indices = flatbuffer_indices->size();
							import_stats.mesh_index_count += (i32)num_indices;
							indices = (u32*) malloc(sizeof(u32) * num_indices);
							for (i32 indices_idx = 0; indices_idx < num_indices; ++indices_idx)
							{
								indices[indices_idx] = flatbuffer_indices->Get(indices_idx);
							}
						}
					}

					u32 num_material_indices = 0;
					i32* material_indices = nullptr;
					if (auto flatbuffer_material_ids = object_mesh->material_ids())
					{
						num_material_indices = flatbuffer_material_ids->size();
						material_indices = (i32*) malloc(sizeof(i32) * num_material_indices);
						for (i32 material_id_idx = 0; material_id_idx < num_material_indices; ++material_id_idx)
						{
							material_indices[material_id_idx] = -1;
		   					const int material_id = flatbuffer_material_ids->Get(material_id_idx);
							if (!state.materials.id_to_index.contains(material_id))
							{
								printf("\tFailed to find material with id: %i\n", material_id);
								continue;
							}
							material_indices[material_id_idx] = state.materials.id_to_index[material_id];
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
							.skinned_vertices = skinned_vertices,
							.skin_matrix_count = skin_matrix_count,
							.armature_id = armature_id,
							.mesh_to_armature = flatbuffer_helpers::to_hmm_mat4(object_mesh->mesh_to_armature()),
							.armature_to_mesh = flatbuffer_helpers::to_hmm_mat4(object_mesh->armature_to_mesh()),

							.num_material_indices = num_material_indices,
							.material_indices = material_indices,
						};
						game_object.mesh = make_mesh(mesh_init_data);
						game_object.has_mesh = true;
					}
					else
					{
						free(indices);
						free(vertices);
						free(skinned_vertices);
						free(material_indices);
					}
				}

				if (auto object_armature = object->armature())
				{
					import_stats.armature_count += 1;
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
						import_stats.animation_count += flatbuffer_animations->size();
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
							import_stats.animation_matrix_count += matrix_count;
							if (matrix_count > 0)
							{
								animation.skin_matrices = (HMM_Mat4*) malloc(sizeof(HMM_Mat4) * matrix_count);
								for (i32 matrix_idx = 0; matrix_idx < matrix_count; ++matrix_idx)
								{
									animation.skin_matrices[matrix_idx] = HMM_M4D(1.0f);
								}

								if (auto flatbuffer_skin_matrices = flatbuffer_animation->skin_matrices())
								{
									const i32 available_float_count = flatbuffer_skin_matrices->size();
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
					import_stats.light_count += 1;
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
					game_object.rigid_body = (RigidBody){
						.is_dynamic = object_rigid_body->is_dynamic(),
						.mass = object_rigid_body->mass(),
						.jolt_body = nullptr,
					};
				}

				// Custom gameplay components we've specified on our blender objects
				if (auto object_components = object->components())
				{
					i32 num_components = object_components->size();
					for (i32 component_idx = 0; component_idx < num_components; ++component_idx)
					{
						auto component_container = object_components->Get(component_idx);
						if (!component_container)  { continue; }
						auto component = component_container->value();
						if (!component) { continue; }

						auto component_type = component_container->value_type();
						switch (component_type)
						{
							case Blender::LiveLink::GameplayComponent_GameplayComponentCharacter:
							{
								using Blender::LiveLink::GameplayComponentCharacter;
								const GameplayComponentCharacter* character_component = reinterpret_cast<const GameplayComponentCharacter*>(component);

								printf("\t\tCharacter Component\n");
								printf("\t\t\t Player Controlled: %s\n", character_component->player_controlled() ? "true" : "false");
								printf("\t\t\t Move Speed: %f\n", character_component->move_speed());

								CharacterSettings character_settings = {
									.initial_location = game_object.current_transform.location,
									.initial_rotation = game_object.current_transform.rotation,
									.player_controlled = character_component->player_controlled(),
									.move_speed = character_component->move_speed(),
									.jump_speed = character_component->jump_speed(),
								};
								object_add_character(game_object, character_settings);

								if (character_settings.player_controlled)
								{
									state.scene.player_character_id = game_object.unique_id;
								}
								break;
							}
							case Blender::LiveLink::GameplayComponent_GameplayComponentCameraControl:
							{
								using Blender::LiveLink::GameplayComponentCameraControl;
								const GameplayComponentCameraControl* cam_control_component = reinterpret_cast<const GameplayComponentCameraControl*>(component);

								const f32 cam_control_follow_distance = cam_control_component->follow_distance();
								const f32 cam_control_follow_speed = cam_control_component->follow_speed();

								printf("\t\tCamera Control Component\n");
								printf("\t\t\t Follow Distance: %f\n", cam_control_follow_distance);
								printf("\t\t\t Follow Speed: %f\n", cam_control_follow_speed);

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
								assert(false);
						}
					}
				}

				// Send updated object data to main thread
				state.live_link.updated_objects.send(game_object);
			}

			state.runtime.blender_data_loaded = true;
		}

		if (auto deleted_object_uids = update->deleted_object_uids())
		{
			import_stats.deleted_object_count = deleted_object_uids->size();
			for (i32 deleted_object_uid : *deleted_object_uids)
			{
				state.live_link.deleted_objects.send(deleted_object_uid);
			}
		}

		if (update->reset())
		{
			import_stats.reset = true;
			state.live_link.reset.send(true);
		}

		state.data_oriented.last_import = import_stats;
		state.data_oriented.import_history.add(import_stats);
		state.data_oriented.selected_import_history_index = (i32)state.data_oriented.import_history.length() - 1;
		printf(
			"Live Link Import Stats #%llu: bytes=%llu generation_seconds=%.6f objects=%i deleted=%i meshes=%i verts=%i indices=%i skinned=%i lights=%i armatures=%i animations=%i matrices=%i materials=%i images=%i image_bytes=%llu malformed=%i reset=%s\n",
			(unsigned long long)import_stats.update_index,
			(unsigned long long)import_stats.byte_count,
			import_stats.generation_seconds,
			import_stats.object_count,
			import_stats.deleted_object_count,
			import_stats.mesh_count,
			import_stats.mesh_vertex_count,
			import_stats.mesh_index_count,
			import_stats.skinned_mesh_count,
			import_stats.light_count,
			import_stats.armature_count,
			import_stats.animation_count,
			import_stats.animation_matrix_count,
			import_stats.material_count,
			import_stats.image_count,
			(unsigned long long)import_stats.image_byte_count,
			import_stats.malformed_object_count,
			import_stats.reset ? "true" : "false"
		);
	}
}

// Live Link Function. Runs on its own thread
void live_link_thread_function()
{
	socket_lib_init();

	// Init socket we'll use to talk to blender
	struct addrinfo hints, *res;
	// first, load up address structs with getaddrinfo():

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	//FCS TODO: Store magic IP and Port numbers in some shared file
	const char* HOST = "127.0.0.1";
	const char* PORT = "65432";
	getaddrinfo(HOST, PORT, &hints, &res);

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
	while(!socket_is_valid(state.live_link.connection_socket) && state.runtime.game_running);

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
					|| 	last_error == socket_error_would_block()
					|| 	last_error == socket_error_timed_out())
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

void update_render_resolution()
{
	state.window.target_render_width = std::max(1, state.window.target_render_width);
	state.window.target_render_height = std::max(1, state.window.target_render_height);

	if (state.window.width <= 0 || state.window.height <= 0)
	{
		state.window.render_width = state.window.target_render_width;
		state.window.render_height = state.window.target_render_height;
		return;
	}

	const f32 window_aspect = (f32)state.window.width / (f32)state.window.height;
	const f32 target_aspect = (f32)state.window.target_render_width / (f32)state.window.target_render_height;

	if (window_aspect > target_aspect)
	{
		state.window.render_height = state.window.target_render_height;
		state.window.render_width = std::max(1, (i32)((f32)state.window.target_render_height * window_aspect + 0.5f));
	}
	else
	{
		state.window.render_width = state.window.target_render_width;
		state.window.render_height = std::max(1, (i32)((f32)state.window.target_render_width / window_aspect + 0.5f));
	}
}

void handle_resize(bool force_resize = false)
{
	int new_width = sapp_width();
    int new_height = sapp_height();

	if (force_resize || new_width != state.window.width || new_height != state.window.height)
	{
		state.window.width = new_width;
		state.window.height = new_height;
		update_render_resolution();

		const int render_pass_count = (int) ERenderPass::COUNT;
		for (i32 pass_index = 0; pass_index < render_pass_count; ++pass_index)
		{
			RenderPassEntry& render_pass_entry = state.render_passes.passes[pass_index];
			RenderPass& render_pass = render_pass_entry.final_pass();
			if (render_pass.desc.type == ERenderPassType::Swapchain)
			{
				render_pass_entry.handle_resize(state.window.width, state.window.height);
			}
			else
			{
				render_pass_entry.handle_resize(state.window.render_width, state.window.render_height);
			}
		}

		TemporalAAPass::invalidate_history(state);
	}
}

GI_Scene gi_scene = {};

void init(void)
{
	stm_setup();

	jolt_init();

	// Init sokol graphics
    sg_setup((sg_desc) {
		.buffer_pool_size = 4096,
		.image_pool_size = 4096,
		//.sampler_pool_size = 1024,
		.shader_pool_size = 128,
		.pipeline_pool_size = 256,
		.view_pool_size = 8192,
        .logger.func = slog_func,
	   	.environment = sglue_environment(),
    });
	gpu_frame_timings_init();

	sg_swapchain swapchain = sglue_swapchain();
	const sg_pixel_format scene_color_format = SG_PIXELFORMAT_RGBA32F;

	state_init();

	// Spin up a thread that blocks until we receive our init event, and then listens for updates
	state.live_link.thread = std::thread(live_link_thread_function);

	// GI Scene Setup
	gi_scene_init(gi_scene, state);
	printf("gi_scene num octree nodes: %zu num payloads: %d num probes: %d\n",
		gi_scene.octree_nodes.length(),
		gi_scene.payload_count,
		gi_scene.non_fallback_probe_count
	);

	#if WITH_DEBUG_UI
	// Init sokol imgui integration
	simgui_setup((simgui_desc_t) {
		.ini_filename = "bin/imgui.ini",
	});
	#endif // WITH_DEBUG_UI

	// Check for Storage Buffer Support
	if (!sg_query_features().compute)
	{
		printf("Sokol Gfx Error: Compute/Storage Buffers are Required\n");
		exit(0);
	}

	Tessellation::init();

	// setup sokol-debugtext
    sdtx_setup((sdtx_desc_t){
        .fonts = {
            [FONT_KC853] = sdtx_font_kc853(),
            [FONT_KC854] = sdtx_font_kc854(),
            [FONT_Z1013] = sdtx_font_z1013(),
            [FONT_CPC]   = sdtx_font_cpc(),
            [FONT_C64]   = sdtx_font_c64(),
            [FONT_ORIC]  = sdtx_font_oric()
        },
        .logger.func = slog_func,
    });

	// Init shadow depth pass
	get_render_pass(ERenderPass::ShadowDepth).init(ShadowDepthPass::make_render_pass_desc(SG_PIXELFORMAT_DEPTH));

	// Init shadow VSM blur pass
	ShadowBlurPass::init_separable(get_render_pass_entry(ERenderPass::ShadowBlur));

	// Init shadow cascade debug pass
	get_render_pass(ERenderPass::ShadowCascadeDebug).init(ShadowCascadeDebugPass::make_render_pass_desc());

	// Init main geometry pass
	get_render_pass(ERenderPass::Geometry).init(GeometryPass::make_render_pass_desc(swapchain.depth_format));

	const sg_pixelformat_info ssao_r8_info = sg_query_pixelformat(SG_PIXELFORMAT_R8);
	const sg_pixel_format ssao_pixel_format = (ssao_r8_info.render && ssao_r8_info.filter)
		? SG_PIXELFORMAT_R8
		: SG_PIXELFORMAT_RGBA8;

	RenderPassDesc ssao_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(ssao_ssao_shader_desc(sg_query_backend())),
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.color_count = 1,
			.colors[0] = {
				.pixel_format = ssao_pixel_format,
			},
			.cull_mode = SG_CULLMODE_NONE,
			.label = "ssao-pipeline",
		},
		.num_outputs = 1,
		.outputs[0] = {
			.pixel_format = ssao_pixel_format,
			.load_action = SG_LOADACTION_CLEAR,
			.store_action = SG_STOREACTION_STORE,
			.clear_value = {1.0, 1.0, 1.0, 1.0},
		},
		.width_scale = 0.5f,
		.height_scale = 0.5f,
		.debug_label = "SSAO",
	};
	get_render_pass(ERenderPass::SSAO).init(ssao_pass_desc);

	{	//SSAO Noise Texture
		std::uniform_real_distribution<f32> randomf32s(0.0, 1.0); // random f32s between [0.0, 1.0]
		std::default_random_engine generator;
		HMM_Vec4 ssao_noise[SSAO_TEXTURE_SIZE];
		for (u32 i = 0; i < SSAO_TEXTURE_SIZE; ++i)
		{
			ssao_noise[i] = HMM_V4(
				randomf32s(generator) * 2.0f - 1.0f,
				randomf32s(generator) * 2.0f - 1.0f,
				0.0f,
				0.0f
			);
		}

		GpuImageDesc ssao_noise_desc = {
			.width = SSAO_TEXTURE_WIDTH,
			.height = SSAO_TEXTURE_WIDTH,
			.pixel_format = SG_PIXELFORMAT_RGBA32F,
			.data = (u8*) &ssao_noise,
			.label = "ssao_noise_texture"
		};
		state.ssao.noise_texture = GpuImage(ssao_noise_desc);

		// Init ssao_fs_params
		state.ssao.fs_params = {
			.screen_size = HMM_V2((f32)state.window.render_width, (f32)state.window.render_height),
		};

		//SSAO Kernel
		for (u32 i = 0; i < SSAO_KERNEL_SIZE; ++i)
		{
			HMM_Vec3 sample = HMM_V3(
				randomf32s(generator) * 2.0f - 1.0f,
				randomf32s(generator) * 2.0f - 1.0f,
				randomf32s(generator)
			);
			sample = HMM_NormV3(sample);
			sample *= randomf32s(generator);

			// scale samples s.t. they're more aligned to center of kernel
			f32 scale = f32(i) / (f32) SSAO_KERNEL_SIZE;
			scale = HMM_Lerp(0.1f, scale * scale, 1.0f);
			sample *= scale;

			state.ssao.fs_params.kernel_samples[i] = HMM_V4(sample.X, sample.Y, sample.Z, 0.0);
		}
	}

	BlurPass::init_separable(
		get_render_pass_entry(ERenderPass::SSAO_Blur),
		ssao_pixel_format,
		0.5f,
		0.5f,
		"SSAO Blur Horizontal",
		"SSAO Blur Vertical"
	);

	ScreenSpaceShadowsPass::init(get_render_pass_entry(ERenderPass::ScreenSpaceShadows), ssao_pixel_format);

	get_render_pass(ERenderPass::Lighting).init(LightingPass::make_render_pass_desc(scene_color_format));

	RenderPassDesc dof_combine_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(dof_combine_dof_combine_shader_desc(sg_query_backend())),
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.color_count = 1,
			.colors[0] = {
				.pixel_format = scene_color_format,
			},
			.cull_mode = SG_CULLMODE_NONE,
			.label = "dof-combine-pipeline",
		},
		.num_outputs = 1,
		.outputs[0] = {
			.pixel_format = scene_color_format,
			.load_action = SG_LOADACTION_DONTCARE,
			.store_action = SG_STOREACTION_STORE,
		},
		.debug_label = "DOF Combine",
	};
	get_render_pass(ERenderPass::DOF_Combine).init(dof_combine_pass_desc);

	get_render_pass(ERenderPass::WireOverlay).init(WireOverlayPass::make_render_pass_desc(scene_color_format));

	get_render_pass(ERenderPass::TemporalAA).init(TemporalAAPass::make_render_pass_desc(scene_color_format));

	RenderPassDesc tonemapping_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(tonemapping_tonemapping_shader_desc(sg_query_backend())),
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.color_count = 1,
			.colors[0] = {
				.pixel_format = swapchain.color_format,
			},
			.cull_mode = SG_CULLMODE_NONE,
			.label = "tonemapping-pipeline",
		},
		.num_outputs = 1,
		.outputs[0] = {
			.pixel_format = swapchain.color_format,
			.load_action = SG_LOADACTION_DONTCARE,
			.store_action = SG_STOREACTION_STORE,
		},
		.debug_label = "Tonemapping",
	};
	get_render_pass(ERenderPass::Tonemapping).init(tonemapping_pass_desc);

	get_render_pass(ERenderPass::FXAA).init(FXAAPass::make_render_pass_desc(swapchain.color_format));

	RenderPassDesc debug_text_pass_desc = {
		// Don't set optional pipeline_desc
		.num_outputs = 1,
		.outputs[0] = {
			.pixel_format = swapchain.color_format,
			.load_action = SG_LOADACTION_CLEAR,
			.store_action = SG_STOREACTION_STORE,
			.clear_value = {0.0, 0.0, 0.0, 0.0},
		},
		.depth_output = {
			.pixel_format = swapchain.depth_format,
		},
		.debug_label = "Debug Text",
	};
	get_render_pass(ERenderPass::DebugText).init(debug_text_pass_desc);

	RenderPassDesc copy_to_swapchain_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(overlay_texture_overlay_texture_shader_desc(sg_query_backend())),
			.depth = {
				.pixel_format = swapchain.depth_format,
				.compare = SG_COMPAREFUNC_ALWAYS,
				.write_enabled = false,
			},
			.color_count = 1,
			.colors[0] = {
				.pixel_format = swapchain.color_format,
			},
			.cull_mode = SG_CULLMODE_NONE,
			.label = "copy-to-swapchain-pipeline",
		},
		.depth_output = {
			.pixel_format = swapchain.depth_format,
		},
		.type = ERenderPassType::Swapchain,
		.debug_label = "Copy To Swapchain",
	};
	get_render_pass(ERenderPass::CopyToSwapchain).init(copy_to_swapchain_pass_desc);

	handle_resize();

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
			parse_flatbuffer_data(flatbuffer_data);
		}
	}
}

struct AppState {
	bool keycodes[SAPP_MAX_KEYCODES];

	HMM_Vec2 mouse_position;
	HMM_Vec2 mouse_delta;

	bool is_mouse_locked = false;
} app_state;

bool is_key_pressed(sapp_keycode keycode)
{
	assert((i32)keycode < SAPP_MAX_KEYCODES);
	return app_state.keycodes[keycode];
}

HMM_Vec2 get_mouse_delta()
{
	return app_state.mouse_delta;
}

void reset_mouse_delta()
{
	app_state.mouse_delta = HMM_V2(0.f,0.f);
}

bool is_mouse_locked()
{
	return app_state.is_mouse_locked;
}

void set_mouse_locked(bool in_locked)
{
	app_state.is_mouse_locked = in_locked;
	sapp_lock_mouse(app_state.is_mouse_locked);
}

bool ray_sphere_intersect(
	const HMM_Vec3& in_ray_origin,
	const HMM_Vec3& in_ray_direction,
	const HMM_Vec3& in_sphere_center,
	const f32 in_sphere_radius,
	f32& out_t
)
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
	if (t < 0.0f)
	{
		t = -b + sqrt_discriminant;
	}
	if (t < 0.0f)
	{
		return false;
	}

	out_t = t;
	return true;
}

void pick_isolated_gi_probe()
{
	const f32 w = sapp_widthf();
	const f32 h = sapp_heightf();
	if (w <= 0.0f || h <= 0.0f)
	{
		return;
	}

	const f32 fov = HMM_AngleDeg(60.0f);
	const f32 aspect_ratio = w / h;

	Camera& camera = get_active_camera();
	const f32 ndc_x = (2.0f * app_state.mouse_position.X / w) - 1.0f;
	const f32 ndc_y = 1.0f - (2.0f * app_state.mouse_position.Y / h);

	const f32 tan_half_fov = HMM_TanF(fov * 0.5f);
	const HMM_Vec3 camera_forward = HMM_NormV3(camera.forward);
	const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera_forward, camera.up));
	const HMM_Vec3 camera_up = HMM_NormV3(HMM_Cross(camera_right, camera_forward));
	const HMM_Vec3 ray_origin = camera.location;
	const HMM_Vec3 ray_direction = HMM_NormV3(
		camera_forward +
		camera_right * (ndc_x * aspect_ratio * tan_half_fov) +
		camera_up * (ndc_y * tan_half_fov)
	);
	i32 closest_probe_index = -1;
	f32 closest_t = std::numeric_limits<f32>::max();
	for (i32 probe_index = 0; probe_index < gi_scene.non_fallback_probe_count; ++probe_index)
	{
		f32 t = 0.0f;
		const HMM_Vec3 probe_position = gi_scene_probe_position_from_index(gi_scene, probe_index);
		const f32 probe_radius = gi_scene_debug_probe_radius_for_probe(gi_scene, probe_index);
		if (ray_sphere_intersect(ray_origin, ray_direction, probe_position, probe_radius, t) && t < closest_t)
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

void update_physics_backed_object_transforms()
{
	JPH::BodyInterface& body_interface = jolt_state.physics_system.GetBodyInterface();

	for (auto& [unique_id, object] : state.scene.objects)
	{
		state.data_oriented.frame.object_update_scan_count += 1;

		object_copy_physics_transform(object, body_interface);

		if (object.storage_buffer_needs_update)
		{
			object.storage_buffer_needs_update = false;
			object_update_storage_buffer(object);
			state.data_oriented.frame.object_update_storage_updates += 1;
		}
	}
}

void update_player_character_control(f32 delta_time)
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
	if (is_key_pressed(SAPP_KEYCODE_W))
	{
		move_direction += projected_cam_forward;
	}
	if (is_key_pressed(SAPP_KEYCODE_S))
	{
		move_direction -= projected_cam_forward;
	}
	if (is_key_pressed(SAPP_KEYCODE_D))
	{
		move_direction += projected_cam_right;
	}
	if (is_key_pressed(SAPP_KEYCODE_A))
	{
		move_direction -= projected_cam_right;
	}

	if (HMM_LenSqrV3(move_direction) > 1.0f)
	{
		move_direction = HMM_NormV3(move_direction);
	}

	if (is_key_pressed(SAPP_KEYCODE_LEFT_SHIFT))
	{
		move_direction *= 3.0f;
	}

	const bool jump = is_key_pressed(SAPP_KEYCODE_SPACE);
	character_move(player_character_state, move_direction, jump, (f32)delta_time);
}

void frame(void)
{
	CPU_TIMING_FRAME("Frame");
	const i64 gpu_timing_frame_index = cpu_timings_get_current_frame_index();
	gpu_frame_timings_begin_frame(gpu_timing_frame_index);

	// Delta Time Calculation
	static u64 last_frame_time = 0;
	const u64 lap_time = stm_laptime(&last_frame_time);
	const double delta_time = stm_sec(lap_time);
    const float ui_dpi_scale = sapp_dpi_scale();
	data_oriented_begin_frame(state);

	state.debug_ui.stats_sample_elapsed += delta_time;
	state.debug_ui.stats_sample_count += 1;
	if (state.debug_ui.fps == 0.0f && delta_time > 0.0)
	{
		state.debug_ui.frame_time_ms = delta_time * 1000.0;
		state.debug_ui.fps = 1.0 / delta_time;
	}
	if (state.debug_ui.stats_sample_elapsed >= 0.25)
	{
		const double average_delta_time = state.debug_ui.stats_sample_elapsed / state.debug_ui.stats_sample_count;
		state.debug_ui.frame_time_ms = average_delta_time * 1000.0;
		state.debug_ui.fps = state.debug_ui.stats_sample_count / state.debug_ui.stats_sample_elapsed;
		state.debug_ui.stats_sample_elapsed = 0.0;
		state.debug_ui.stats_sample_count = 0;
	}

	DEBUG_UI(
		simgui_new_frame((simgui_frame_desc_t){
			.width = state.window.width,
			.height = state.window.height,
			.delta_time = delta_time,
			.dpi_scale = ui_dpi_scale,
		});
	);

	{
		CPU_TIMING_SCOPE("Live Link");

		// Receive Any Updated Objects
		while (optional<Object> received_updated_object = state.live_link.updated_objects.receive())
		{
			Object& updated_object = *received_updated_object;
			i32 updated_object_uid = updated_object.unique_id;
			state.data_oriented.frame.live_link_updated_objects += 1;

			printf("Updating Object. UID: %i\n", updated_object_uid);

			// Cleanup old object
			bool gi_scene_geometry_changed = object_contributes_to_gi_scene(updated_object);
			if (state.scene.objects.contains(updated_object_uid))
			{
				Object& existing_object = state.scene.objects[updated_object_uid];
				gi_scene_geometry_changed = gi_scene_geometry_changed || object_contributes_to_gi_scene(existing_object);
				object_cleanup(existing_object);
			}

			if (updated_object.has_light)
			{
				mark_lighting_dirty();
			}

			if (updated_object.has_rigid_body)
			{
				object_add_jolt_body(updated_object);
			}

			if (gi_scene_geometry_changed)
			{
				mark_scene_geometry_dirty();
			}

			state.scene.objects[updated_object_uid] = updated_object;
			scene_mark_indexes_dirty(state);
		}

		// Receive Any Deleted Objects
		while (optional<i32> received_deleted_object = state.live_link.deleted_objects.receive())
		{
			i32 deleted_object_uid = *received_deleted_object;
			state.data_oriented.frame.live_link_deleted_objects += 1;
			if (state.scene.objects.contains(deleted_object_uid))
			{
				printf("Removing object. UID: %i\n", deleted_object_uid);
				Object& object_to_delete = state.scene.objects[deleted_object_uid];

				if (object_to_delete.has_light)
				{
					mark_lighting_dirty();
				}

				if (object_contributes_to_gi_scene(object_to_delete))
				{
					mark_scene_geometry_dirty();
				}

				object_cleanup(object_to_delete);
				state.scene.objects.erase(deleted_object_uid);
				scene_mark_indexes_dirty(state);
			}
		}

		// Receive any Reset Messages
		while(optional<bool> received_reset = state.live_link.reset.receive())
		{
			state.data_oriented.frame.live_link_reset_count += 1;
			state.runtime.blender_data_loaded = false;
			mark_lighting_dirty();
			mark_scene_geometry_dirty();

			for (auto& [unique_id, object] : state.scene.objects)
			{
				if (object.has_rigid_body)
				{
					object_remove_jolt_body(object);
				}

				object_cleanup_gpu_resources(object);
			}

			state.scene.objects.clear();
			state.scene.camera_control_id.reset();
			state.scene.player_character_id.reset();
			state.scene.primary_sun_id.reset();
			scene_reset_indexes(state);

			reset_materials();
			reset_images();
			TemporalAAPass::invalidate_history(state);
		}

		scene_ensure_indexes(state);
	}

	refresh_primary_sun_id();

	// Space Bar + Left Control Starts/Stops simulation
	DEFINE_TOGGLE_TWO_KEYS(state.runtime.is_simulating, SAPP_KEYCODE_SPACE, SAPP_KEYCODE_LEFT_CONTROL);

	#if WITH_DEBUG_UI
	// Control + I toggles imgui debug window
	DEFINE_TOGGLE_TWO_KEYS(g_show_imgui, SAPP_KEYCODE_I, SAPP_KEYCODE_LEFT_CONTROL);
	#endif // WITH_DEBUG_UI

	// D + Left Control toggle debug camera
	DEFINE_EVENT_TWO_KEYS(SAPP_KEYCODE_D, SAPP_KEYCODE_LEFT_CONTROL,
		if (!state.debug_camera.active) state.debug_camera.camera = get_active_camera();
		state.debug_camera.active = !state.debug_camera.active;
	);

	// Reset State
	DEFINE_EVENT_TWO_KEYS(SAPP_KEYCODE_R, SAPP_KEYCODE_LEFT_CONTROL,
		// Reset object transforms and recreate physics state
		for (auto& [unique_id, object] : state.scene.objects)
		{
			object.current_transform = object.initial_transform;
			object.storage_buffer_needs_update = true;

			if (object.has_rigid_body)
			{
				object_reset_jolt_body(object);
			}
		}
		TemporalAAPass::invalidate_history(state);
	);

	DEBUG_UI(
		ImGui::Begin("DEBUG");

		if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Text("Window Resolution: %d x %d", state.window.width, state.window.height);
			ImGui::Text("Render Resolution: %d x %d", state.window.render_width, state.window.render_height);

			ImGui::Text("Target Resolution:");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(96.0f);
			const bool target_width_changed = ImGui::InputInt("##Target Resolution Width", &state.window.target_render_width);
			const bool target_width_active = ImGui::IsItemActive();
			ImGui::SameLine();
			ImGui::Text("x");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(96.0f);
			const bool target_height_changed = ImGui::InputInt("##Target Resolution Height", &state.window.target_render_height);
			const bool target_height_active = ImGui::IsItemActive();
			ImGui::Checkbox("Maintain Target Aspect Ratio", &state.window.maintain_target_aspect_ratio);

			if (state.window.maintain_target_aspect_ratio)
			{
				if (target_width_changed && !target_height_changed)
				{
					state.window.target_render_height = std::max(1, (i32)((f32)state.window.target_render_width / state.window.target_render_aspect_ratio + 0.5f));
				}
				else if (target_height_changed && !target_width_changed)
				{
					state.window.target_render_width = std::max(1, (i32)((f32)state.window.target_render_height * state.window.target_render_aspect_ratio + 0.5f));
				}
			}

			if (!target_width_active && !target_height_active)
			{
				const i32 target_render_width = std::max(1, state.window.target_render_width);
				const i32 target_render_height = std::max(1, state.window.target_render_height);
				state.window.target_render_aspect_ratio = (f32)target_render_width / (f32)target_render_height;
			}

			const bool target_resolution_changed = target_width_changed || target_height_changed;
			if (target_resolution_changed)
			{
				handle_resize(true);
			}
			ImGui::Text("frame time: %.2f ms", state.debug_ui.frame_time_ms);
			ImGui::Text("FPS: %.1f", state.debug_ui.fps);
			ImGui::Spacing();
		    ImGui::Checkbox("Profiler", &state.debug_ui.show_profiler);
			ImGui::Spacing();

			draw_stats_ui(state);
		}

		if (ImGui::CollapsingHeader("Animation", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (ImGui::Button("Play"))
			{
				state.animation.is_playing = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("Pause"))
			{
				state.animation.is_playing = false;
			}
			ImGui::SameLine();
			if (ImGui::Button("Rewind"))
			{
				rewind_skinned_animations();
			}

			ImGui::SetNextItemWidth(160.0f);
			ImGui::DragFloat("Playback Rate", &state.animation.playback_rate, 0.01f, 0.0f, 4.0f, "%.2fx");
			ImGui::Checkbox("Skinning Debug View", &state.animation.skinning_debug_view);

			f32 animation_label_width = 0.0f;
			scene_ensure_indexes(state);
			for (const i32 unique_id : state.scene.indexes.armature_object_ids)
			{
				if (!state.scene.objects.contains(unique_id))
				{
					continue;
				}

				Object& object = state.scene.objects[unique_id];
				assert(object.has_armature);
				if (object.armature.animation_count == 0 || !object.armature.animations)
				{
					continue;
				}

				const char* object_name = object.name ? object.name : "<Unnamed Object>";
				const std::string animation_label = std::string(object_name) + " Animation:";
				const f32 label_width = ImGui::CalcTextSize(animation_label.c_str()).x;
				if (label_width > animation_label_width)
				{
					animation_label_width = label_width;
				}
			}

			const f32 animation_combo_x = ImGui::GetCursorPosX() + animation_label_width + ImGui::GetStyle().ItemSpacing.x;
			for (const i32 unique_id : state.scene.indexes.armature_object_ids)
			{
				if (!state.scene.objects.contains(unique_id))
				{
					continue;
				}

				Object& object = state.scene.objects[unique_id];
				assert(object.has_armature);
				if (object.armature.animation_count == 0 || !object.armature.animations)
				{
					continue;
				}

				const char* object_name = object.name ? object.name : "<Unnamed Object>";
				const std::string animation_label = std::string(object_name) + " Animation:";
				ImGui::PushID(unique_id);
				ImGui::TextUnformatted(animation_label.c_str());
				ImGui::SameLine(animation_combo_x);
				ImGui::SetNextItemWidth(180.0f);

				i32 selected_animation_index = CLAMP(
					object.armature.active_animation_index,
					0,
					(i32)object.armature.animation_count - 1
				);
				const char* selected_animation_name = object.armature.animations[selected_animation_index].name
					? object.armature.animations[selected_animation_index].name
					: "<Unnamed Animation>";

				if (ImGui::BeginCombo("##Animation", selected_animation_name))
				{
					for (u32 animation_idx = 0; animation_idx < object.armature.animation_count; ++animation_idx)
					{
						const AnimationClip& animation = object.armature.animations[animation_idx];
						const char* animation_name = animation.name ? animation.name : "<Unnamed Animation>";
						const bool is_selected = selected_animation_index == (i32)animation_idx;
						if (ImGui::Selectable(animation_name, is_selected))
						{
							object.armature.active_animation_index = (i32)animation_idx;
							object.armature.playback_time = 0.0f;
							object.armature.current_frame = 0;
						}
						if (is_selected)
						{
							ImGui::SetItemDefaultFocus();
						}
					}
					ImGui::EndCombo();
				}
				ImGui::PopID();
			}
		}

		if (ImGui::CollapsingHeader("Rendering Features", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Indent();
			if (ImGui::CollapsingHeader("Tessellation", ImGuiTreeNodeFlags_DefaultOpen))
			{
				bool tessellation_changed = false;
				tessellation_changed |= ImGui::Checkbox("Enable Tessellation", &state.tessellation.enabled);
				tessellation_changed |= ImGui::Combo("Mode", (i32*) &state.tessellation.mode, ETessellationModeNames, IM_ARRAYSIZE(ETessellationModeNames));
				tessellation_changed |= ImGui::SliderInt("Fixed Factor", &state.tessellation.fixed_factor, 1, state.tessellation.max_factor);
				tessellation_changed |= ImGui::SliderInt("Max Factor", &state.tessellation.max_factor, 1, (i32) Tessellation::MAX_FACTOR);
				tessellation_changed |= ImGui::SliderFloat("Target Segment", &state.tessellation.target_pixels_per_segment, 1.0f, 64.0f, "%.1f px");
				tessellation_changed |= ImGui::SliderFloat("Phong Strength", &state.tessellation.phong_strength, 0.0f, 1.0f, "%.2f");
				tessellation_changed |= ImGui::Checkbox("Virtual Patches", &state.tessellation.virtual_patches_enabled);
				tessellation_changed |= ImGui::SliderInt("Virtual Patch Depth", &state.tessellation.virtual_patch_max_depth, 0, 4);
				tessellation_changed |= ImGui::DragInt("Max Patches", &state.tessellation.max_generated_patches, 256.0f, 1, 1024 * 1024);
				tessellation_changed |= ImGui::DragInt("Max Vertices", &state.tessellation.max_generated_vertices, 1024.0f, 3, 64 * 1024 * 1024);
				tessellation_changed |= ImGui::DragInt("Max Indices", &state.tessellation.max_generated_indices, 1024.0f, 3, 128 * 1024 * 1024);
				tessellation_changed |= ImGui::SliderFloat("Bounds Padding", &state.tessellation.bounds_padding, 0.0f, 10.0f, "%.2f");

				ImGui::Text("Meshes: %d  Overflow: %d", state.tessellation.mesh_count, state.tessellation.overflowed_mesh_count);
				ImGui::Text("Source Tris: %d  Patches: %d", state.tessellation.source_triangle_count, state.tessellation.patch_count);
				ImGui::Text("Generated: %d verts / %d indices", state.tessellation.generated_vertex_count, state.tessellation.generated_index_count);
				ImGui::Text("Max Factor: %d", state.tessellation.max_factor_seen);
				ImGui::Text("Readback: %s  Age: %d", state.tessellation.readback_supported ? "Supported" : "Unsupported", state.tessellation.readback_age);

				if (tessellation_changed)
				{
					invalidate_shadow_cache();
				}
			}

			if (ImGui::CollapsingHeader("Wireframe", ImGuiTreeNodeFlags_DefaultOpen))
			{
				if (ImGui::Checkbox("Shaded Wireframe", &state.wireframe.shaded_wireframe))
				{
					TemporalAAPass::invalidate_history(state);
				}
				ImGui::SliderFloat("Wire Width", &state.wireframe.width, 0.5f, 4.0f, "%.2f px");
				ImGui::SliderFloat("Wire Softness", &state.wireframe.softness, 0.25f, 3.0f, "%.2f px");
				ImGui::SliderFloat("Wire Opacity", &state.wireframe.opacity, 0.0f, 1.0f, "%.2f");
				ImGui::ColorEdit3("Wire Color", &state.wireframe.color.X);
			}

			ImGui::Separator();

			if (ImGui::CollapsingHeader("Image Effects", ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::SliderFloat("Exposure (EV)", &state.tonemapping.fs_params.exposure_bias, -5.0f, 5.0f, "%.2f stops");
				if (ImGui::CollapsingHeader("Antialiasing", ImGuiTreeNodeFlags_DefaultOpen))
				{
					bool taa_changed = false;
					taa_changed |= ImGui::Checkbox("Temporal AA", &state.temporal_aa.enable);
					ImGui::Checkbox("FXAA", &state.temporal_aa.enable_fxaa);
					ImGui::BeginDisabled(!state.temporal_aa.enable);
					taa_changed |= ImGui::SliderFloat("TAA History Blend", &state.temporal_aa.blend_alpha, 0.0f, 1.0f, "%.2f");
					taa_changed |= ImGui::SliderFloat("TAA Sharpen", &state.temporal_aa.sharpen_strength, 0.0f, 0.5f, "%.3f");
					taa_changed |= ImGui::SliderFloat("TAA Rejection", &state.temporal_aa.rejection_threshold, 0.0f, 1.0f, "%.3f");
					static const char* temporal_aa_debug_modes[] = {
						"Off",
						"History Acceptance",
						"Previous UV",
					};
					taa_changed |= ImGui::Combo("TAA Debug", &state.temporal_aa.debug_mode, temporal_aa_debug_modes, IM_ARRAYSIZE(temporal_aa_debug_modes));
					ImGui::EndDisabled();
					if (taa_changed)
					{
						TemporalAAPass::invalidate_history(state);
					}
				}
				ImGui::Checkbox("SSAO", &state.ssao.enable);
				if (ImGui::CollapsingHeader("Depth-of-Field", ImGuiTreeNodeFlags_DefaultOpen))
				{
					if (ImGui::Checkbox("Enable DoF", &state.dof.enable))
					{
						TemporalAAPass::invalidate_history(state);
					}
					ImGui::BeginDisabled(!state.dof.enable);
					ImGui::SliderFloat("Focus Distance", &state.dof.focus_distance, 0.1f, 500.0f, "%.1f");
					ImGui::SliderFloat("Focus Range", &state.dof.focus_range, 0.1f, 200.0f, "%.1f");
					ImGui::SliderFloat("Max CoC Radius", &state.dof.max_coc_radius, 0.0f, 32.0f, "%.1f px");
					ImGui::SliderFloat("Foreground Scale", &state.dof.foreground_blur_scale, 0.0f, 4.0f, "%.2f");
					ImGui::SliderFloat("Background Scale", &state.dof.background_blur_scale, 0.0f, 4.0f, "%.2f");
					ImGui::Checkbox("Show CoC Debug", &state.dof.debug_show_coc);
					ImGui::EndDisabled();
				}
			}
			ImGui::Unindent();

			ImGui::Separator();

			ImGui::Indent();
			if (ImGui::CollapsingHeader("Shadows", ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::Checkbox("Shadow Rendering", &state.shadow.rendering_enable);
				ImGui::Checkbox("Shadow Blur", &state.shadow.blur_enable);
				ImGui::Checkbox("Freeze Shadow Depth", &state.shadow.depth_freeze);
				if (ImGui::Button("Recapture Shadow Depth"))
				{
					request_shadow_recapture();
				}
				if (ImGui::SliderInt("Num Cascades", &state.shadow.num_cascades, 1, MAX_SHADOW_CASCADES))
				{
					invalidate_shadow_cache();
				}
				f32& active_cascade_distance_scale = state.shadow.cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares
					? state.shadow.centered_square_cascade_distance_scale
					: state.shadow.frustum_cascade_distance_scale;
				if (ImGui::SliderFloat("Cascade Distance Scale", &active_cascade_distance_scale, 0.25f, 4.0f, "%.2f"))
				{
					invalidate_shadow_cache();
				}
				if (ImGui::Combo("Cascade Placement", (i32*) &state.shadow.cascade_placement_mode, EShadowCascadePlacementModeNames, IM_ARRAYSIZE(EShadowCascadePlacementModeNames)))
				{
					invalidate_shadow_cache();
				}
				if (state.shadow.cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares)
				{
					if (ImGui::SliderFloat("Centered Square Lookahead", &state.shadow.centered_square_lookahead_distance, 0.0f, 1000.0f, "%.2f"))
					{
						if (!state.shadow.depth_freeze)
						{
							invalidate_shadow_cache();
						}
					}
					bool center_changed = false;
					ImGui::BeginDisabled(!state.shadow.depth_freeze);
					center_changed = ImGui::DragFloat3("Centered Square Center", &state.shadow.centered_square_center.X, 0.25f, -10000.0f, 10000.0f, "%.2f");
					ImGui::EndDisabled();
					if (center_changed)
					{
						invalidate_shadow_cache();
					}
				}
				ImGui::Checkbox("Show Cascade Selection", &state.shadow.debug_show_cascade_selection);
				if (ImGui::CollapsingHeader("Screen Space Shadows", ImGuiTreeNodeFlags_DefaultOpen))
				{
					ImGui::Checkbox("Enable Screen Space Shadows", &state.shadow.screen_space.enable);
					ImGui::SliderFloat("Contact Ray Length", &state.shadow.screen_space.ray_length, 0.0f, 10.0f, "%.2f");
					ImGui::SliderFloat("Thickness", &state.shadow.screen_space.thickness, 0.001f, 0.5f, "%.3f");
					ImGui::SliderFloat("Jitter Strength", &state.shadow.screen_space.jitter_strength, 0.0f, 2.0f, "%.2f");
					ImGui::SliderInt("Max Steps", &state.shadow.screen_space.max_steps, 1, 64);
					ImGui::SliderFloat("Intensity", &state.shadow.screen_space.intensity, 0.0f, 1.0f, "%.2f");
					ImGui::SliderInt("Filter Radius", &state.shadow.screen_space.filter_radius, 0, 2);
					ImGui::Checkbox("Show Screen Space Shadow Mask", &state.shadow.screen_space.debug_show_mask);
					if (state.shadow.screen_space.debug_show_mask)
					{
						GpuImage& screen_space_shadow_image = get_render_pass(ERenderPass::ScreenSpaceShadows).get_color_output(0);
						const GpuImageDesc& screen_space_shadow_desc = screen_space_shadow_image.get_desc();
						const f32 max_debug_size = 256.0f;
						const f32 aspect_ratio = (f32)screen_space_shadow_desc.width / (f32)screen_space_shadow_desc.height;
						const ImVec2 image_size = aspect_ratio >= 1.0f
							? ImVec2(max_debug_size, max_debug_size / aspect_ratio)
							: ImVec2(max_debug_size * aspect_ratio, max_debug_size);
						ImGui::Image(simgui_imtextureid(screen_space_shadow_image.get_texture_view(0)), image_size);
					}
				}
			}
			ImGui::Unindent();

			ImGui::Separator();

			ImGui::Indent();
			if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::Checkbox("Sky Rendering", &state.sky.rendering_enable);
				ImGui::Checkbox("Direct Lighting", &state.lighting.direct_enable);

				ImGui::Separator();

				ImGui::Indent();
				if (ImGui::CollapsingHeader("Global Illumination", ImGuiTreeNodeFlags_DefaultOpen))
				{
					ImGui::Checkbox("GI", &state.gi.enable);
					ImGui::Checkbox("GI Probe Occlusion", &state.gi.probe_occlusion);
					if (ImGui::SliderInt("GI Octree Depth", &state.gi.octree_depth, GI_Scene::min_octree_depth, GI_Scene::max_octree_depth))
					{
						state.gi.layout_dirty = true;
						state.gi.is_updating = true;
					}
					ImGui::Text(
						"Octree: depth %d  nodes %zu  payloads %d  probes %d",
						gi_scene.octree_depth,
						gi_scene.octree_nodes.length(),
						gi_scene.payload_count,
						gi_scene.non_fallback_probe_count
					);
					ImGui::Text(
						"Atlas: %zu / %d",
						gi_scene.probes.length(),
						gi_scene_atlas_capacity()
					);
					ImGui::Text(
						"Bounds Min: %.2f %.2f %.2f",
						gi_scene.scene_bounds.min.X,
						gi_scene.scene_bounds.min.Y,
						gi_scene.scene_bounds.min.Z
					);
					ImGui::Text(
						"Bounds Max: %.2f %.2f %.2f",
						gi_scene.scene_bounds.max.X,
						gi_scene.scene_bounds.max.Y,
						gi_scene.scene_bounds.max.Z
					);
					ImGui::Text(
						"Cell Extent: %.2f / %.2f  Max Radial Depth: %.2f",
						gi_scene.min_occupied_cell_extent,
						gi_scene.max_occupied_cell_extent,
						gi_scene.max_radial_depth
					);
					if (ImGui::Combo("Probe Radiance Mode", (i32*) &state.gi.probe_radiance_mode, EProbeRadianceModeNames, IM_ARRAYSIZE(EProbeRadianceModeNames)))
					{
						state.gi.is_updating = true;
					}
					if (ImGui::Combo("Probe Occlusion Mode", (i32*) &state.gi.probe_occlusion_mode, EProbeOcclusionModeNames, IM_ARRAYSIZE(EProbeOcclusionModeNames)))
					{
						state.gi.is_updating = true;
					}
					if (ImGui::Checkbox("render sky to probes", &state.gi.render_sky_to_probes))
					{
						state.gi.is_updating = true;
					}
					ImGui::Checkbox("Show Probes", &state.gi.show_probes);
					if (ImGui::Checkbox("Probe Isolation", &state.gi.probe_isolation_enable))
					{
						if (state.gi.probe_isolation_enable)
						{
							state.gi.show_probes = true;
							set_mouse_locked(false);
						}
						else
						{
							state.gi.isolated_probe_index = -1;
						}
					}
					if (state.gi.probe_isolation_enable)
					{
						state.gi.show_probes = true;

						ImGui::SameLine();
						if (ImGui::SmallButton("Clear"))
						{
							state.gi.isolated_probe_index = -1;
						}

						if (state.gi.isolated_probe_index >= 0)
						{
							ImGui::Text("Isolated Probe: %d", state.gi.isolated_probe_index);
						}
						else
						{
							ImGui::Text("Isolated Probe: None");
						}
					}
					ImGui::SliderFloat("GI Intensity", &state.gi.intensity, 0.0f, 10.0f, "%.2f");
					if (ImGui::Button("Update GI Probes") && !state.gi.is_updating)
					{
						state.gi.is_updating = true;
					}
					ImGui::SameLine();
					ImGui::Checkbox("Compute Irradiance", &state.gi.compute_irradiance);

					if (state.gi.is_updating)
					{
						ImGui::SameLine();
						ImGui::Text("Updating...");
					}

					if (ImGui::Combo("Probe Vis Mode", (i32*) &state.gi.probe_vis_mode, EProbeVisModeNames, IM_ARRAYSIZE(EProbeVisModeNames)))
					{
						if (state.gi.probe_vis_mode == EProbeVisMode::SH9Irradiance || state.gi.probe_vis_mode == EProbeVisMode::SG9Irradiance)
						{
							state.gi.is_updating = true;
						}
					}
					if (ImGui::Checkbox("Debug Constant White Probes", &state.gi.debug_constant_white_probes))
					{
						state.gi.is_updating = true;
					}
				}
				ImGui::Unindent();
			}
			ImGui::Unindent();
		}

		if (ImGui::CollapsingHeader("Render Texture Viewer"))
		{
			{
				RenderPass& depth_pass = get_render_pass(ERenderPass::ShadowDepth);
				GpuImage& image = depth_pass.get_depth_output();
				const GpuImageDesc& desc = image.get_desc();
				ImGui::Text("Shadow Cascades");
				ImGui::Text("%d x %d x %d", desc.width, desc.height, ShadowDepthPass::get_active_cascade_count(state));
				const f32 active_cascade_distance_scale = state.shadow.cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares
					? state.shadow.centered_square_cascade_distance_scale
					: state.shadow.frustum_cascade_distance_scale;
				ImGui::Text("Distance Scale: %.2f", active_cascade_distance_scale);
				for (i32 cascade_idx = 0; cascade_idx < ShadowDepthPass::get_active_cascade_count(state); ++cascade_idx)
				{
					ImGui::Text("Cascade %d: %.2f", cascade_idx, ShadowDepthPass::cascade_distances[cascade_idx]);
				}
				const i32 active_cascade_count = ShadowDepthPass::get_active_cascade_count(state);
				if (state.shadow.debug_cascade_index >= active_cascade_count)
				{
					state.shadow.debug_cascade_index = active_cascade_count - 1;
				}
				ImGui::SliderInt("Debug Cascade", &state.shadow.debug_cascade_index, 0, active_cascade_count - 1);
				const char* shadow_debug_modes[] = { "Moments", "Depth" };
				ImGui::Combo("Debug View", &state.shadow.debug_view_mode, shadow_debug_modes, IM_ARRAYSIZE(shadow_debug_modes));

				GpuImage& debug_image = get_render_pass(ERenderPass::ShadowCascadeDebug).get_color_output(0);
				const GpuImageDesc& debug_desc = debug_image.get_desc();
				const f32 max_debug_size = 256.0f;
				const f32 aspect_ratio = (f32)debug_desc.width / (f32)debug_desc.height;
				const ImVec2 image_size = aspect_ratio >= 1.0f
					? ImVec2(max_debug_size, max_debug_size / aspect_ratio)
					: ImVec2(max_debug_size * aspect_ratio, max_debug_size);
				ImGui::Image(simgui_imtextureid(debug_image.get_texture_view(0)), image_size);
			}

			ImGui::Separator();

			if (ImGui::TreeNode("Main Pass"))
			{
				RenderPass& geometry_pass = get_render_pass(ERenderPass::Geometry);
				for (i32 i = 0; i < geometry_pass.get_num_color_outputs(); ++i)
				{
					GpuImage& image = get_render_pass(ERenderPass::Geometry).get_color_output(i);
					const GpuImageDesc& desc = image.get_desc();
					const ImVec2 image_size = ImVec2(desc.width / 4.0, desc.height / 4.0);

					// Use a solid background color (e.g., Black) to hide transparency
					const ImVec4 bg_col = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
					const ImVec4 tint_col = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // Pure white tint

					ImGui::ImageWithBg(
						simgui_imtextureid(image.get_texture_view(0)),
						image_size,
						ImVec2(0,0), ImVec2(1,1),
						bg_col, tint_col
					);
				}

				ImGui::TreePop(); // Remember to pop!
			}

			const ImVec2 debug_texture_size(256,256);

			// Octahedral Atlas Visualization
			ImGui::Text("Octahedral Atlas: Lighting");
			ImGui::Image(simgui_imtextureid(gi_scene_get_octahedral_lighting_view(gi_scene)), debug_texture_size);
			ImGui::Text("Octahedral Atlas: Depth");
			ImGui::Image(simgui_imtextureid(gi_scene_get_octahedral_depth_view(gi_scene)), debug_texture_size);

			// Baked Sky Visualization
			ImGui::Text("Baked Sky");
			ImGui::Image(simgui_imtextureid(SkyBakePass::get_baked_sky_image_view()), debug_texture_size);
		}
	);

	{
		CPU_TIMING_SCOPE("Simulation");
		if (state.runtime.is_simulating)
		{
			update_player_character_control((f32)delta_time);

			jolt_update(delta_time);
		}
	}

	{
		CPU_TIMING_SCOPE("Object Transforms");
		update_physics_backed_object_transforms();
	}

	{
		CPU_TIMING_SCOPE("Skinned Animation");
		update_skinned_animations((f32)delta_time);
	}

	{
		CPU_TIMING_SCOPE("Camera + Controls");

		// Debug Camera Control
		if (state.debug_camera.active && !is_key_pressed(SAPP_KEYCODE_LEFT_CONTROL))
		{
			Camera& camera = state.debug_camera.camera;
			const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));

			f32 move_speed = 10.0f * delta_time;
			if (is_key_pressed(SAPP_KEYCODE_LEFT_SHIFT))
			{
				move_speed *= 5.0f;
			}

			if (is_key_pressed(SAPP_KEYCODE_W) || is_key_pressed(SAPP_KEYCODE_UP))
			{
				camera.location += camera.forward * move_speed;
			}
			if (is_key_pressed(SAPP_KEYCODE_S) || is_key_pressed(SAPP_KEYCODE_DOWN))
            {
                camera.location -= camera.forward * move_speed;
            }
			if (is_key_pressed(SAPP_KEYCODE_D) || is_key_pressed(SAPP_KEYCODE_RIGHT))
			{
				camera.location += camera_right * move_speed;
			}
			if (is_key_pressed(SAPP_KEYCODE_A) || is_key_pressed(SAPP_KEYCODE_LEFT))
			{
				camera.location -= camera_right * move_speed;
			}
			if (is_key_pressed(SAPP_KEYCODE_E))
			{
				camera.location += camera.up * move_speed;
			}
			if (is_key_pressed(SAPP_KEYCODE_Q))
			{
				camera.location -= camera.up * move_speed;
			}

			if (is_mouse_locked())
			{
				const f32 look_speed = 1.0f * delta_time;
				const HMM_Vec2 mouse_delta = get_mouse_delta();

				const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));
				camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera.up, -mouse_delta.X * look_speed));
				camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera_right, -mouse_delta.Y * look_speed));
			}
		}

		// Player Camera Control
		if (state.scene.camera_control_id && !state.debug_camera.active)
		{
			Object& camera_control_object = state.scene.objects[*state.scene.camera_control_id];
			CameraControl& camera_control = camera_control_object.camera_control;
			Camera& camera = camera_control.camera;

			if (is_mouse_locked())
			{
				//FCS TODO: Add max angle property (angle above XY plane) that we can rotate camera
				//FCS TODO: Add rotation speed property to camera control component
				const f32 look_speed = 1.0f * delta_time;
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
				HMM_Clamp(0.0f, camera_control.follow_speed * delta_time, 1.0f),
				desired_location
			);
		}
	}

	// Rendering
	{
		CPU_TIMING_SCOPE("Rendering");
		Camera& camera = get_active_camera();
		HMM_Mat4 projection_matrix = HMM_M4D(1.0f);
		HMM_Mat4 view_matrix = HMM_M4D(1.0f);
		HMM_Mat4 view_projection_matrix = HMM_M4D(1.0f);
		HMM_Mat4 unjittered_view_projection_matrix = HMM_M4D(1.0f);
		i32 temporal_aa_output_index = state.temporal_aa.history_index;

		{
			CPU_TIMING_SCOPE("Render Preparation");

			// Run initially, and then only if our updates from blender encounter a light
			if (state.lighting.needs_data_update)
			{
				CPU_TIMING_SCOPE("Lighting Data Update");
				state.lighting.needs_data_update = false;

				const i32 MAX_LIGHTS_PER_TYPE = 1024;

				// Init point_lights_buffer if we haven't already
				if (!state.lighting.point_lights_buffer.is_gpu_buffer_valid())
				{
					state.lighting.point_lights_buffer = GpuBuffer((GpuBufferDesc<lighting_PointLight_t>){
						.data = nullptr,
						.size = sizeof(lighting_PointLight_t) * MAX_LIGHTS_PER_TYPE,
						.usage = {
							.storage_buffer = true,
							.stream_update = true,
						},
						.label = "point_lights",
					});
				}

				// Init spot_lights_buffer if we haven't already
				if (!state.lighting.spot_lights_buffer.is_gpu_buffer_valid())
				{
					state.lighting.spot_lights_buffer = GpuBuffer((GpuBufferDesc<lighting_SpotLight_t>){
						.data = nullptr,
						.size = sizeof(lighting_SpotLight_t) * MAX_LIGHTS_PER_TYPE,
						.usage = {
							.storage_buffer = true,
							.stream_update = true,
						},
						.label = "spot_lights",
					});
				}

				// Init sun_lights_buffer if we haven't already
				if (!state.lighting.sun_lights_buffer.is_gpu_buffer_valid())
				{
					state.lighting.sun_lights_buffer = GpuBuffer((GpuBufferDesc<lighting_SunLight_t>){
						.data = nullptr,
						.size = sizeof(lighting_SunLight_t) * MAX_LIGHTS_PER_TYPE,
						.usage = {
							.storage_buffer = true,
							.stream_update = true,
						},
						.label = "sun_lights",
					});
				}

				state.lighting.point_lights.reset();
				state.lighting.spot_lights.reset();
				state.lighting.sun_lights.reset();

				scene_ensure_indexes(state);
				state.data_oriented.frame.lighting_candidate_count += (i32)state.scene.indexes.light_object_ids.length();
				for (const i32 unique_id : state.scene.indexes.light_object_ids)
				{
					if (!state.scene.objects.contains(unique_id))
					{
						continue;
					}

					const Object& object = state.scene.objects[unique_id];
					assert(object.has_light);
					state.data_oriented.frame.lighting_processed_count += 1;

					const Light& light = object.light;
					switch(light.type)
					{
						case LightType::Point:
						{
							if (state.lighting.point_lights.length() >= MAX_LIGHTS_PER_TYPE)
							{
								printf("Exceeded Max Number of Point Lights (%i)\n", MAX_LIGHTS_PER_TYPE);
								continue;
							}

							lighting_PointLight_t new_point_light = {};
							const Transform& transform = object.current_transform;
							memcpy(&new_point_light.location, &transform.location, sizeof(f32) * 4);
							memcpy(&new_point_light.color, &object.light.color, sizeof(f32) * 3);
							new_point_light.color[3] = 1.0; // Force alpha to 1.0
							new_point_light.power = object.light.point.power;
							state.lighting.point_lights.add(new_point_light);
							break;
						}
						case LightType::Spot:
						{
							if (state.lighting.spot_lights.length() >= MAX_LIGHTS_PER_TYPE)
							{
								printf("Exceeded Max Number of Spot Lights (%i)\n", MAX_LIGHTS_PER_TYPE);
								continue;
							}

							lighting_SpotLight_t new_spot_light = {};
							const Transform& transform = object.current_transform;
							memcpy(&new_spot_light.location, &transform.location, sizeof(f32) * 4);
							memcpy(&new_spot_light.color, &object.light.color, sizeof(f32) * 3);
							new_spot_light.color[3] = 1.0; // Force alpha to 1.0

							HMM_Vec3 spot_light_dir = HMM_NormV3(HMM_RotateV3Q(HMM_V3(0,0,-1), transform.rotation));
							memcpy(&new_spot_light.direction, &spot_light_dir, sizeof(f32) * 3);

							new_spot_light.spot_angle_radians = object.light.spot.beam_angle / 2.0f;
							new_spot_light.power = object.light.spot.power;
							new_spot_light.edge_blend = object.light.spot.edge_blend;
							state.lighting.spot_lights.add(new_spot_light);
							break;
						}
						case LightType::Sun:
						{
							if (state.lighting.sun_lights.length() >= MAX_LIGHTS_PER_TYPE)
							{
								printf("Exceeded Max Number of Sun Lights (%i)\n", MAX_LIGHTS_PER_TYPE);
								continue;
							}

							lighting_SunLight_t new_sun_light = {};
							const Transform& transform = object.current_transform;
							memcpy(&new_sun_light.location, &transform.location, sizeof(f32) * 4);
							memcpy(&new_sun_light.color, &object.light.color, sizeof(f32) * 3);
							new_sun_light.color[3] = 1.0; // Force alpha to 1.0

							HMM_Vec3 sun_light_dir = HMM_NormV3(HMM_RotateV3Q(HMM_V3(0,0,-1), transform.rotation));
							memcpy(&new_sun_light.direction, &sun_light_dir, sizeof(f32) * 3);

							new_sun_light.power = object.light.sun.power;
							new_sun_light.cast_shadows = object.light.sun.cast_shadows;
							state.lighting.sun_lights.add(new_sun_light);
							break;
						}
						case LightType::Area:
						{
							break;
						}
						default: break;
					}
				}

				state.lighting.fs_params.num_point_lights = state.lighting.point_lights.length();
				printf("Num Point Lights: %i\n", state.lighting.fs_params.num_point_lights);
				if (state.lighting.fs_params.num_point_lights > 0)
				{
					state.lighting.point_lights_buffer.update_gpu_buffer(
						(sg_range){
							.ptr = state.lighting.point_lights.data(),
							.size = sizeof(lighting_PointLight_t) * state.lighting.fs_params.num_point_lights,
						}
					);
				}

				state.lighting.fs_params.num_spot_lights = state.lighting.spot_lights.length();
				printf("Num Spot Lights: %i\n", state.lighting.fs_params.num_spot_lights);
				if (state.lighting.fs_params.num_spot_lights > 0)
				{
					state.lighting.spot_lights_buffer.update_gpu_buffer(
						(sg_range){
							.ptr = state.lighting.spot_lights.data(),
							.size = sizeof(lighting_SpotLight_t) * state.lighting.fs_params.num_spot_lights,
						}
					);
				}

				state.lighting.fs_params.num_sun_lights = state.lighting.sun_lights.length();
				printf("Num Sun Lights: %i\n", state.lighting.fs_params.num_sun_lights);
				if (state.lighting.fs_params.num_sun_lights > 0)
				{
					state.lighting.sun_lights_buffer.update_gpu_buffer(
						(sg_range){
							.ptr = state.lighting.sun_lights.data(),
							.size = sizeof(lighting_SunLight_t) * state.lighting.fs_params.num_sun_lights,
						}
					);
				}
			}

			// Bake Sky
			if (state.sky.rendering_enable || state.gi.render_sky_to_probes)
			{
				CPU_TIMING_SCOPE("Sky Bake Update");
				SkyBakePass::render(state);
			}

			// View + Projection matrix setup
			const f32 fov = HMM_AngleDeg(60.0f);
			{
				CPU_TIMING_SCOPE("Camera Matrices");
				const f32 w = (f32)state.window.render_width;
				const f32 h = (f32)state.window.render_height;
				const f32 aspect_ratio = w/h;
				projection_matrix = mat4_perspective(fov, aspect_ratio);

				HMM_Vec3 target = camera.location + camera.forward * 10;
				view_matrix = HMM_LookAt_RH(camera.location, target, camera.up);
				unjittered_view_projection_matrix = HMM_MulM4(projection_matrix, view_matrix);
				if (state.temporal_aa.enable)
				{
					state.temporal_aa.jitter_phase = (i32)(state.data_oriented.frame_index & 1);
					state.temporal_aa.current_jitter_pixels = TemporalAAPass::get_decima_jitter_pixels(state.temporal_aa.jitter_phase);
					projection_matrix = TemporalAAPass::apply_projection_jitter(
						projection_matrix,
						state.temporal_aa.current_jitter_pixels,
						HMM_V2(w, h)
					);
				}
				else
				{
					state.temporal_aa.current_jitter_pixels = HMM_V2(0.0f, 0.0f);
				}
				view_projection_matrix = HMM_MulM4(projection_matrix, view_matrix);
				if (!state.shadow.depth_freeze)
				{
					state.shadow.centered_square_center = camera.location + HMM_NormV3(camera.forward) * state.shadow.centered_square_lookahead_distance;
				}
			}

			{
				CPU_TIMING_SCOPE("GPU Skinning Cache");
				GpuSkinning::update(state, state.tessellation.enabled || state.wireframe.shaded_wireframe);
			}

			{
				CPU_TIMING_SCOPE("Tessellation Update");
				Tessellation::update(state, camera, fov);
			}

			// Update our GI Scene
			{
				CPU_TIMING_SCOPE("GI Scene Update");
				gi_scene_update(gi_scene, state);
			}
		}

		if (state.shadow.rendering_enable)
		{
			if (!ShadowDepthPass::get_valid_shadow_sun(state))
			{
				invalidate_shadow_cache();
			}
			else
			{
				const bool should_update_shadow_depth = state.shadow.depth_freeze
					? state.shadow.force_recapture
					: true;
				if (should_update_shadow_depth)
				{
					RenderPass& shadow_depth_pass = get_render_pass(ERenderPass::ShadowDepth);
					shadow_depth_pass.set_pass_count_override(ShadowDepthPass::get_active_cascade_count(state));
					shadow_depth_pass.execute(
						[&](const RenderPassExecutionContext& context)
						{
							ShadowDepthPass::render(state, context.slice_idx);
						}
					);
					shadow_depth_pass.set_pass_count_override(-1);
					state.shadow.force_recapture = false;
					ShadowDepthPass::has_valid_shadow_blur = false;
				}

				if (state.shadow.blur_enable && ShadowDepthPass::has_valid_shadow_map && !ShadowDepthPass::has_valid_shadow_blur)
				{
					ShadowBlurPass::execute_separable(
						get_render_pass_entry(ERenderPass::ShadowBlur),
						get_render_pass(ERenderPass::ShadowDepth).get_color_output(0).get_texture_array_view(),
						state.gpu.linear_sampler,
						HMM_V2(
							(f32)ShadowDepthPass::ShadowMapResolution,
							(f32)ShadowDepthPass::ShadowMapResolution
						),
						21
					);
					ShadowDepthPass::has_valid_shadow_blur = true;
				}
				else if (!state.shadow.blur_enable)
				{
					ShadowDepthPass::has_valid_shadow_blur = false;
				}

				if (ShadowDepthPass::has_valid_shadow_map)
				{
					GpuImage& shadow_moments_texture = state.shadow.blur_enable && ShadowDepthPass::has_valid_shadow_blur
						? get_render_pass(ERenderPass::ShadowBlur).get_color_output(0)
						: get_render_pass(ERenderPass::ShadowDepth).get_color_output(0);
					const i32 active_cascade_count = ShadowDepthPass::get_active_cascade_count(state);
					if (state.shadow.debug_cascade_index >= active_cascade_count)
					{
						state.shadow.debug_cascade_index = active_cascade_count - 1;
					}
					ShadowCascadeDebugPass::render(
						get_render_pass(ERenderPass::ShadowCascadeDebug),
						shadow_moments_texture.get_texture_array_view(),
						state.gpu.linear_sampler,
						state.shadow.debug_cascade_index,
						state.shadow.debug_view_mode
					);
				}
			}
		}
		else
		{
			invalidate_shadow_cache();
		}

		const f32 cull_bounds_padding = state.tessellation.enabled ? state.tessellation.bounds_padding : 0.0f;
		CullResult cull_result;
		{
			CPU_TIMING_SCOPE("Geometry Culling");
			cull_result = cull_objects(state, unjittered_view_projection_matrix, cull_bounds_padding);
		}

		{ // Geometry Pass
			get_render_pass(ERenderPass::Geometry).execute(
				[&](const i32)
				{
				    geometry_vs_params_t vs_params;
					geometry_fs_params_t fs_params;
					{
						CPU_TIMING_SCOPE("Geometry Uniforms");
						vs_params.view = view_matrix;
						vs_params.projection = projection_matrix;
						fs_params.skinning_debug_view = state.animation.skinning_debug_view ? 1 : 0;

						// Apply Vertex Uniforms
						sg_apply_uniforms(0, SG_RANGE(vs_params));
					}

					// Submit draw calls for objects after culling
					{
						CPU_TIMING_SCOPE("Geometry Draw Meshes");
						draw_visible_geometry_meshes(
							state,
							cull_result,
							get_render_pass(ERenderPass::Geometry),
							vs_params,
							fs_params,
							sglue_swapchain().depth_format
						);
					}

					if (state.gi.show_probes)
					{
						CPU_TIMING_SCOPE("Geometry GI Probes");
						gi_scene_render_debug(gi_scene, view_matrix, projection_matrix);
					}

					if (state.sky.rendering_enable)
					{
						CPU_TIMING_SCOPE("Geometry Sky");
						SkyPass::render(
							view_projection_matrix,
							get_active_camera().location,
							sglue_swapchain().depth_format
						);
					}
				}
			);
		}

		if (state.ssao.enable)
		{
			RenderPass& ssao_pass = get_render_pass(ERenderPass::SSAO);
			{ // SSAO
				ssao_pass.execute(
					[&](const i32)
					{
						state.ssao.fs_params.screen_size = HMM_V2((f32)ssao_pass.current_width, (f32)ssao_pass.current_height);
						state.ssao.fs_params.view = view_matrix;
						state.ssao.fs_params.projection = projection_matrix;
						state.ssao.fs_params.ssao_enable = state.ssao.enable;
						sg_apply_uniforms(0, SG_RANGE(state.ssao.fs_params));

						RenderPass& geometry_pass = get_render_pass(ERenderPass::Geometry);

						sg_bindings bindings = (sg_bindings){
							.views = {
								[0] = geometry_pass.get_color_output(1).get_texture_view(0),	// geometry pass position
								[1] = geometry_pass.get_color_output(2).get_texture_view(0),	// geometry pass normal
								[2] = state.ssao.noise_texture.get_texture_view(0),			// ssao noise texture
							},
							.samplers[0] = state.gpu.linear_sampler,
						};
						gpu_apply_bindings(&bindings);

						sg_draw(0,6,1);
					}
				);
			}

			{ // SSAO Blur
				RenderPass& ssao_blur_pass = get_render_pass(ERenderPass::SSAO_Blur);
				BlurPass::execute_separable(
					get_render_pass_entry(ERenderPass::SSAO_Blur),
					ssao_pass.get_color_output(0).get_texture_view(0),
					state.gpu.linear_sampler,
					HMM_V2((f32)ssao_blur_pass.current_width, (f32)ssao_blur_pass.current_height),
					4
				);
			}
		}

		bool screen_space_shadows_valid = false;
		if (state.shadow.rendering_enable && state.shadow.screen_space.enable)
		{
			Object* screen_space_shadow_sun = ShadowDepthPass::get_valid_shadow_sun(state);
			if (screen_space_shadow_sun)
			{
				RenderPass& geometry_pass = get_render_pass(ERenderPass::Geometry);
				HMM_Vec3 sun_light_dir = HMM_NormV3(HMM_RotateV3Q(HMM_V3(0, 0, -1), screen_space_shadow_sun->current_transform.rotation));
				ScreenSpaceShadowsPass::execute(
					get_render_pass_entry(ERenderPass::ScreenSpaceShadows),
					geometry_pass.get_color_output(1).get_texture_view(0),
					geometry_pass.get_color_output(2).get_texture_view(0),
					state.gpu.linear_sampler,
					view_matrix,
					projection_matrix,
					sun_light_dir,
					state.shadow.screen_space.ray_length,
					state.shadow.screen_space.thickness,
					state.shadow.screen_space.jitter_strength,
					state.shadow.screen_space.max_steps,
					state.shadow.screen_space.filter_radius
				);
				screen_space_shadows_valid = true;
			}
		}

		{ // Lighting Pass
			get_render_pass(ERenderPass::Lighting).execute(
				[&](const i32)
				{
					state.lighting.fs_params.view_position = get_active_camera().location;
					state.lighting.fs_params.view_forward = get_active_camera().forward;
					state.lighting.fs_params.ssao_enable = state.ssao.enable;
					state.lighting.fs_params.direct_lighting_enable = state.lighting.direct_enable;
					state.lighting.fs_params.gi_enable = state.gi.enable;
					state.lighting.fs_params.gi_probe_occlusion = state.gi.probe_occlusion;
					state.lighting.fs_params.probe_occlusion_mode = static_cast<i32>(state.gi.probe_occlusion_mode);
					state.lighting.fs_params.probe_radiance_mode = static_cast<i32>(state.gi.probe_radiance_mode);
					state.lighting.fs_params.gi_intensity = state.gi.intensity;
					state.lighting.fs_params.atlas_total_size = gi_scene.atlas_total_size;
					state.lighting.fs_params.atlas_entry_size = gi_scene.atlas_entry_size;
					state.lighting.fs_params.gi_fallback_probe_index = gi_scene.fallback_probe_index;
					state.lighting.fs_params.gi_octree_node_count = (i32)gi_scene.octree_nodes.length();
					state.lighting.fs_params.shadow_map_enable = state.shadow.rendering_enable && ShadowDepthPass::has_valid_shadow_map ? 1 : 0;
					state.lighting.fs_params.shadow_num_cascades = ShadowDepthPass::get_active_cascade_count(state);
					state.lighting.fs_params.shadow_cascade_placement_mode = (i32) state.shadow.cascade_placement_mode;
					state.lighting.fs_params.shadow_debug_show_cascade_selection = state.shadow.debug_show_cascade_selection ? 1 : 0;
					state.lighting.fs_params.isolated_probe_index = state.gi.probe_isolation_enable
						? (state.gi.isolated_probe_index >= 0 ? state.gi.isolated_probe_index : -2)
						: -1;
					state.lighting.fs_params.screen_space_shadows_enable = screen_space_shadows_valid ? 1 : 0;
					state.lighting.fs_params.shadow_bias = 0.001f;
					state.lighting.fs_params.screen_space_shadow_intensity = state.shadow.screen_space.intensity;
					state.lighting.fs_params.shadow_map_texel_size = HMM_V2(
						1.0f / (f32)ShadowDepthPass::ShadowMapResolution,
						1.0f / (f32)ShadowDepthPass::ShadowMapResolution
					);
					state.lighting.fs_params.shadow_cascade_distances = HMM_V4(
						ShadowDepthPass::cascade_distances[0],
						ShadowDepthPass::cascade_distances[1],
						ShadowDepthPass::cascade_distances[2],
						ShadowDepthPass::cascade_distances[3]
					);
					for (i32 i = 0; i < MAX_SHADOW_CASCADES; ++i)
					{
						state.lighting.fs_params.shadow_view_projections[i] = ShadowDepthPass::shadow_view_projections[i];
					}

					// Apply Fragment Uniforms
					sg_apply_uniforms(0, SG_RANGE(state.lighting.fs_params));

					RenderPass& geometry_pass = get_render_pass(ERenderPass::Geometry);
					RenderPass& ssao_blur_pass = get_render_pass(ERenderPass::SSAO_Blur);

					GpuImage& color_texture = geometry_pass.get_color_output(0);
					GpuImage& position_texture = geometry_pass.get_color_output(1);
					GpuImage& normal_texture = geometry_pass.get_color_output(2);
					GpuImage& roughness_metallic_texture = geometry_pass.get_color_output(3);
					GpuImage& blurred_ssao_texture = ssao_blur_pass.get_color_output(0);
					GpuImage& shadow_moments_texture = state.shadow.blur_enable && ShadowDepthPass::has_valid_shadow_blur
						? get_render_pass(ERenderPass::ShadowBlur).get_color_output(0)
						: get_render_pass(ERenderPass::ShadowDepth).get_color_output(0);
					GpuImage& screen_space_shadow_texture = screen_space_shadows_valid
						? get_render_pass(ERenderPass::ScreenSpaceShadows).get_color_output(0)
						: state.gpu.default_image;

					sg_bindings bindings = {
						.views = {
							[0] = color_texture.get_texture_view(0),
							[1] = position_texture.get_texture_view(0),
							[2] = normal_texture.get_texture_view(0),
							[3] = roughness_metallic_texture.get_texture_view(0),
							[4] = blurred_ssao_texture.get_texture_view(0),
							[5] = state.lighting.point_lights_buffer.get_storage_view(),
							[6] = state.lighting.spot_lights_buffer.get_storage_view(),
							[7] = state.lighting.sun_lights_buffer.get_storage_view(),
							[8] = gi_scene.probes_buffer.get_storage_view(),
							[9] = gi_scene.cells_buffer.get_storage_view(),
							[10] = gi_scene_get_octahedral_lighting_view(gi_scene),
							[11] = gi_scene_get_octahedral_depth_view(gi_scene),
							[12] = shadow_moments_texture.get_texture_array_view(),
							[13] = gi_scene.sh9_coefficients_buffer.get_storage_view(),
							[14] = gi_scene.sg9_lobes_buffer.get_storage_view(),
							[15] = gi_scene.octree_nodes_buffer.get_storage_view(),
							[16] = screen_space_shadow_texture.get_texture_view(0),
						},
						.samplers = {
							[0] = state.gpu.linear_sampler,
							[1] = state.gpu.linear_sampler,
						},
					};
					gpu_apply_bindings(&bindings);

					sg_draw(0,6,1);
				}
			);
		}

		{ // DOF

			if (state.dof.enable)
			{
				get_render_pass(ERenderPass::DOF_Combine).execute(
					[&](const i32)
					{
						RenderPass& lighting_pass = get_render_pass(ERenderPass::Lighting);
						RenderPass& geometry_pass = get_render_pass(ERenderPass::Geometry);

						const dof_combine_fs_params_t dof_combine_fs_params = {
							.cam_pos = HMM_V4(camera.location.X, camera.location.Y, camera.location.Z, 1.0f),
							.cam_forward = HMM_V4(camera.forward.X, camera.forward.Y, camera.forward.Z, 0.0f),
							.screen_size = HMM_V2((f32)state.window.render_width, (f32)state.window.render_height),
							.focus_distance = state.dof.focus_distance,
							.focus_range = state.dof.focus_range,
							.max_coc_radius = state.dof.max_coc_radius,
							.foreground_blur_scale = state.dof.foreground_blur_scale,
							.background_blur_scale = state.dof.background_blur_scale,
							.debug_mode = state.dof.debug_show_coc ? 1 : 0,
						};
						sg_apply_uniforms(0, SG_RANGE(dof_combine_fs_params));

						GpuImage& position_texture = geometry_pass.get_color_output(1);

						sg_bindings bindings = (sg_bindings){
							.views = {
								[0] = lighting_pass.get_color_output(0).get_texture_view(0),
								[1] = position_texture.get_texture_view(0),
							},
							.samplers[0] = state.gpu.linear_sampler,
						};

						gpu_apply_bindings(&bindings);

						sg_draw(0,6,1);
					}
				);
			}
		}

		if (state.wireframe.shaded_wireframe)
		{
			get_render_pass(ERenderPass::WireOverlay).execute(
				[&](const i32)
				{
					RenderPass& lighting_pass = get_render_pass(ERenderPass::Lighting);
					RenderPass& dof_combine_pass = get_render_pass(ERenderPass::DOF_Combine);
					RenderPass& geometry_pass = get_render_pass(ERenderPass::Geometry);

					GpuImage& source_color_image = state.dof.enable
						? dof_combine_pass.get_color_output(0)
						: lighting_pass.get_color_output(0);

					{
						sg_bindings copy_bindings = {
							.views = {
								[0] = source_color_image.get_texture_view(0),
							},
							.samplers[0] = state.gpu.nearest_sampler,
						};
						gpu_apply_bindings(&copy_bindings);
						sg_draw(0, 6, 1);
					}

					{
						CPU_TIMING_SCOPE("Wire Overlay Meshes");
						sg_apply_pipeline(WireOverlayPass::get_mesh_overlay_pipeline(SG_PIXELFORMAT_RGBA32F));

						wire_overlay_mesh_fs_params_t mesh_fs_params = {
							.color = state.wireframe.color,
							.camera_position = HMM_V4V(camera.location, 1.0f),
							.camera_forward = HMM_V4V(HMM_NormV3(camera.forward), 0.0f),
							.screen_size = HMM_V2((f32) state.window.render_width, (f32) state.window.render_height),
							.width = state.wireframe.width,
							.softness = state.wireframe.softness,
							.opacity = state.wireframe.opacity,
							.visibility_tolerance = state.wireframe.visibility_tolerance,
						};
						sg_apply_uniforms(1, SG_RANGE(mesh_fs_params));

						GpuImage& position_texture = geometry_pass.get_color_output(1);
						state.data_oriented.frame.draw_calls += 1;
						for (const i32 unique_id : cull_result.object_ids)
						{
							if (!state.scene.objects.contains(unique_id))
							{
								continue;
							}

							Object& object = state.scene.objects[unique_id];
							if (!object.has_mesh)
							{
								continue;
							}

							Mesh& mesh = object.mesh;
							MeshRenderView render_view = mesh_get_render_view(mesh);
							if (render_view.index_count == 0)
							{
								continue;
							}

							if (mesh_render_view_uses_skinning(mesh, render_view) &&
								!mesh_has_valid_skinned_vertex_cache(mesh))
							{
								continue;
							}
							mesh_populate_render_storage_views(mesh, render_view);
							if (render_view.vertex_storage_view.id == SG_INVALID_ID ||
								render_view.index_storage_view.id == SG_INVALID_ID)
							{
								continue;
							}

							wire_overlay_mesh_vs_params_t mesh_vs_params = {
								.view = view_matrix,
								.projection = projection_matrix,
								.model = transform_model_matrix(object.current_transform),
							};
							sg_apply_uniforms(0, SG_RANGE(mesh_vs_params));

							sg_bindings mesh_bindings = {
								.views = {
									[0] = render_view.vertex_storage_view,
									[1] = render_view.index_storage_view,
									[2] = position_texture.get_texture_view(0),
								},
								.samplers[0] = state.gpu.nearest_sampler,
							};
							gpu_apply_bindings(&mesh_bindings);
							sg_draw(0, render_view.index_count, 1);
							state.data_oriented.frame.draw_mesh_count += 1;
						}
					}
				}
			);
		}

		if (state.temporal_aa.enable)
		{
			RenderPass& temporal_aa_pass = get_render_pass(ERenderPass::TemporalAA);
			temporal_aa_output_index = state.temporal_aa.history_index;
			const i32 previous_history_index = (temporal_aa_output_index + 1) % 2;
			temporal_aa_pass.execute_one(
				temporal_aa_output_index,
				[&](const RenderPassExecutionContext& context)
				{
					RenderPass& lighting_pass = get_render_pass(ERenderPass::Lighting);
					RenderPass& dof_combine_pass = get_render_pass(ERenderPass::DOF_Combine);
					RenderPass& wire_overlay_pass = get_render_pass(ERenderPass::WireOverlay);
					RenderPass& geometry_pass = get_render_pass(ERenderPass::Geometry);

					GpuImage& source_color_image = state.wireframe.shaded_wireframe
						? wire_overlay_pass.get_color_output(0)
						: state.dof.enable
						? dof_combine_pass.get_color_output(0)
						: lighting_pass.get_color_output(0);

					const temporal_aa_fs_params_t fs_params = {
						.previous_view_projection = state.temporal_aa.previous_view_projection,
						.screen_size = HMM_V2((f32)state.window.render_width, (f32)state.window.render_height),
						.sharpen_axis = (state.temporal_aa.jitter_phase & 1) == 1 ? HMM_V2(1.0f, 0.0f) : HMM_V2(0.0f, 1.0f),
						.blend_alpha = state.temporal_aa.blend_alpha,
						.sharpen_strength = state.temporal_aa.sharpen_strength,
						.rejection_threshold = state.temporal_aa.rejection_threshold,
						.history_valid = state.temporal_aa.history_valid ? 1 : 0,
						.debug_mode = state.temporal_aa.debug_mode,
					};
					sg_apply_uniforms(0, SG_RANGE(fs_params));

					sg_bindings bindings = {
						.views = {
							[0] = source_color_image.get_texture_view(0),
							[1] = geometry_pass.get_color_output(1).get_texture_view(0),
							[2] = temporal_aa_pass.get_color_output(1, previous_history_index).get_texture_view(0),
						},
						.samplers = {
							[0] = state.gpu.linear_sampler,
							[1] = state.gpu.nearest_sampler,
						},
					};
					gpu_apply_bindings(&bindings);

					sg_draw(0,6,1);
					assert(context.image_idx == temporal_aa_output_index);
				}
			);

			state.temporal_aa.previous_view_projection = view_projection_matrix;
			state.temporal_aa.history_valid = true;
			state.temporal_aa.history_index = previous_history_index;
		}

		{ // Tonemapping Pass
			get_render_pass(ERenderPass::Tonemapping).execute(
				[&](const i32)
				{
					RenderPass& lighting_pass = get_render_pass(ERenderPass::Lighting);
					RenderPass& dof_combine_pass = get_render_pass(ERenderPass::DOF_Combine);
					RenderPass& wire_overlay_pass = get_render_pass(ERenderPass::WireOverlay);
					RenderPass& temporal_aa_pass = get_render_pass(ERenderPass::TemporalAA);

					sg_apply_uniforms(0, SG_RANGE(state.tonemapping.fs_params));

					sg_bindings bindings = (sg_bindings){
						.views = {
							[0] = state.temporal_aa.enable
								? temporal_aa_pass.get_color_output(0, temporal_aa_output_index).get_texture_view(0)
								: state.wireframe.shaded_wireframe
								? wire_overlay_pass.get_color_output(0).get_texture_view(0)
								: state.dof.enable
								? dof_combine_pass.get_color_output(0).get_texture_view(0)
								: lighting_pass.get_color_output(0).get_texture_view(0),
						},
						.samplers[0] = state.gpu.linear_sampler,
					};

					gpu_apply_bindings(&bindings);

					sg_draw(0,6,1);
				}
			);
		}

		if (state.temporal_aa.enable_fxaa)
		{
			get_render_pass(ERenderPass::FXAA).execute(
				[&](const i32)
				{
					RenderPass& tonemapping_pass = get_render_pass(ERenderPass::Tonemapping);
					const fxaa_fs_params_t fs_params = {
						.screen_size = HMM_V2((f32)state.window.render_width, (f32)state.window.render_height),
						.contrast_threshold = 0.0312f,
						.relative_threshold = 0.125f,
					};
					sg_apply_uniforms(0, SG_RANGE(fs_params));

					sg_bindings bindings = {
						.views = {
							[0] = tonemapping_pass.get_color_output(0).get_texture_view(0),
						},
						.samplers[0] = state.gpu.linear_sampler,
					};
					gpu_apply_bindings(&bindings);

					sg_draw(0,6,1);
				}
			);
		}

		{ // Debug Text
			get_render_pass(ERenderPass::DebugText).execute(
				[&](const i32)
				{
					// Larger numbers scales down text
					const f32 text_scale = 0.5f;
					sdtx_canvas(state.window.render_width * text_scale, state.window.render_height * text_scale);
					sdtx_origin(1.0f, 2.0f);
					sdtx_home();

					if (!state.runtime.blender_data_loaded)
					{
						draw_debug_text(FONT_C64, "Waiting on data from Blender\n", 255,255,255);
					}

					if (state.debug_camera.active)
					{
						draw_debug_text(FONT_C64, "Debug Camera Active\n", 255,255,255);
					}

					if (!state.runtime.is_simulating)
					{
						draw_debug_text(FONT_C64, "Simulation Paused\n", 255,255,255);
					}

					sdtx_draw();
				}
			);
		}

		{ // Copy To Swapchain Pass
			get_render_pass(ERenderPass::CopyToSwapchain).execute(
				[&](const i32)
				{
					RenderPass& tonemapping_pass = get_render_pass(ERenderPass::Tonemapping);
					RenderPass& fxaa_pass = get_render_pass(ERenderPass::FXAA);
					RenderPass& debug_text_pass = get_render_pass(ERenderPass::DebugText);

					// This can be overridden by the debug image viewer below
					GpuImage& image_to_copy_to_swapchain = state.temporal_aa.enable_fxaa
						? fxaa_pass.get_color_output(0)
						: tonemapping_pass.get_color_output(0);
					GpuImage* image_to_copy_to_swapchain_ptr = &image_to_copy_to_swapchain;

					DEBUG_UI(
						const i32 num_images = state.images.items.length();
						if (num_images > 0)
						{
							if (ImGui::CollapsingHeader("Debug Image Viewer"))
							{
								ImGui::Checkbox("Fullscreen", &state.images.enable_debug_fullscreen);
								ImGui::SliderInt(
									"Image Index",
									&state.images.debug_index,
									0, num_images - 1,
									"%d", ImGuiSliderFlags_ClampOnInput
								);

								GpuImage& image = state.images.items[state.images.debug_index];
								const ImVec2 size = ImVec2(256, 256);

								ImTextureID imtex_id = simgui_imtextureid(image.get_texture_view(0));
								ImGui::Image(imtex_id, size);
							}

							if (state.images.enable_debug_fullscreen)
							{
								image_to_copy_to_swapchain_ptr = &state.images.items[state.images.debug_index];
							}
						}
					);

					sg_bindings bindings = (sg_bindings){
						.views = {
							[0] = image_to_copy_to_swapchain_ptr->get_texture_view(0),
							[1] = debug_text_pass.get_color_output(0).get_texture_view(0),
						},
						.samplers[0] = state.gpu.nearest_sampler,
					};
					gpu_apply_bindings(&bindings);

					sg_draw(0,6,1);

					DEBUG_UI(
						ImGui::End();
						draw_cpu_profiler_window();
						simgui_render();
					);
				}
			);
		}

		{
			CPU_TIMING_SCOPE("sg_commit");
			gpu_frame_timings_end_frame(gpu_timing_frame_index);
			sg_commit();
		}
	}

	reset_mouse_delta();
}

void cleanup(void)
{
	// Tell live_link_thread we're done running and wait for it to complete
	state.runtime.game_running = false;
	state.live_link.thread.join();

	for (i32 pass_index = 0; pass_index < (i32)ERenderPass::COUNT; ++pass_index)
	{
		state.render_passes.passes[pass_index].cleanup();
	}
	SkyBakePass::cleanup();
	gi_scene.lighting_capture.cleanup();

	jolt_shutdown();

	#if WITH_DEBUG_UI
	simgui_shutdown();
	#endif // WITH_DEBUG_UI

	gpu_frame_timings_shutdown();
    sg_shutdown();
}

void event(const sapp_event* event)
{
	#if WITH_DEBUG_UI
	const bool imgui_wants_mouse_capture = g_show_imgui ? ImGui::GetIO().WantCaptureMouse : false;
	// Pass events to sokol_imgui
	simgui_handle_event(event);
	#else
	const bool imgui_wants_mouse_capture = false;
	#endif // WITH_DEBUG_UI

	switch(event->type)
	{
		case SAPP_EVENTTYPE_KEY_DOWN:
		{
			// stop execution on escape key
			if (event->key_code == SAPP_KEYCODE_ESCAPE)
			{
			 	if (event->modifiers & SAPP_MODIFIER_SHIFT)
				{
					set_mouse_locked(false);
				}
				else
				{
					sapp_quit();
				}
			}

			app_state.keycodes[event->key_code] = true;
			break;
		}
		case SAPP_EVENTTYPE_KEY_UP:
		{
			app_state.keycodes[event->key_code] = false;
			break;
		}
		case SAPP_EVENTTYPE_MOUSE_DOWN:
		{
			app_state.mouse_position = HMM_V2(event->mouse_x, event->mouse_y);

			if (!imgui_wants_mouse_capture && event->mouse_button == SAPP_MOUSEBUTTON_LEFT && state.gi.probe_isolation_enable && state.gi.show_probes)
			{
				pick_isolated_gi_probe();
				break;
			}

			// Lock Mouse on left click into game space
			if (!imgui_wants_mouse_capture && event->mouse_button == SAPP_MOUSEBUTTON_LEFT)
			{
				if (!is_mouse_locked())
				{
					set_mouse_locked(true);
				}
            }

			break;
		}
		case SAPP_EVENTTYPE_MOUSE_UP:
		{
			break;
		}
		case SAPP_EVENTTYPE_MOUSE_MOVE:
		{
			app_state.mouse_position.X = event->mouse_x;
			app_state.mouse_position.Y = event->mouse_y;
			app_state.mouse_delta.X += event->mouse_dx;
			app_state.mouse_delta.Y += event->mouse_dy;
			break;
		}
		case SAPP_EVENTTYPE_RESIZED:
		{
			handle_resize();
			break;
		}
		default: break;
	}
}

sapp_desc sokol_main(int argc, char* argv[])
{
	cxxopts::Options options("Game", "Game that uses blender as its tooling");

	options.add_options()
	  ("f,file", "File name", cxxopts::value<std::string>())
  	;

	// First positional arg can be file to load
	options.parse_positional({"file"});

	auto result = options.parse(argc, argv);

	// If we passed an init file, load it on startup
	if (result.count("file") > 0)
	{
		state.runtime.init_file = result["f"].as<std::string>();
	}

    return (sapp_desc) {
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
		.event_cb = event,
        .width = 1920,
        .height = 1080,
        .sample_count = 1,
		.swap_interval = 1, 
		.high_dpi = true,
        .window_title = "Blender Game",
        .icon = {
			.sokol_default = true,
		},
        .logger.func = slog_func,
		.win32.console_attach = true,
    };
}
