#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "core/types.h"

BoundingBox make_bounds(HMM_Vec3 in_min, HMM_Vec3 in_max)
{
	return (BoundingBox) { .min = in_min, .max = in_max };
}

void test_fully_outside()
{
	const BoundingSphere sphere = { .center = HMM_V3(0.0f, 0.0f, 0.0f), .radius = 1.0f };
	const BoundingBox bounds = make_bounds(
		HMM_V3(2.0f, -0.25f, -0.25f), HMM_V3(3.0f, 0.25f, 0.25f));
	assert(bounding_box_outside_sphere(bounds, sphere));
}

void test_intersecting()
{
	const BoundingSphere sphere = { .center = HMM_V3(0.0f, 0.0f, 0.0f), .radius = 1.0f };
	const BoundingBox bounds = make_bounds(
		HMM_V3(0.5f, -0.25f, -0.25f), HMM_V3(1.5f, 0.25f, 0.25f));
	assert(!bounding_box_outside_sphere(bounds, sphere));
}

void test_tangent()
{
	const BoundingSphere sphere = { .center = HMM_V3(0.0f, 0.0f, 0.0f), .radius = 1.0f };
	const BoundingBox bounds = make_bounds(
		HMM_V3(1.0f, -0.25f, -0.25f), HMM_V3(2.0f, 0.25f, 0.25f));
	assert(!bounding_box_outside_sphere(bounds, sphere));
}

void test_box_inside_sphere()
{
	const BoundingSphere sphere = { .center = HMM_V3(0.0f, 0.0f, 0.0f), .radius = 2.0f };
	const BoundingBox bounds = make_bounds(
		HMM_V3(-0.5f, -0.5f, -0.5f), HMM_V3(0.5f, 0.5f, 0.5f));
	assert(!bounding_box_outside_sphere(bounds, sphere));
}

void test_sphere_inside_box()
{
	const BoundingSphere sphere = { .center = HMM_V3(0.0f, 0.0f, 0.0f), .radius = 0.25f };
	const BoundingBox bounds = make_bounds(
		HMM_V3(-2.0f, -2.0f, -2.0f), HMM_V3(2.0f, 2.0f, 2.0f));
	assert(!bounding_box_outside_sphere(bounds, sphere));
}

int main()
{
	test_fully_outside();
	test_intersecting();
	test_tangent();
	test_box_inside_sphere();
	test_sphere_inside_box();
	std::puts("probe influence culling tests passed");
	return 0;
}
