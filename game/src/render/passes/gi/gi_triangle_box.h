#pragma once

#include <algorithm>
#include <cmath>

#include "core/types.h"

// Triangle vs axis-aligned box overlap (Akenine-Möller separating axis test;
// tests/gi_triangle_box_tests.cpp). Candidate axes: the 3 box axes, the 9
// cross products of box axes with triangle edges, and the triangle normal.
// Touching counts as overlapping.
inline bool gi_triangle_box_overlap(
	const HMM_Vec3 in_box_center, const HMM_Vec3 in_box_half_extent,
	const HMM_Vec3 in_a, const HMM_Vec3 in_b, const HMM_Vec3 in_c)
{
	const HMM_Vec3 v[3] = { in_a - in_box_center, in_b - in_box_center, in_c - in_box_center };
	const HMM_Vec3 h = in_box_half_extent;

	// Projects the triangle onto in_axis and compares with the box's projected radius.
	const auto separated = [&](const HMM_Vec3 in_axis)
	{
		const f32 p0 = HMM_DotV3(in_axis, v[0]);
		const f32 p1 = HMM_DotV3(in_axis, v[1]);
		const f32 p2 = HMM_DotV3(in_axis, v[2]);
		const f32 radius = h.X * fabsf(in_axis.X) + h.Y * fabsf(in_axis.Y) + h.Z * fabsf(in_axis.Z);
		return std::min({ p0, p1, p2 }) > radius || std::max({ p0, p1, p2 }) < -radius;
	};

	// Box axes first: this is the cheap bounds-vs-bounds rejection.
	for (i32 axis = 0; axis < 3; ++axis)
	{
		HMM_Vec3 unit = {};
		unit.Elements[axis] = 1.0f;
		if (separated(unit)) return false;
	}

	const HMM_Vec3 edges[3] = { v[1] - v[0], v[2] - v[1], v[0] - v[2] };
	for (i32 axis = 0; axis < 3; ++axis)
	{
		HMM_Vec3 unit = {};
		unit.Elements[axis] = 1.0f;
		for (const HMM_Vec3& edge : edges)
		{
			if (separated(HMM_Cross(unit, edge))) return false;
		}
	}

	return !separated(HMM_Cross(edges[0], edges[1]));
}
