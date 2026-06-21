#pragma once

#include "core/types.h"
#include "state/state.h"

#define WITH_VERBOSE_CULL_RESULTS 1

struct CullResult
{
	// Object IDs remaining after culling. Ownership stays in State::SceneState.
	StretchyBuffer<i32> object_ids;

	// Number of objects this cull result rejected
	i32 cull_count = 0;
	i32 candidate_count = 0;

	#if WITH_VERBOSE_CULL_RESULTS
	i32 non_renderable_cull_count = 0;
	i32 visibility_cull_count = 0;
	i32 frustum_cull_count = 0;
	#endif
};

CullResult cull_objects(State& in_state, const HMM_Mat4& in_view_proj, f32 in_bounds_padding = 0.0f)
{
	scene_ensure_indexes(in_state);

	CullResult out_cull_result = {
		.cull_count = 0,
		.candidate_count = (i32)in_state.scene.indexes.mesh_object_ids.length(),
	};

	Frustum frustum = frustum_create(in_view_proj);
	const i32 total_object_count = (i32)in_state.scene.objects.size();
	const i32 mesh_object_count = (i32)in_state.scene.indexes.mesh_object_ids.length();
	const i32 non_renderable_count = MAX(0, total_object_count - mesh_object_count);
	out_cull_result.cull_count += non_renderable_count;
	#if WITH_VERBOSE_CULL_RESULTS
	out_cull_result.non_renderable_cull_count += non_renderable_count;
	#endif

	for (const i32 unique_id : in_state.scene.indexes.mesh_object_ids)
	{
		if (!in_state.scene.objects.contains(unique_id))
		{
			out_cull_result.cull_count += 1;
			#if WITH_VERBOSE_CULL_RESULTS
			out_cull_result.non_renderable_cull_count += 1;
			#endif
			continue;
		}

		Object& object = in_state.scene.objects[unique_id];

		// Cull invisible objects
		if (!object.visibility)
		{
			out_cull_result.cull_count += 1;
			#if WITH_VERBOSE_CULL_RESULTS
			out_cull_result.visibility_cull_count += 1;
	  		#endif
			continue;
		}

		// TODO: Compute animated bounds for skinned meshes so they can be frustum culled safely.
		if (object.mesh.has_skinned_vertices)
		{
			out_cull_result.object_ids.add(unique_id);
			in_state.data_oriented.frame.cull_skinned_visible_count += 1;
			#if WITH_VERBOSE_CULL_RESULTS
			out_cull_result.frustum_cull_count += 0;
			#endif
			continue;
		}

		// Frustum Cull
		BoundingBox object_bounding_box = object_get_bounding_box(object);
		if (in_bounds_padding > 0.0f)
		{
			HMM_Vec3 padding = HMM_V3(in_bounds_padding, in_bounds_padding, in_bounds_padding);
			object_bounding_box.min -= padding;
			object_bounding_box.max += padding;
		}
		if (frustum_cull(frustum, object_bounding_box))
		{
			out_cull_result.cull_count += 1;
			#if WITH_VERBOSE_CULL_RESULTS
			out_cull_result.frustum_cull_count += 1;
	  		#endif
			continue;
		}

		out_cull_result.object_ids.add(unique_id);
	}

	in_state.data_oriented.frame.cull_calls += 1;
	in_state.data_oriented.frame.cull_candidate_count += out_cull_result.candidate_count;
	in_state.data_oriented.frame.cull_visible_count += (i32)out_cull_result.object_ids.length();
	in_state.data_oriented.frame.cull_non_renderable_count += out_cull_result.non_renderable_cull_count;
	in_state.data_oriented.frame.cull_visibility_count += out_cull_result.visibility_cull_count;
	in_state.data_oriented.frame.cull_frustum_count += out_cull_result.frustum_cull_count;

	return out_cull_result;
}
