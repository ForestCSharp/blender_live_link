#pragma once

#include <cfloat>

#include <cmath>

#include "state/state.h"

namespace AnimationSystem
{
	// ---- Skinned animation ----
	
	void rewind(State& in_state)
	{
		scene_ensure_indexes(in_state);
		for (i32 armature_object_id : in_state.scene.indexes.armature_object_ids)
		{
			auto found = in_state.scene.objects.find(armature_object_id);
			if (found == in_state.scene.objects.end())
			{
				continue;
			}
	
			Armature& armature = found->second.armature;
			armature.playback_time = 0.0f;
			armature.current_frame = 0;
		}
	}
	
	// Advances all armatures before mech attachment evaluation so bone sockets
	// and rendered skinning consume the same animation frame.
	void advance(State& in_state, f32 in_delta_time)
	{
		scene_ensure_indexes(in_state);
		in_state.data_oriented.frame.animation_armature_candidates += (i32) in_state.scene.indexes.armature_object_ids.length();
	
		// Phase A: advance armature playback
		for (i32 armature_object_id : in_state.scene.indexes.armature_object_ids)
		{
			auto found = in_state.scene.objects.find(armature_object_id);
			if (found == in_state.scene.objects.end())
			{
				continue;
			}
	
			Armature& armature = found->second.armature;
			AnimationClip* animation = armature_get_active_animation(armature);
			if (!animation || animation->frame_count <= 0)
			{
				continue;
			}
	
			if (in_state.runtime.is_simulating && in_state.animation.is_playing && in_state.animation.playback_rate > 0.0f)
			{
				armature.playback_time += in_delta_time * in_state.animation.playback_rate;
	
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
			in_state.data_oriented.frame.animation_armatures_updated += 1;
		}
	}
	
	// Computes each skinned mesh's final matrices (armature_to_mesh * clip *
	// mesh_to_armature) and packs the shared per-frame arena.
	void pack_skin_matrices(State& in_state)
	{
		scene_ensure_indexes(in_state);
		in_state.skin_matrices.items.clear();
	
		in_state.data_oriented.frame.animation_skinned_mesh_candidates += (i32) in_state.scene.indexes.skinned_mesh_object_ids.length();
		for (i32 skinned_object_id : in_state.scene.indexes.skinned_mesh_object_ids)
		{
			auto found = in_state.scene.objects.find(skinned_object_id);
			if (found == in_state.scene.objects.end())
			{
				continue;
			}
	
			Mesh& mesh = found->second.mesh;
			mesh.skin_matrix_arena_offset = -1;
			mesh.skinned_local_bounds_valid = false;
			if (!mesh.has_skinned_vertices || mesh.skin_matrix_count == 0 || !mesh.skin_matrices)
			{
				continue;
			}
	
			mesh_reset_skin_matrices(mesh);
	
			// The armature is a separate scene object referenced by id
			auto armature_found = in_state.scene.objects.find(mesh.armature_id);
			if (armature_found != in_state.scene.objects.end() && armature_found->second.has_armature)
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
	
			// Conservative deformed bounds, built from the same matrices being
			// packed. A deformed vertex is a convex blend of (L_i * v + t_i), so
			// it never leaves the AABB of the bone translations expanded by
			// max||L_i|| * bind-pose radius. The Frobenius norm is an upper bound
			// on the operator norm, which keeps the estimate safe rather than
			// tight. Unwritten bones were reset to identity above, so their
			// t = 0 is included and covers vertices that stay in bind pose.
			const f32 bind_pose_radius = mesh_bind_pose_radius(mesh);
			HMM_Vec3 deformed_min = HMM_V3(FLT_MAX, FLT_MAX, FLT_MAX);
			HMM_Vec3 deformed_max = HMM_V3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
			f32 max_linear_norm = 0.0f;

			mesh.skin_matrix_arena_offset = (i32) in_state.skin_matrices.items.length();
			for (u32 matrix_idx = 0; matrix_idx < mesh.skin_matrix_count; ++matrix_idx)
			{
				const HMM_Mat4& skin_matrix = mesh.skin_matrices[matrix_idx];

				const HMM_Vec3 translation = skin_matrix.Columns[3].XYZ;
				deformed_min = HMM_MinV3(deformed_min, translation);
				deformed_max = HMM_MaxV3(deformed_max, translation);

				f32 linear_norm_squared = 0.0f;
				for (i32 column_index = 0; column_index < 3; ++column_index)
				{
					linear_norm_squared += HMM_LenSqrV3(skin_matrix.Columns[column_index].XYZ);
				}
				max_linear_norm = MAX(max_linear_norm, sqrtf(linear_norm_squared));

				in_state.skin_matrices.items.add(skin_matrix);
			}

			const f32 bounds_padding = max_linear_norm * bind_pose_radius;
			const HMM_Vec3 padding_vector = HMM_V3(bounds_padding, bounds_padding, bounds_padding);
			mesh.skinned_local_bounds = {
				.min = deformed_min - padding_vector,
				.max = deformed_max + padding_vector,
			};
			mesh.skinned_local_bounds_valid = mesh.skin_matrix_count > 0;
			in_state.data_oriented.frame.animation_skin_matrix_uploads += 1;
		}
	
		skin_matrix_arena_upload(in_state);
	}
}

