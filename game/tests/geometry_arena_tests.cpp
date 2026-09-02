#include <cassert>
#include <cstdio>

#include "render/geometry_arena_freelist.h"

// Exercises the geometry arena's suballocation bookkeeping without Vulkan.
// The arena hands vkCmdDrawIndexed its firstIndex/vertexOffset directly, so an
// off-by-one here renders as garbage geometry rather than failing loudly.

static void test_bump_allocation()
{
	DynamicArray<GeometryFreeBlock> free_list;
	u32 high_water = 0;
	u32 offset = 0;

	assert(geometry_arena_suballocate(free_list, high_water, 100, 10, offset));
	assert(offset == 0 && high_water == 10);

	assert(geometry_arena_suballocate(free_list, high_water, 100, 25, offset));
	assert(offset == 10 && high_water == 35);

	// Zero-sized requests succeed without consuming space.
	assert(geometry_arena_suballocate(free_list, high_water, 100, 0, offset));
	assert(high_water == 35);
}

static void test_capacity_exhaustion()
{
	DynamicArray<GeometryFreeBlock> free_list;
	u32 high_water = 0;
	u32 offset = 0;

	assert(geometry_arena_suballocate(free_list, high_water, 50, 50, offset));
	assert(offset == 0 && high_water == 50);

	// Exactly full: any further request must fail rather than overflow.
	assert(!geometry_arena_suballocate(free_list, high_water, 50, 1, offset));
	assert(high_water == 50);
}

static void test_reuse_before_bump()
{
	DynamicArray<GeometryFreeBlock> free_list;
	u32 high_water = 0;
	u32 offset = 0;

	geometry_arena_suballocate(free_list, high_water, 100, 10, offset);	// [0,10)
	geometry_arena_suballocate(free_list, high_water, 100, 10, offset);	// [10,20)
	geometry_arena_suballocate(free_list, high_water, 100, 10, offset);	// [20,30)
	assert(high_water == 30);

	geometry_arena_release_block(free_list, 10, 10);
	assert(geometry_arena_free_elements(free_list) == 10);

	// A fitting request must come out of the hole, not the high water mark.
	assert(geometry_arena_suballocate(free_list, high_water, 100, 10, offset));
	assert(offset == 10);
	assert(high_water == 30);
	assert(free_list.length() == 0);

	// A request too large for the hole falls through to bump allocation.
	geometry_arena_release_block(free_list, 10, 10);
	assert(geometry_arena_suballocate(free_list, high_water, 100, 20, offset));
	assert(offset == 30 && high_water == 50);
}

static void test_partial_reuse_splits_block()
{
	DynamicArray<GeometryFreeBlock> free_list;
	u32 high_water = 40;
	u32 offset = 0;

	geometry_arena_release_block(free_list, 0, 40);
	assert(geometry_arena_suballocate(free_list, high_water, 100, 15, offset));
	assert(offset == 0);
	// Remainder stays available at the shifted offset.
	assert(free_list.length() == 1);
	assert(free_list[0].offset == 15 && free_list[0].count == 25);
}

static void test_coalescing()
{
	DynamicArray<GeometryFreeBlock> free_list;

	// Release out of order; adjacent blocks must merge into one.
	geometry_arena_release_block(free_list, 20, 10);
	geometry_arena_release_block(free_list, 0, 10);
	assert(free_list.length() == 2);

	geometry_arena_release_block(free_list, 10, 10);
	assert(free_list.length() == 1);
	assert(free_list[0].offset == 0 && free_list[0].count == 30);

	// A non-adjacent block stays separate.
	geometry_arena_release_block(free_list, 40, 5);
	assert(free_list.length() == 2);
	assert(geometry_arena_free_elements(free_list) == 35);

	// Filling the gap merges everything back together.
	geometry_arena_release_block(free_list, 30, 10);
	assert(free_list.length() == 1);
	assert(free_list[0].offset == 0 && free_list[0].count == 45);
}

static void test_release_is_lossless()
{
	DynamicArray<GeometryFreeBlock> free_list;
	u32 high_water = 0;
	u32 offset = 0;

	// Allocate a run, release all of it, and confirm nothing is lost or double
	// counted - a leak here silently shrinks the arena over a live-link session.
	u32 offsets[8] = {};
	for (i32 i = 0; i < 8; ++i)
	{
		assert(geometry_arena_suballocate(free_list, high_water, 1000, 7, offset));
		offsets[i] = offset;
	}
	assert(high_water == 56);

	for (i32 i = 7; i >= 0; --i)
	{
		geometry_arena_release_block(free_list, offsets[i], 7);
	}
	assert(free_list.length() == 1);
	assert(free_list[0].offset == 0 && free_list[0].count == 56);
	assert(geometry_arena_free_elements(free_list) == 56);
}

static void test_zero_sized_release_is_noop()
{
	DynamicArray<GeometryFreeBlock> free_list;
	geometry_arena_release_block(free_list, 100, 0);
	assert(free_list.length() == 0);
}

int main()
{
	test_bump_allocation();
	test_capacity_exhaustion();
	test_reuse_before_bump();
	test_partial_reuse_splits_block();
	test_coalescing();
	test_release_is_lossless();
	test_zero_sized_release_is_noop();
	printf("geometry_arena_tests: all passed\n");
	return 0;
}
