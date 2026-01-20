#pragma once

// Ankerl's Segmented Vector and Fast Unordered Hash Math
#include "ankerl/unordered_dense.h"
using ankerl::unordered_dense::map;

#include "core/types.h"
#include "game_object/game_object.h"

#define WITH_VERBOSE_CULL_RESULTS 1

struct CullResult
{
	// Pointers to objects remaining after culling
	map<i32, Object*> objects;

	// Number of objects this cull result rejected
	i32 cull_count = 0;

	#if WITH_VERBOSE_CULL_RESULTS
	i32 non_renderable_cull_count = 0;
	i32 visibility_cull_count = 0;
	i32 frustum_cull_count = 0;
	#endif
};

CullResult cull_objects(map<i32, Object>& in_objects, const HMM_Mat4& in_view_proj)
{
	CullResult out_cull_result = {
		.cull_count = 0,
	};

	Frustum frustum = frustum_create(in_view_proj);

	for (auto& [unique_id, object] : in_objects)
	{
		// Cull non-renderable objects
		if (!object.has_mesh)
		{
			out_cull_result.cull_count += 1;
			#if WITH_VERBOSE_CULL_RESULTS
			out_cull_result.non_renderable_cull_count += 1;
	  		#endif
			continue;
		}

		// Cull invisible objects
		if (!object.visibility)
		{
			out_cull_result.cull_count += 1;
			#if WITH_VERBOSE_CULL_RESULTS
			out_cull_result.visibility_cull_count += 1;
	  		#endif
			continue;
		}

		// Frustum Cull
		BoundingBox object_bounding_box = object_get_bounding_box(object);
		if (frustum_cull(frustum, object_bounding_box))
		{
			out_cull_result.cull_count += 1;
			#if WITH_VERBOSE_CULL_RESULTS
			out_cull_result.frustum_cull_count += 1;
	  		#endif
			continue;
		}

		out_cull_result.objects[unique_id] = &object;
	}

	return out_cull_result;
}
