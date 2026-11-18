#include <cassert>
#include <cstdio>

#include "render/passes/gi/gi_triangle_box.h"

// The GI octree only subdivides cells this test says a triangle touches, so a
// false negative leaves a hole in the probe grid on a real surface.

static bool overlaps(HMM_Vec3 in_a, HMM_Vec3 in_b, HMM_Vec3 in_c)
{
	// Unit box: [-1, 1]^3.
	return gi_triangle_box_overlap(HMM_V3(0, 0, 0), HMM_V3(1, 1, 1), in_a, in_b, in_c);
}

int main()
{
	// Fully inside.
	assert(overlaps(HMM_V3(-0.5f, -0.5f, 0), HMM_V3(0.5f, -0.5f, 0), HMM_V3(0, 0.5f, 0)));

	// Fully outside along each axis, both sides.
	for (i32 axis = 0; axis < 3; ++axis)
	{
		for (f32 side : { -1.0f, 1.0f })
		{
			HMM_Vec3 offset = {};
			offset.Elements[axis] = side * 3.0f;
			assert(!overlaps(HMM_V3(-0.5f, -0.5f, 0) + offset, HMM_V3(0.5f, -0.5f, 0) + offset, HMM_V3(0, 0.5f, 0) + offset));
		}
	}

	// Bounds overlap the box, but the triangle passes diagonally past a corner:
	// only an edge cross-product axis separates it.
	assert(!overlaps(HMM_V3(1.5f, 0.6f, -5), HMM_V3(0.6f, 1.5f, -5), HMM_V3(1.05f, 1.05f, 5)));

	// Bounds overlap on every axis, but the plane x+y+z = 3.1 passes just
	// beyond the box corner (1,1,1) (sum 3); at 2.9 it cuts the corner.
	assert(!overlaps(HMM_V3(13.1f, -5, -5), HMM_V3(-5, 13.1f, -5), HMM_V3(-5, -5, 13.1f)));
	assert(overlaps(HMM_V3(12.9f, -5, -5), HMM_V3(-5, 12.9f, -5), HMM_V3(-5, -5, 12.9f)));

	// Touching a face counts.
	assert(overlaps(HMM_V3(1, -0.5f, -0.5f), HMM_V3(1, 0.5f, -0.5f), HMM_V3(1, 0, 0.5f)));

	// A large triangle slicing through the box with no vertex inside it.
	assert(overlaps(HMM_V3(-10, -10, 0), HMM_V3(10, -10, 0), HMM_V3(0, 10, 0)));
	assert(overlaps(HMM_V3(-10, 0, -10), HMM_V3(10, 0, -10), HMM_V3(0, 0, 10)));

	// Degenerate (zero-area) triangle inside the box still counts.
	assert(overlaps(HMM_V3(0, 0, 0), HMM_V3(0.5f, 0, 0), HMM_V3(0.25f, 0, 0)));

	puts("GI_TRIANGLE_BOX_TESTS_PASSED");
	return 0;
}
