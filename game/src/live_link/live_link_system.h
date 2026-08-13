#pragma once

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

#include "blender_live_link_generated.h"
#include "core/dynamic_array.h"
#include "render/imgui_layer.h"
#include "state/state.h"

namespace LiveLinkSystem
{
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
		// no transpose is required.
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
	void parse_flatbuffer_data(DynamicArray<u8>& flatbuffer_data)
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
	
		if (auto editor_camera = update->editor_camera())
		{
			auto location_fb = editor_camera->location();
			auto forward_fb = editor_camera->forward();
			auto up_fb = editor_camera->up();
			if (location_fb && forward_fb && up_fb)
			{
				Camera camera = {
					.location = flatbuffer_helpers::to_hmm_vec3(location_fb),
					.forward = flatbuffer_helpers::to_hmm_vec3(forward_fb),
					.up = flatbuffer_helpers::to_hmm_vec3(up_fb),
				};
				const bool finite =
					std::isfinite(camera.location.X) &&
					std::isfinite(camera.location.Y) &&
					std::isfinite(camera.location.Z) &&
					std::isfinite(camera.forward.X) &&
					std::isfinite(camera.forward.Y) &&
					std::isfinite(camera.forward.Z) &&
					std::isfinite(camera.up.X) &&
					std::isfinite(camera.up.Y) &&
					std::isfinite(camera.up.Z);
				const bool non_degenerate =
					HMM_LenSqrV3(camera.forward) > 1.0e-12f &&
					HMM_LenSqrV3(camera.up) > 1.0e-12f;
				if (finite && non_degenerate)
				{
					camera.forward = HMM_NormV3(camera.forward);
					camera.up = HMM_NormV3(camera.up);
					if (fabsf(HMM_DotV3(camera.forward, camera.up)) < 0.999f)
					{
						scene_update.editor_camera = Camera{.location = camera.location, .forward = camera.forward, .up = UnitVectors::Up};
					}
				}
			}
		}
	
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
			scene_update.has_object_batch = true;
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
	
					// Parse optional skinning data.
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
					// maps them to indices when the main thread drains this update.
					// Registration deliberately happens after parsing.
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
	
				// Parse armature bones and animation clips.
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
							case Blender::LiveLink::GameplayComponent_GameplayComponentSkyAtmosphere:
							{
								using Blender::LiveLink::GameplayComponentSkyAtmosphere;
								const GameplayComponentSkyAtmosphere* sky_component =
									reinterpret_cast<const GameplayComponentSkyAtmosphere*>(component);
								const f32 atmosphere_height = CLAMP(sky_component->atmosphere_height_m(), 10000.0f, 200000.0f);
								const HMM_Vec3 ground_albedo = sky_component->ground_albedo()
									? flatbuffer_helpers::to_hmm_vec3(sky_component->ground_albedo())
									: HMM_V3(0.1f, 0.1f, 0.1f);
								game_object.has_sky_atmosphere = true;
								const f32 authored_planet_center_z = sky_component->planet_center_z_m();
								game_object.sky_atmosphere = (SkyAtmosphere) {
									.enabled = sky_component->enabled(),
									.planet_center_z_m = std::isfinite(authored_planet_center_z)
										? CLAMP(authored_planet_center_z, -1.0e9f, 1.0e9f)
										: -6360000.0f,
									.air_density = CLAMP(sky_component->air_density(), 0.0f, 4.0f),
									.aerosol_density = CLAMP(sky_component->aerosol_density(), 0.0f, 10.0f),
									.ozone_density = CLAMP(sky_component->ozone_density(), 0.0f, 4.0f),
									.ground_albedo = HMM_V3(
										CLAMP(ground_albedo.X, 0.0f, 1.0f),
										CLAMP(ground_albedo.Y, 0.0f, 1.0f),
										CLAMP(ground_albedo.Z, 0.0f, 1.0f)),
									.sky_intensity = CLAMP(sky_component->sky_intensity(), 0.0f, 20.0f),
									.sun_disc_angular_diameter_degrees = CLAMP(sky_component->sun_disc_angular_diameter_degrees(), 0.01f, 10.0f),
									.sun_disc_intensity = CLAMP(sky_component->sun_disc_intensity(), 0.0f, 20.0f),
									.atmosphere_height_m = atmosphere_height,
									.rayleigh_scale_height_m = CLAMP(sky_component->rayleigh_scale_height_m(), 1000.0f, 30000.0f),
									.mie_scale_height_m = CLAMP(sky_component->mie_scale_height_m(), 100.0f, 10000.0f),
									.mie_anisotropy = CLAMP(sky_component->mie_anisotropy(), 0.0f, 0.95f),
									.max_sun_zenith_angle_degrees = CLAMP(sky_component->max_sun_zenith_angle_degrees(), 90.0f, 120.0f),
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
									break;
							}
							case Blender::LiveLink::GameplayComponent_GameplayComponentPart:
							{
								using Blender::LiveLink::GameplayComponentPart;
								const GameplayComponentPart* part_component = reinterpret_cast<const GameplayComponentPart*>(component);
								game_object.has_part = true;
								game_object.part = {
									.type = (PartType) part_component->part_type(),
								};
								break;
							}
							case Blender::LiveLink::GameplayComponent_GameplayComponentAttachmentPoint:
							{
								using Blender::LiveLink::GameplayComponentAttachmentPoint;
								const GameplayComponentAttachmentPoint* attachment_component = reinterpret_cast<const GameplayComponentAttachmentPoint*>(component);
								game_object.has_attachment_point = true;
								game_object.attachment_point = {
									.owner_part_id = attachment_component->owner_part_id(),
									.part_type = (PartType) attachment_component->part_type(),
									.binding_type = (AttachmentBindingType) attachment_component->binding_type(),
									.armature_id = attachment_component->armature_id(),
									.bone_name = copy_flatbuffer_string(attachment_component->bone_name()),
									.local_transform = flatbuffer_helpers::to_hmm_mat4(attachment_component->local_transform()),
									.valid = attachment_component->valid(),
								};
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
		state.live_link.scene_updates.send(std::move(scene_update));
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
			DynamicArray<u8> flatbuffer_data;
	
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
	// a bindless array slot. Takes ownership of pending.pixels and frees it in
	// all paths.
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
		// image.pixels are scene-linear floats * 255). Banding in
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
	
	// Images are only destroyed on scene reset; individual removal is unsupported.
	// Retirement keeps old views/allocations alive until their frame fence.
	void reset_images()
	{
		if (state.images.items.length() > 0)
		{
			for (GpuImage& image : state.images.items)
			{
				ImGuiLayer::unregister_texture(image.view);
				for (VkImageView layer_view : image.layer_views)
				{
					ImGuiLayer::unregister_texture(layer_view);
				}
				vulkan_context_retire_image(&state.vk, image);
			}
		}
		state.images.items.reset();
		state.images.id_to_index.clear();
	}
	
	// Registers one material (main thread). Returns true when it was newly
	// appended. Updates to an already registered id are ignored until a scene
	// reset.
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
		// registered just before materials; id 0 means no image).
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
	// while frames are in flight. Per-message processing order is:
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
			if (scene_update.has_object_batch)
			{
				state.runtime.blender_data_loaded = true;
			}
	
			if (!state.debug_camera.live_link_initialization_complete)
			{
				state.debug_camera.live_link_initialization_complete = true;
				if (scene_update.editor_camera)
				{
					state.debug_camera.camera = *scene_update.editor_camera;
					state.debug_camera.initial_location = state.debug_camera.camera.location;
					const Camera& camera = state.debug_camera.camera;
					printf(
						"Debug camera initialized from Blender viewport: "
						"location=(%.3f, %.3f, %.3f) forward=(%.3f, %.3f, %.3f) up=(%.3f, %.3f, %.3f)\n",
						camera.location.X,
						camera.location.Y,
						camera.location.Z,
						camera.forward.X,
						camera.forward.Y,
						camera.forward.Z,
						camera.up.X,
						camera.up.Y,
						camera.up.Z
					);
				}
				else
				{
					printf("Debug camera using built-in fallback; first Live Link update had no valid Blender viewport camera\n");
				}
			}
	
			// Runtime clones borrow catalog allocations, so remove them before any
			// template in this complete Live Link batch can be replaced or deleted.
			mech_suspend_runtime_objects();
	
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
	
				// Create the Jolt body on the drained copy BEFORE the map insert
				// so the JPH::Body* is copied into the map.
				if (updated_object.has_rigid_body)
				{
					object_add_jolt_body(updated_object);
				}
	
				// Finalize the character here too: parsing only fills settings,
				// because Jolt objects must be created on the main thread.
				if (updated_object.has_character)
				{
					updated_object.character = character_create(jolt_state, updated_object.character.settings);
				}
	
				const bool selects_player =
					updated_object.has_character && updated_object.character.settings.player_controlled;
				const bool selects_camera = updated_object.has_camera_control;
				scene_insert_or_replace_object(state, std::move(updated_object));
				if (selects_player)
				{
					state.scene.player_character_id = updated_object_uid;
				}
				if (selects_camera)
				{
					state.scene.camera_control_id = updated_object_uid;
				}
			}
	
			// Deleted objects
			for (i32 deleted_object_uid : scene_update.deleted_object_uids)
			{
				state.data_oriented.frame.live_link_deleted_objects += 1;
				if (scene_remove_object(state, deleted_object_uid))
				{
					printf("Removing object. UID: %i\n", deleted_object_uid);
				}
			}
	
			// Reset
			if (scene_update.reset)
			{
				state.data_oriented.frame.live_link_reset_count += 1;
				state.runtime.blender_data_loaded = false;
				mech_reset_all();
				scene_clear_objects(state);
				reset_materials();
				reset_images();
			}
	
			if (!scene_update.reset)
			{
				mech_reconcile_instances();
			}
		}
	}

	inline bool load_initial_file(State& in_state, const std::string& in_path)
	{
		(void) in_state;
		if (FILE* file = fopen(in_path.c_str(), "rb"))
		{
			fseek(file, 0, SEEK_END);
			const long file_size = ftell(file);
			rewind(file);
			assert(file_size > 0);

			DynamicArray<u8> flatbuffer_data;
			flatbuffer_data.add_uninitialized(file_size);
			const size_t bytes_read = fread(flatbuffer_data.data(), 1, file_size, file);
			fclose(file);
			assert(bytes_read == (size_t) file_size);

			parse_flatbuffer_data(flatbuffer_data);
			return true;
		}

		printf("Failed to open init file: %s\n", in_path.c_str());
		return false;
	}

	inline void start(State& in_state)
	{
		in_state.live_link.thread = std::thread(live_link_thread_function);
	}

	inline void drain(State& in_state)
	{
		(void) in_state;
		live_link_drain_channels();
	}

	inline void stop(State& in_state)
	{
		in_state.runtime.game_running = false;
		if (in_state.live_link.thread.joinable())
		{
			in_state.live_link.thread.join();
		}
	}

	inline void cleanup_imported_resources(State& in_state)
	{
		(void) in_state;
		reset_images();
	}
}
