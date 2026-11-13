#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <string_view>

#include "ankerl/unordered_dense.h"
#include "blender_live_link_generated.h"
#include "live_link/live_link_types.h"

namespace LiveLinkSystem
{
	namespace LL = Blender::LiveLink;

	// Flatbuffer helper conversion functions
	namespace flatbuffer_helpers
	{
		HMM_Vec3 to_hmm_vec3(const LL::Vec3* in_flatbuffers_vector)
		{
			assert(in_flatbuffers_vector);
			return HMM_V3(
				in_flatbuffers_vector->x(),
				in_flatbuffers_vector->y(),
				in_flatbuffers_vector->z()
			);
		}

		HMM_Vec4 to_hmm_vec4(const LL::Vec3* in_flatbuffers_vector, f32 in_w)
		{
			assert(in_flatbuffers_vector);
			return HMM_V4(
				in_flatbuffers_vector->x(),
				in_flatbuffers_vector->y(),
				in_flatbuffers_vector->z(),
				in_w
			);
		}

		HMM_Vec4 to_hmm_vec4(const LL::Vec4* in_flatbuffers_vector)
		{
			assert(in_flatbuffers_vector);
			return HMM_V4(
				in_flatbuffers_vector->x(),
				in_flatbuffers_vector->y(),
				in_flatbuffers_vector->z(),
				in_flatbuffers_vector->w()
			);
		}

		HMM_Quat to_hmm_quat(const LL::Quat* in_flatbuffers_quat)
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
		HMM_Mat4 to_hmm_mat4(const LL::Matrix* in_flatbuffers_matrix)
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

	void decode_skinning(const LL::Mesh& source, i32 object_uid, MeshInitData& mesh)
	{
		const auto* joints = source.joint_indices();
		const auto* weights = source.joint_weights();
		if (mesh.armature_id <= 0 || !joints || !weights) return;

		if (joints->size() != weights->size() || joints->size() != (u64) mesh.num_vertices * 4)
		{
			printf("\tDropping malformed skinning data on object UID: %i\n", object_uid);
			return;
		}

		// The largest index sizes the skin matrix array before the armature is available.
		constexpr i32 MAX_JOINT_INDEX = 65535;
		i32 largest_joint = 0;
		for (i32 joint : *joints)
		{
			if (joint < 0 || joint > MAX_JOINT_INDEX)
			{
				printf("\tDropping skinning with joint index outside [0, %i] on object UID: %i\n",
					MAX_JOINT_INDEX, object_uid);
				return;
			}
			largest_joint = MAX(largest_joint, joint);
		}
		mesh.skin_matrix_count = (u32) largest_joint + 1;
		mesh.skinned_vertices = (SkinnedVertex*) malloc(sizeof(SkinnedVertex) * mesh.num_vertices);
		for (u32 vertex = 0; vertex < mesh.num_vertices; ++vertex)
		{
			for (u32 influence = 0; influence < 4; ++influence)
			{
				const u32 index = vertex * 4 + influence;
				mesh.skinned_vertices[vertex].joint_indices.Elements[influence] = (f32) joints->Get(index);
				mesh.skinned_vertices[vertex].joint_weights.Elements[influence] = weights->Get(index);
			}
		}
	}

	void decode_mesh(const LL::Mesh& source, Object& object)
	{
		const auto* positions = source.positions();
		const auto* normals = source.normals();
		const auto* texcoords = source.texcoords();
		if (!positions || !normals || !texcoords || positions->size() % 3 != 0
			|| normals->size() < positions->size() || texcoords->size() < positions->size() / 3 * 2)
		{
			printf("\tDropping malformed mesh vertex streams on object UID: %i\n", object.unique_id);
			return;
		}

		MeshInitData mesh;
		mesh.num_vertices = positions->size() / 3;
		const auto* indices = source.indices();
		if (!mesh.num_vertices || !indices || indices->size() == 0) return;
		if (indices->size() % 3 != 0)
		{
			printf("\tDropping malformed triangle index stream on object UID: %i\n", object.unique_id);
			return;
		}
		for (u32 index : *indices)
		{
			if (index >= mesh.num_vertices)
			{
				printf("\tDropping triangle indices past vertex count %u on object UID: %i\n",
					mesh.num_vertices, object.unique_id);
				return;
			}
		}

		mesh.num_indices = indices->size();
		mesh.indices = (u32*) malloc(sizeof(u32) * mesh.num_indices);
		std::copy(indices->begin(), indices->end(), mesh.indices);
		mesh.vertices = (Vertex*) malloc(sizeof(Vertex) * mesh.num_vertices);
		for (u32 i = 0; i < mesh.num_vertices; ++i)
		{
			mesh.vertices[i] = {
				.position = HMM_V4(positions->Get(i * 3), positions->Get(i * 3 + 1), positions->Get(i * 3 + 2), 1.0f),
				.normal = HMM_V4(normals->Get(i * 3), normals->Get(i * 3 + 1), normals->Get(i * 3 + 2), 0.0f),
				.texcoord = HMM_V2(texcoords->Get(i * 2), texcoords->Get(i * 2 + 1)),
			};
		}

		if (const auto* ids = source.material_ids(); ids && ids->size() > 0)
		{
			mesh.num_material_indices = ids->size();
			mesh.material_indices = (i32*) malloc(sizeof(i32) * ids->size());
			std::copy(ids->begin(), ids->end(), mesh.material_indices);
		}
		mesh.armature_id = source.armature_id();
		mesh.mesh_to_armature = flatbuffer_helpers::to_hmm_mat4(source.mesh_to_armature());
		mesh.armature_to_mesh = flatbuffer_helpers::to_hmm_mat4(source.armature_to_mesh());
		decode_skinning(source, object.unique_id, mesh);
		object.mesh = make_mesh(mesh);
		object.has_mesh = true;
	}

	void decode_component(const LL::GameplayComponentContainer& component, Object& game_object)
	{
		if (!component.value()) return;
		switch (component.value_type())
		{
			case LL::GameplayComponent_GameplayComponentCharacter:
			{
				const auto* character_component = component.value_as_GameplayComponentCharacter();

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
						.height = character_component->height(),
						.radius = character_component->radius(),
					},
				};
				break;
			}
			case LL::GameplayComponent_GameplayComponentEnemy:
			{
				const auto* enemy_component = component.value_as_GameplayComponentEnemy();
				auto non_negative = [](f32 value, f32 fallback) { return std::isfinite(value) ? MAX(value, 0.0f) : fallback; };
				game_object.has_enemy = true;
				game_object.enemy = {};
				game_object.enemy.settings = {
					.roam_radius = non_negative(enemy_component->roam_radius(), 15.0f),
					.engage_range = non_negative(enemy_component->engage_range(), 40.0f),
				};
				// Seeded by uid so each enemy roams differently but reproducibly
				game_object.enemy.rng.seed((u32) game_object.unique_id);
				break;
			}
			case LL::GameplayComponent_GameplayComponentFogController:
			{
				const auto* fog_component = component.value_as_GameplayComponentFogController();

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
			case LL::GameplayComponent_GameplayComponentSkyAtmosphere:
			{
				const auto* sky_component = component.value_as_GameplayComponentSkyAtmosphere();
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
			case LL::GameplayComponent_GameplayComponentCloudSystem:
			{
				const auto* cloud_component = component.value_as_GameplayComponentCloudSystem();
				game_object.has_cloud_system = true;
				CloudSystem& cloud = game_object.cloud_system;
				auto finite_or = [](f32 value, f32 fallback) {
					return std::isfinite(value) ? value : fallback;
				};
				cloud.enabled = cloud_component->enabled();
				cloud.seed = cloud_component->seed();
				cloud.weather_world_scale_m = MAX(
					finite_or(cloud_component->weather_world_scale_m(), 100000.0f), 1000.0f);
				if (const LL::Vec2* wind = cloud_component->wind_direction())
				{
					cloud.wind_direction = HMM_V2(
						finite_or(wind->x(), 1.0f), finite_or(wind->y(), 0.0f));
				}
				cloud.wind_speed_m_s = CLAMP(
					finite_or(cloud_component->wind_speed_m_s(), 20.0f), 0.0f, 200.0f);
				cloud.shadow_enabled = cloud_component->shadow_enabled();
				cloud.shadow_extent_m = MAX(
					finite_or(cloud_component->shadow_extent_m(), 8000.0f), 1000.0f);
				const i32 payload_layer_count = cloud_component->layers()
					? (i32)cloud_component->layers()->size() : 0;
				if (payload_layer_count > MAX_CLOUD_LAYERS)
					printf("Cloud system warning: capped %i payload layers to %i\n",
						payload_layer_count, MAX_CLOUD_LAYERS);
				cloud.layer_count = 0;
				for (i32 layer_index = 0;
					layer_index < MIN(payload_layer_count, MAX_CLOUD_LAYERS); ++layer_index)
				{
					const LL::CloudLayer* source =
						cloud_component->layers()->Get(layer_index);
					if (!source) continue;
					CloudLayer& layer = cloud.layers[cloud.layer_count++];
					layer.enabled = source->enabled();
					layer.profile = (CloudLayerProfile) CLAMP((i32) source->profile(), 0, 3);
					layer.seed_offset = source->seed_offset();
					layer.base_altitude_m = MAX(finite_or(source->base_altitude_m(), 1800.0f), 0.0f);
					layer.thickness_m = MAX(finite_or(source->thickness_m(), 3000.0f), 10.0f);
					layer.coverage = CLAMP(finite_or(source->coverage(), 0.5f), 0.0f, 1.0f);
					layer.density = CLAMP(finite_or(source->density(), 1.0f), 0.0f, 4.0f);
					layer.shape_scale_m = MAX(finite_or(source->shape_scale_m(), 8000.0f), 100.0f);
					layer.detail_scale_m = MAX(finite_or(source->detail_scale_m(), 1000.0f), 10.0f);
					layer.erosion = CLAMP(finite_or(source->erosion(), 0.65f), 0.0f, 1.0f);
					layer.anvil_bias = CLAMP(finite_or(source->anvil_bias(), 0.1f), 0.0f, 1.0f);
					layer.wind_multiplier = CLAMP(finite_or(source->wind_multiplier(), 1.0f), -4.0f, 4.0f);
					layer.phase_forward = CLAMP(finite_or(source->phase_forward(), 0.75f), -0.95f, 0.95f);
					layer.phase_backward = CLAMP(finite_or(source->phase_backward(), -0.25f), -0.95f, 0.95f);
					layer.phase_blend = CLAMP(finite_or(source->phase_blend(), 0.8f), 0.0f, 1.0f);
					layer.ambient_scale = CLAMP(finite_or(source->ambient_scale(), 0.6f), 0.0f, 2.0f);
					layer.multi_scattering_strength =
						CLAMP(finite_or(source->multi_scattering_strength(), 0.8f), 0.0f, 1.0f);
				}
				std::sort(cloud.layers, cloud.layers + cloud.layer_count,
					[](const CloudLayer& a, const CloudLayer& b) {
						return a.base_altitude_m < b.base_altitude_m;
					});
				break;
			}
			case LL::GameplayComponent_GameplayComponentPhysics:
			{
				const auto* physics_component = component.value_as_GameplayComponentPhysics();
				game_object.has_rigid_body = true;
				game_object.rigid_body = (RigidBody) {
					.is_dynamic = physics_component->is_dynamic(),
					.mass = std::isfinite(physics_component->mass()) ? MAX(physics_component->mass(), 0.001f) : 1.0f,
					.is_mesh_collider = physics_component->type() == LL::PhysicsType_PerPoly,
					.jolt_body = nullptr,	// created at drain on the main thread
				};
				break;
			}
			case LL::GameplayComponent_GameplayComponentTessellation:
			{
				const auto* tessellation_component = component.value_as_GameplayComponentTessellation();
				game_object.has_tessellation = true;
				game_object.tessellation = (TessellationSettings) {
					.displacement_scale = std::isfinite(tessellation_component->displacement_scale())
						? MAX(tessellation_component->displacement_scale(), 0.0f) : 1.0f,
					.max_factor_override = CLAMP(tessellation_component->max_factor_override(), 0, 31),
				};
				break;
			}
			case LL::GameplayComponent_GameplayComponentCameraControl:
			{
				const auto* cam_control_component = component.value_as_GameplayComponentCameraControl();

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
			case LL::GameplayComponent_GameplayComponentWeapon:
			{
				const auto* weapon = component.value_as_GameplayComponentWeapon();
				game_object.has_weapon = true;
				game_object.weapon.accepted_bone_label = weapon->accepted_bone_label() ? weapon->accepted_bone_label()->str() : "";
				game_object.weapon.muzzle_local_transform = flatbuffer_helpers::to_hmm_mat4(weapon->muzzle_local_transform());
				game_object.weapon.muzzle_valid = weapon->muzzle_valid();
				game_object.weapon.rate_of_fire_seconds = weapon->rate_of_fire_seconds();
				break;
			}
			case LL::GameplayComponent_GameplayComponentPart:
			{
				const auto* part_component = component.value_as_GameplayComponentPart();
				game_object.has_part = true;
				game_object.part = {
					.type = (PartType) part_component->part_type(),
					.ik_shoulder_bone = flatbuffers::GetString(part_component->ik_shoulder_bone()),
					.ik_elbow_bone = flatbuffers::GetString(part_component->ik_elbow_bone()),
					.ik_hand_bone = flatbuffers::GetString(part_component->ik_hand_bone()),
					.ik_left_hip_bone = flatbuffers::GetString(part_component->ik_left_hip_bone()),
					.ik_left_knee_bone = flatbuffers::GetString(part_component->ik_left_knee_bone()),
					.ik_left_hock_bone = flatbuffers::GetString(part_component->ik_left_hock_bone()),
					.ik_left_foot_bone = flatbuffers::GetString(part_component->ik_left_foot_bone()),
					.ik_right_hip_bone = flatbuffers::GetString(part_component->ik_right_hip_bone()),
					.ik_right_knee_bone = flatbuffers::GetString(part_component->ik_right_knee_bone()),
					.ik_right_hock_bone = flatbuffers::GetString(part_component->ik_right_hock_bone()),
					.ik_right_foot_bone = flatbuffers::GetString(part_component->ik_right_foot_bone()),
				};
				break;
			}
			case LL::GameplayComponent_GameplayComponentAttachmentPoint:
			{
				const auto* attachment_component = component.value_as_GameplayComponentAttachmentPoint();
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
				break;
		}
	}

	std::optional<SceneUpdate> decode_update(std::span<const u8> payload)
	{
		if (payload.empty())
		{
			return std::nullopt;
		}

		// Trust boundary: bytes come from any local process (or a -f file), and
		// unverified offsets read out of bounds. Bad frames are dropped.
		flatbuffers::Verifier verifier(payload.data(), payload.size());
		if (!LL::VerifySizePrefixedUpdateBuffer(verifier))
		{
			printf("[live-link] dropping malformed update (%zu bytes)\n", payload.size());
			return std::nullopt;
		}

		auto* update = LL::GetSizePrefixedUpdate(payload.data());

		// Everything in this Update is packaged into one SceneUpdate message and
		// registered/applied on the main thread at drain time
		SceneUpdate scene_update;
		scene_update.reset = update->reset();
		scene_update.stats.byte_count = payload.size();
		scene_update.stats.generation_seconds = update->generation_seconds();
		scene_update.stats.reset = scene_update.reset;

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
						// Match Blender's viewport position and viewing direction without
						// importing viewport roll into the game's Z-up navigation frame.
						// A vertical view cannot use world Z as its up vector, so retain
						// Blender's orthogonal up vector for zenith/nadir smoke tests.
						const HMM_Vec3 navigation_up =
							fabsf(HMM_DotV3(camera.forward, UnitVectors::Up)) < 0.999f
							? UnitVectors::Up
							: camera.up;
						scene_update.editor_camera = Camera{
							.location = camera.location,
							.forward = camera.forward,
							.up = navigation_up,
						};
					}
				}
			}
		}

		// The queued update owns the pixels after the source frame is released.
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

				PendingImage& pending = scene_update.images.emplace();
				pending.unique_id = image->unique_id();
				pending.width = width;
				pending.height = height;
				pending.pixels.assign(image_data->data(), image_data->data() + expected_size);

				const u64 size_hash = ((u64) width << 32) | (u32) height;
				const u64 pixel_hash = ankerl::unordered_dense::hash<std::string_view>{}(
					std::string_view((const char*) pending.pixels.data(), expected_size));
				pending.content_hash = pixel_hash ^ (size_hash * 0x9E3779B97F4A7C15ull);
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
					.displacement_scale = material->displacement_scale(),
					.displacement_midpoint = material->displacement_midpoint(),
					.base_color_image_id = material->base_color_image_id(),
					.emission_color_image_id = material->emission_color_image_id(),
					.metallic_image_id = material->metallic_image_id(),
					.roughness_image_id = material->roughness_image_id(),
					.displacement_image_id = material->displacement_image_id(),
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

				if (const auto* mesh = object->mesh())
				{
					decode_mesh(*mesh, game_object);
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
								.attachment_label = copy_flatbuffer_string(flatbuffer_bone->attachment_label()),
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

							// The counts and the matrix data arrive separately: keep the clip's
							// matrices only when the data is exactly frame_count * bone_count
							// matrices. Without them armature_sample_pose uses the bind pose.
							auto flatbuffer_skin_matrices = flatbuffer_animation->skin_matrices();
							const u32 available_float_count = flatbuffer_skin_matrices ? flatbuffer_skin_matrices->size() : 0;
							const bool has_matching_matrices =
								animation.frame_count > 0 &&
								animation.bone_count > 0 &&
								available_float_count % 16 == 0 &&
								(i64) animation.frame_count * animation.bone_count == available_float_count / 16;
							if (has_matching_matrices)
							{
								const u32 matrix_count = available_float_count / 16;
								scene_update.stats.animation_matrix_count += (i32) matrix_count;
								animation.skin_matrices = (HMM_Mat4*) malloc(sizeof(HMM_Mat4) * matrix_count);
								for (u32 matrix_idx = 0; matrix_idx < matrix_count; ++matrix_idx)
								{
									for (u32 col = 0; col < 4; ++col)
									{
										for (u32 row = 0; row < 4; ++row)
										{
											animation.skin_matrices[matrix_idx].Elements[col][row] = flatbuffer_skin_matrices->Get(matrix_idx * 16 + col * 4 + row);
										}
									}
								}
							}
							else if (available_float_count > 0 || animation.frame_count * (i64) animation.bone_count != 0)
							{
								printf("\tDropping malformed animation matrices on object UID: %i\n", unique_id);
							}
						}
					}
				}

				if (auto object_light = object->light())
				{
					LightType light_type = (LightType) object_light->type();

					// color and the per-type tables are optional in the schema: a
					// missing one drops only the light, like an unknown type does.
					const auto* light_color = object_light->color();
					game_object.has_light = light_color != nullptr;
					if (!light_color)
					{
						printf("\t\tLight on object UID %i has no color; dropping light\n", unique_id);
					}
					else
					{
						game_object.light = (Light){
							.type = light_type,
							.color = flatbuffer_helpers::to_hmm_vec3(light_color),
						};

						switch (light_type)
						{
							case LightType::Point:
							{
								auto point_light = object_light->point_light();
								if (!point_light) { game_object.has_light = false; break; }
								game_object.light.point = (PointLight) {
									.power = point_light->power(),
								};
								break;
							}
							case LightType::Spot:
							{
								auto spot_light = object_light->spot_light();
								if (!spot_light) { game_object.has_light = false; break; }
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
								if (!sun_light) { game_object.has_light = false; break; }
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
								printf("\t\tUnsupported light type %i on object UID %i; dropping light\n",
									(i32) light_type, unique_id);
								game_object.has_light = false;
								break;
						}
					}
				}

				if (const auto* components = object->components())
				{
					for (const auto* component : *components)
					{
						if (component) decode_component(*component, game_object);
					}
				}
				scene_update.objects.add(std::move(game_object));
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

		return scene_update;
	}
}
