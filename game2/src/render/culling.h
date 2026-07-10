#pragma once

#include "core/types.h"
#include "core/stretchy_buffer.h"

// CPU frustum culling (port of game/src/render/culling.h:24-104).
// Frustum/frustum_create/frustum_cull live in core/types.h.

struct CullResult
{
	StretchyBuffer<i32> object_ids;
	i32 frustum_cull_count = 0;
};

// Culls scene mesh objects against a view-projection frustum. Skinned meshes
// bypass the frustum test (no animated bounds yet — game/ parity TODO).
CullResult cull_objects(State& in_state, const HMM_Mat4& in_view_proj, f32 in_bounds_padding)
{
	CullResult out_cull_result;

	Frustum frustum = frustum_create(in_view_proj);

	scene_ensure_indexes(in_state);
	for (i32 mesh_object_id : in_state.scene.indexes.mesh_object_ids)
	{
		auto found = in_state.scene.objects.find(mesh_object_id);
		if (found == in_state.scene.objects.end())
		{
			continue;
		}

		Object& object = found->second;
		if (!object.visibility || !object.has_mesh)
		{
			continue;
		}

		// TODO: Compute animated bounds for skinned meshes so they can be
		// frustum culled safely (game/ parity)
		if (object.mesh.has_skinned_vertices)
		{
			out_cull_result.object_ids.add(mesh_object_id);
			continue;
		}

		BoundingBox object_bounding_box = bounding_box_transform(object.mesh.bounding_box, object.current_transform);
		if (in_bounds_padding > 0.0f)
		{
			const HMM_Vec3 padding = HMM_V3(in_bounds_padding, in_bounds_padding, in_bounds_padding);
			object_bounding_box.min -= padding;
			object_bounding_box.max += padding;
		}

		if (frustum_cull(frustum, object_bounding_box))
		{
			out_cull_result.frustum_cull_count += 1;
			continue;
		}

		out_cull_result.object_ids.add(mesh_object_id);
	}

	return out_cull_result;
}
