#pragma once

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
	
			mesh.skin_matrix_arena_offset = (i32) in_state.skin_matrices.items.length();
			for (u32 matrix_idx = 0; matrix_idx < mesh.skin_matrix_count; ++matrix_idx)
			{
				in_state.skin_matrices.items.add(mesh.skin_matrices[matrix_idx]);
			}
			in_state.data_oriented.frame.animation_skin_matrix_uploads += 1;
		}
	
		skin_matrix_arena_upload(in_state);
	}
}

