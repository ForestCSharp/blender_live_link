#pragma once

#include <algorithm>

#include "core/types.h"
#include "core/dynamic_array.h"

// Suballocation bookkeeping for the shared geometry arena, kept free of Vulkan
// so it can be unit tested directly (tests/geometry_arena_tests.cpp).
//
// All offsets and counts are in *elements*, never bytes, so the values handed to
// vkCmdDrawIndexed fall out directly and alignment needs no special handling.

struct GeometryFreeBlock
{
	u32 offset;
	u32 count;
};

// First-fit over the free list, falling back to bump allocation at the high
// water mark. Returns false when neither can satisfy the request.
inline bool geometry_arena_suballocate(
	DynamicArray<GeometryFreeBlock>& in_out_free_list,
	u32& in_out_high_water,
	u32 in_capacity,
	u32 in_count,
	u32& out_offset)
{
	if (in_count == 0)
	{
		out_offset = 0;
		return true;
	}

	for (size_t block_index = 0; block_index < in_out_free_list.length(); ++block_index)
	{
		GeometryFreeBlock& block = in_out_free_list[block_index];
		if (block.count < in_count)
		{
			continue;
		}

		out_offset = block.offset;
		block.offset += in_count;
		block.count -= in_count;
		if (block.count == 0)
		{
			// Swap-erase; ordering is restored by the coalesce pass on release.
			in_out_free_list[block_index] = in_out_free_list.last();
			in_out_free_list.pop();
		}
		return true;
	}

	if (in_out_high_water + in_count > in_capacity)
	{
		return false;
	}

	out_offset = in_out_high_water;
	in_out_high_water += in_count;
	return true;
}

// Returns a block and merges it with any neighbours. Only runs on live-link
// deletes, so an O(n log n) sort per call is not worth avoiding.
inline void geometry_arena_release_block(
	DynamicArray<GeometryFreeBlock>& in_out_free_list,
	u32 in_offset,
	u32 in_count)
{
	if (in_count == 0)
	{
		return;
	}

	in_out_free_list.add({ .offset = in_offset, .count = in_count });

	std::sort(
		&in_out_free_list[0],
		&in_out_free_list[0] + in_out_free_list.length(),
		[](const GeometryFreeBlock& a, const GeometryFreeBlock& b)
		{
			return a.offset < b.offset;
		});

	size_t write_index = 0;
	for (size_t read_index = 1; read_index < in_out_free_list.length(); ++read_index)
	{
		GeometryFreeBlock& current = in_out_free_list[write_index];
		const GeometryFreeBlock& next = in_out_free_list[read_index];
		if (current.offset + current.count == next.offset)
		{
			current.count += next.count;
			continue;
		}
		++write_index;
		in_out_free_list[write_index] = next;
	}

	while (in_out_free_list.length() > write_index + 1)
	{
		in_out_free_list.pop();
	}
}

inline u64 geometry_arena_free_elements(DynamicArray<GeometryFreeBlock>& in_free_list)
{
	u64 total = 0;
	for (size_t block_index = 0; block_index < in_free_list.length(); ++block_index)
	{
		total += in_free_list[block_index].count;
	}
	return total;
}
