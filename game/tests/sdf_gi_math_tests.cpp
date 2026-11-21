#include <cassert>
#include <cstdio>
#include <initializer_list>

#include "render/passes/sdf_gi/sdf_gi_math.h"

// A level that doesn't contain the camera (with margin) leaves the screen
// probes nearest the camera without a distance field to trace.

using namespace SdfGi;

static bool near(f32 a, f32 b) { return fabsf(a - b) < 1e-4f; }

int main()
{
	const f32 voxel = 0.25f;
	const f32 extent = voxel * LEVEL_RES;
	const f32 snap = voxel * SNAP_VOXELS;

	// The camera sits well inside its level, at least half the level minus one
	// snap step from every face, for positive and negative coordinates.
	for (f32 x : { -1000.3f, -2.0f, -0.01f, 0.0f, 0.7f, 5.99f, 123.456f })
	{
		const HMM_Vec3 camera = HMM_V3(x, -x * 0.5f, x * 2.0f);
		const HMM_Vec3 origin = level_target_origin(camera, voxel);
		for (i32 axis = 0; axis < 3; ++axis)
		{
			const f32 local = camera.Elements[axis] - origin.Elements[axis];
			assert(local >= extent * 0.5f - 1e-3f);
			assert(local <= extent * 0.5f + snap + 1e-3f);
			// Origins land on the snap grid.
			const f32 steps = origin.Elements[axis] / snap;
			assert(near(steps, roundf(steps)));
		}
	}

	// Moving less than one snap step inside a cell keeps the origin (no rebuild).
	const HMM_Vec3 a = level_target_origin(HMM_V3(0.1f, 0.1f, 0.1f), voxel);
	const HMM_Vec3 b = level_target_origin(HMM_V3(0.1f + snap * 0.5f, 0.1f, 0.1f), voxel);
	assert(near(a.X, b.X) && near(a.Y, b.Y) && near(a.Z, b.Z));
	const HMM_Vec3 c = level_target_origin(HMM_V3(0.1f + snap, 0.1f, 0.1f), voxel);
	assert(near(c.X - a.X, snap));

	// Edit regions: inside, touching a face and outside the level.
	const HMM_Vec3 origin = HMM_V3(0, 0, 0);
	assert(level_overlaps(origin, voxel, { HMM_V3(1, 1, 1), HMM_V3(2, 2, 2) }));
	assert(level_overlaps(origin, voxel, { HMM_V3(extent, 1, 1), HMM_V3(extent + 5, 2, 2) }));
	assert(!level_overlaps(origin, voxel, { HMM_V3(extent + 0.1f, 1, 1), HMM_V3(extent + 5, 2, 2) }));
	assert(!level_overlaps(origin, voxel, { HMM_V3(1, -5, 1), HMM_V3(2, -0.1f, 2) }));

	printf("SDF_GI_MATH_TESTS_PASSED\n");
	return 0;
}
