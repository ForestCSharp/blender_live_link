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

// ---------------------------------------------------------------------------
// Content hashing (drives mesh deduplication)
// ---------------------------------------------------------------------------

// Mirrors the engine's 48-byte Vertex without dragging in render_types.h, which
// depends on Vulkan. The hash is templated, so the exact type does not matter -
// only that it is POD and the same size.
struct TestVertex
{
	f32 position[4];
	f32 normal[4];
	f32 texcoord[2];
	f32 _padding[2];
};
static_assert(sizeof(TestVertex) == 48, "must match the engine vertex stride");

static TestVertex make_vertex(f32 in_value)
{
	TestVertex vertex = {};
	vertex.position[0] = in_value;
	vertex.position[1] = in_value + 1.0f;
	vertex.position[2] = in_value + 2.0f;
	vertex.position[3] = 1.0f;
	vertex.normal[2] = 1.0f;
	vertex.texcoord[0] = in_value;
	vertex.texcoord[1] = in_value;
	return vertex;
}

static void test_hash_is_deterministic()
{
	TestVertex vertices[3] = { make_vertex(0.0f), make_vertex(1.0f), make_vertex(2.0f) };
	u32 indices[3] = { 0, 1, 2 };

	const GeometryContentHash first = geometry_arena_content_hash(vertices, 3, indices, 3);
	const GeometryContentHash second = geometry_arena_content_hash(vertices, 3, indices, 3);
	assert(first == second);
	// Zero is the "not shared" sentinel and must never be produced.
	assert(first.is_valid());
}

static void test_hash_separates_identical_from_perturbed()
{
	TestVertex a[3] = { make_vertex(0.0f), make_vertex(1.0f), make_vertex(2.0f) };
	TestVertex b[3] = { make_vertex(0.0f), make_vertex(1.0f), make_vertex(2.0f) };
	u32 indices[3] = { 0, 1, 2 };

	// Byte-identical geometry must share a key, which is what lets 10k copies
	// of one Blender collection instance collapse to a single allocation.
	assert(geometry_arena_content_hash(a, 3, indices, 3)
		== geometry_arena_content_hash(b, 3, indices, 3));

	// A single perturbed float must not.
	b[1].position[0] += 1e-5f;
	assert(geometry_arena_content_hash(a, 3, indices, 3)
		!= geometry_arena_content_hash(b, 3, indices, 3));
}

static void test_hash_covers_indices_and_order()
{
	TestVertex vertices[3] = { make_vertex(0.0f), make_vertex(1.0f), make_vertex(2.0f) };
	u32 indices[3] = { 0, 1, 2 };
	u32 reordered[3] = { 0, 2, 1 };

	// Winding matters, so index order has to be part of the key.
	assert(geometry_arena_content_hash(vertices, 3, indices, 3)
		!= geometry_arena_content_hash(vertices, 3, reordered, 3));
}

// The hash is authoritative for identity now, so both halves have to carry
// signal - a change must not leave either 64-bit half untouched.
static void test_hash_changes_both_halves()
{
	TestVertex a[3] = { make_vertex(0.0f), make_vertex(1.0f), make_vertex(2.0f) };
	TestVertex b[3] = { make_vertex(0.0f), make_vertex(1.0f), make_vertex(2.0f) };
	u32 indices[3] = { 0, 1, 2 };
	b[2].normal[1] += 0.5f;

	const GeometryContentHash first = geometry_arena_content_hash(a, 3, indices, 3);
	const GeometryContentHash second = geometry_arena_content_hash(b, 3, indices, 3);
	assert(first.low != second.low);
	assert(first.high != second.high);
}

// A one-bit input change should move roughly half the output bits. Anything
// close to zero would mean the hash is not safe to trust without a memcmp.
static void test_hash_avalanche()
{
	u32 indices[3] = { 0, 1, 2 };
	i32 total_flipped = 0;
	const i32 sample_count = 64;

	for (i32 sample = 0; sample < sample_count; ++sample)
	{
		TestVertex a[3] = { make_vertex(0.0f), make_vertex(1.0f), make_vertex(2.0f) };
		TestVertex b[3] = { make_vertex(0.0f), make_vertex(1.0f), make_vertex(2.0f) };

		// Flip a single bit in the raw bytes.
		u8* raw = (u8*) b;
		raw[sample] ^= 1u;

		const GeometryContentHash first = geometry_arena_content_hash(a, 3, indices, 3);
		const GeometryContentHash second = geometry_arena_content_hash(b, 3, indices, 3);
		assert(first != second);

		total_flipped += __builtin_popcountll(first.low ^ second.low);
		total_flipped += __builtin_popcountll(first.high ^ second.high);
	}

	const f64 mean_flipped = (f64) total_flipped / (f64) sample_count;
	// Ideal is 64 of 128 bits; allow a generous band around it.
	assert(mean_flipped > 48.0 && mean_flipped < 80.0);
}

static void test_hash_covers_counts()
{
	TestVertex vertices[4] = {
		make_vertex(0.0f), make_vertex(1.0f), make_vertex(2.0f), make_vertex(3.0f) };
	u32 indices[6] = { 0, 1, 2, 0, 2, 3 };

	// A prefix of the same buffer must not collide with the whole thing.
	assert(geometry_arena_content_hash(vertices, 3, indices, 3)
		!= geometry_arena_content_hash(vertices, 4, indices, 6));
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
	test_hash_is_deterministic();
	test_hash_separates_identical_from_perturbed();
	test_hash_covers_indices_and_order();
	test_hash_covers_counts();
	test_hash_changes_both_halves();
	test_hash_avalanche();
	printf("geometry_arena_tests: all passed\n");
	return 0;
}
