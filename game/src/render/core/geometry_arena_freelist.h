#pragma once

#include <algorithm>

#include <cstring>

#include "core/types.h"
#include "core/dynamic_array.h"

// Suballocation and content-hashing logic for the shared geometry arena, kept
// free of Vulkan so it can be unit tested directly
// (tests/geometry_arena_tests.cpp).
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

// MurmurHash3 x64_128, used as the geometry content key.
//
// The width is what matters here: at a million meshes the 128-bit birthday
// bound is ~1e-27, far below the odds of an undetected memory fault, so the
// hash can be treated as authoritative identity. A 64-bit hash could not be,
// and FNV-1a in particular is a multiplicative hash with weak avalanche in the
// high bits - a poor fit for mesh data, which is full of repeated float
// patterns, zeroed padding and near-sequential indices.

struct GeometryContentHash
{
	u64 low = 0;
	u64 high = 0;

	bool operator==(const GeometryContentHash& in_other) const
	{
		return low == in_other.low && high == in_other.high;
	}
	bool operator!=(const GeometryContentHash& in_other) const { return !(*this == in_other); }

	// Zero is reserved to mean "not shared".
	bool is_valid() const { return low != 0 || high != 0; }
};

inline u64 geometry_hash_rotl64(u64 in_value, i32 in_bits)
{
	return (in_value << in_bits) | (in_value >> (64 - in_bits));
}

inline u64 geometry_hash_fmix64(u64 in_value)
{
	in_value ^= in_value >> 33;
	in_value *= 0xff51afd7ed558ccdull;
	in_value ^= in_value >> 33;
	in_value *= 0xc4ceb9fe1a85ec53ull;
	in_value ^= in_value >> 33;
	return in_value;
}

inline GeometryContentHash geometry_arena_hash_bytes(
	const void* in_data, u64 in_byte_count, u64 in_seed)
{
	const u8* data = (const u8*) in_data;
	const u64 block_count = in_byte_count / 16;

	u64 h1 = in_seed;
	u64 h2 = in_seed;
	const u64 c1 = 0x87c37b91114253d5ull;
	const u64 c2 = 0x4cf5ad432745937full;

	for (u64 block_index = 0; block_index < block_count; ++block_index)
	{
		u64 k1 = 0;
		u64 k2 = 0;
		memcpy(&k1, data + block_index * 16, sizeof(u64));
		memcpy(&k2, data + block_index * 16 + 8, sizeof(u64));

		k1 *= c1; k1 = geometry_hash_rotl64(k1, 31); k1 *= c2; h1 ^= k1;
		h1 = geometry_hash_rotl64(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52dce729;
		k2 *= c2; k2 = geometry_hash_rotl64(k2, 33); k2 *= c1; h2 ^= k2;
		h2 = geometry_hash_rotl64(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495ab5;
	}

	const u8* tail = data + block_count * 16;
	u64 k1 = 0;
	u64 k2 = 0;
	switch (in_byte_count & 15)
	{
		case 15: k2 ^= ((u64) tail[14]) << 48; [[fallthrough]];
		case 14: k2 ^= ((u64) tail[13]) << 40; [[fallthrough]];
		case 13: k2 ^= ((u64) tail[12]) << 32; [[fallthrough]];
		case 12: k2 ^= ((u64) tail[11]) << 24; [[fallthrough]];
		case 11: k2 ^= ((u64) tail[10]) << 16; [[fallthrough]];
		case 10: k2 ^= ((u64) tail[9]) << 8;   [[fallthrough]];
		case  9: k2 ^= ((u64) tail[8]) << 0;
			k2 *= c2; k2 = geometry_hash_rotl64(k2, 33); k2 *= c1; h2 ^= k2;
			[[fallthrough]];
		case  8: k1 ^= ((u64) tail[7]) << 56; [[fallthrough]];
		case  7: k1 ^= ((u64) tail[6]) << 48; [[fallthrough]];
		case  6: k1 ^= ((u64) tail[5]) << 40; [[fallthrough]];
		case  5: k1 ^= ((u64) tail[4]) << 32; [[fallthrough]];
		case  4: k1 ^= ((u64) tail[3]) << 24; [[fallthrough]];
		case  3: k1 ^= ((u64) tail[2]) << 16; [[fallthrough]];
		case  2: k1 ^= ((u64) tail[1]) << 8;  [[fallthrough]];
		case  1: k1 ^= ((u64) tail[0]) << 0;
			k1 *= c1; k1 = geometry_hash_rotl64(k1, 31); k1 *= c2; h1 ^= k1;
			break;
		default: break;
	}

	h1 ^= in_byte_count;
	h2 ^= in_byte_count;
	h1 += h2;
	h2 += h1;
	h1 = geometry_hash_fmix64(h1);
	h2 = geometry_hash_fmix64(h2);
	h1 += h2;
	h2 += h1;

	return GeometryContentHash{ h1, h2 };
}

// Content key for a mesh's static geometry. Deliberately covers only geometry:
// the transform is per-object and already lives in ObjectData, so two objects
// with the same mesh at different positions still share one allocation.
//
// Vertices and indices are hashed separately and then combined with the counts,
// a hash-of-hashes rather than a streaming pass, because the two arrays are
// separate allocations and Murmur consumes 16-byte blocks.
//
// Templated on the vertex type so this header stays free of the Vulkan-dependent
// render_types.h and can be unit tested on its own.
template<typename VertexType>
inline GeometryContentHash geometry_arena_content_hash(
	const VertexType* in_vertices, u32 in_vertex_count,
	const u32* in_indices, u32 in_index_count)
{
	const GeometryContentHash vertex_hash = geometry_arena_hash_bytes(
		in_vertices, (u64) in_vertex_count * sizeof(VertexType), 0x9e3779b97f4a7c15ull);
	const GeometryContentHash index_hash = geometry_arena_hash_bytes(
		in_indices, (u64) in_index_count * sizeof(u32), 0xbf58476d1ce4e5b9ull);

	const u64 combined[6] = {
		vertex_hash.low, vertex_hash.high,
		index_hash.low, index_hash.high,
		in_vertex_count, in_index_count,
	};
	GeometryContentHash hash = geometry_arena_hash_bytes(
		combined, sizeof(combined), 0x94d049bb133111ebull);

	// Never collide with the "not shared" sentinel.
	if (!hash.is_valid())
	{
		hash.low = 1;
	}
	return hash;
}
