#pragma once

#include "core/types.h"
#include "core/dynamic_array.h"
#include "core/timings.h"

// CPU frustum culling.
// Frustum/frustum_create/frustum_cull live in core/types.h.
//
// World-space bounds are transformed once per frame in
// build_render_object_snapshot() and cached in State::cull_entries, so a cull
// is a flat scan over one contiguous array with no hash lookups and no
// per-view AABB transforms. That matters because a frame culls four or more
// views: the main geometry pass, one per shadow cascade, and one per GI probe
// cube face while GI is converging.

struct CullStats
{
	i32 candidate_count = 0;
	i32 non_renderable_cull_count = 0;
	i32 visibility_cull_count = 0;
	i32 influence_cull_count = 0;
	i32 frustum_cull_count = 0;
};

// Culls cached world-space bounds against an optional influence sphere and a
// view-projection frustum. Skinned meshes bypass the bounds tests because
// animated bounds are not yet available.
//
// out_visible receives render_object_index values in scene index order, which
// is also the order the draw loops record in. It is cleared but keeps its
// capacity, so steady-state frames allocate nothing.
void cull_objects(
	State& in_state,
	const HMM_Mat4& in_view_proj,
	f32 in_bounds_padding,
	DynamicArray<i32>& out_visible,
	const BoundingSphere* in_influence_sphere = nullptr)
{
	CPU_TIMING_SCOPE("Cull Objects");

	out_visible.clear();

	const Frustum frustum = frustum_create(in_view_proj);
	const HMM_Vec3 padding = HMM_V3(in_bounds_padding, in_bounds_padding, in_bounds_padding);

	CullStats stats;
	stats.non_renderable_cull_count = in_state.cull_missing_object_count;
	stats.candidate_count = (i32) in_state.cull_entries.length() + stats.non_renderable_cull_count;

	const i32 entry_count = (i32) in_state.cull_entries.length();
	for (i32 entry_index = 0; entry_index < entry_count; ++entry_index)
	{
		const CullEntry& entry = in_state.cull_entries[entry_index];

		if ((entry.flags & CullEntryFlag_Renderable) == 0)
		{
			stats.visibility_cull_count += 1;
			continue;
		}

		// TODO: Compute animated bounds for skinned meshes so they can be
		// frustum culled safely.
		if (entry.flags & CullEntryFlag_Skinned)
		{
			out_visible.add(entry_index);
			in_state.data_oriented.frame.cull_skinned_visible_count += 1;
			continue;
		}

		BoundingBox object_bounding_box = entry.world_bounds;
		if (in_bounds_padding > 0.0f)
		{
			object_bounding_box.min -= padding;
			object_bounding_box.max += padding;
		}

		if (in_influence_sphere != nullptr
			&& bounding_box_outside_sphere(object_bounding_box, *in_influence_sphere))
		{
			stats.influence_cull_count += 1;
			continue;
		}

		if (frustum_cull(frustum, object_bounding_box))
		{
			stats.frustum_cull_count += 1;
			continue;
		}

		out_visible.add(entry_index);
	}

	in_state.data_oriented.frame.cull_calls += 1;
	in_state.data_oriented.frame.cull_candidate_count += stats.candidate_count;
	in_state.data_oriented.frame.cull_visible_count += (i32) out_visible.length();
	in_state.data_oriented.frame.cull_non_renderable_count += stats.non_renderable_cull_count;
	in_state.data_oriented.frame.cull_visibility_count += stats.visibility_cull_count;
	in_state.data_oriented.frame.cull_influence_count += stats.influence_cull_count;
	in_state.data_oriented.frame.cull_frustum_count += stats.frustum_cull_count;
}
