#pragma once

#include <cassert>
#include <cstring>

#include "ankerl/unordered_dense.h"
#include "core/types.h"
#include "core/dynamic_array.h"
#include "render/geometry_arena_freelist.h"
#include "render/gpu_buffer.h"
#include "render/render_types.h"

// Shared vertex/index storage.
//
// Every mesh used to own a dedicated VkBuffer pair, so firstIndex/vertexOffset
// were always 0 and every draw had to rebind both buffers. Packing static
// geometry into one buffer pair lets a pass bind once and address each mesh
// through its slice offsets instead.
//
// Scope is deliberately narrow: static, non-tessellated meshes only. Skinned
// meshes need a second vertex binding, and tessellated geometry is resized
// every frame from GPU-planned patch counts, which is the opposite of what an
// arena assumes. Both stay on the legacy per-mesh path.
//
// Sizes are tracked in *elements*, never bytes, so the values handed to
// vkCmdDrawIndexed fall out directly and alignment needs no special handling.

struct MeshArenaSlice
{
	u32 vertex_offset = 0;	// element index -> vkCmdDrawIndexed vertexOffset
	u32 vertex_count = 0;
	u32 first_index = 0;	// element index -> vkCmdDrawIndexed firstIndex
	u32 index_count = 0;
	// Key into the arena's dedup registry. Zero means this slice is not shared
	// and its blocks are freed directly on release.
	GeometryContentHash content_hash;
	bool valid = false;
};

// One arena allocation shared by every mesh whose geometry hashes and compares
// equal to it.
struct GeometrySharedEntry
{
	MeshArenaSlice slice;
	i32 ref_count = 0;
};

// The registry is keyed on the low 64 bits, so a bucket hit still has to match
// the full 128-bit value before anything is shared.

struct GeometryArena
{
	GpuBuffer<Vertex> vertex_buffer;
	GpuBuffer<u32> index_buffer;

	u32 vertex_capacity = 0;
	u32 index_capacity = 0;
	u32 vertex_high_water = 0;
	u32 index_high_water = 0;

	DynamicArray<GeometryFreeBlock> vertex_free;
	DynamicArray<GeometryFreeBlock> index_free;

	// Bumped whenever the backing buffers are reallocated, so callers can tell
	// that every previously handed-out slice is now stale.
	u64 generation = 0;

	// Identical geometry shares one allocation. Blender collection instances
	// arrive pre-flattened into independent per-placement objects, so a scene
	// with 500 placements of one collection would otherwise get 500 byte-identical
	// copies of the same vertex data.
	ankerl::unordered_dense::map<u64, GeometrySharedEntry> shared_by_hash;

	// Stats surfaced in the debug UI.
	u32 live_slice_count = 0;
	u32 dedup_hit_count = 0;
	u64 wasted_vertex_elements = 0;
	u64 wasted_index_elements = 0;
	u32 grow_count = 0;
};

inline GeometryArena g_geometry_arena;

static constexpr u32 GEOMETRY_ARENA_INITIAL_VERTICES = 256u * 1024u;
static constexpr u32 GEOMETRY_ARENA_INITIAL_INDICES = 768u * 1024u;

// ---------------------------------------------------------------------------
// Backing storage
// ---------------------------------------------------------------------------

inline void geometry_arena_create_buffers(u32 in_vertex_capacity, u32 in_index_capacity)
{
	GeometryArena& arena = g_geometry_arena;

	arena.vertex_buffer.destroy_gpu_buffer();
	arena.index_buffer.destroy_gpu_buffer();

	// data == nullptr, so get_gpu_buffer() allocates without uploading; content
	// arrives later through write_range.
	arena.vertex_buffer = GpuBuffer<Vertex>({
		.data = nullptr,
		.size = (u64) in_vertex_capacity * sizeof(Vertex),
		.usage = { .vertex_buffer = true, .storage_buffer = true },
		.label = "Geometry Arena Vertices",
	});
	arena.index_buffer = GpuBuffer<u32>({
		.data = nullptr,
		.size = (u64) in_index_capacity * sizeof(u32),
		.usage = { .index_buffer = true, .storage_buffer = true },
		.label = "Geometry Arena Indices",
	});

	arena.vertex_capacity = in_vertex_capacity;
	arena.index_capacity = in_index_capacity;
	arena.vertex_high_water = 0;
	arena.index_high_water = 0;
	arena.vertex_free.clear();
	arena.index_free.clear();
	// Every slice handed out before this point is stale, so the dedup registry
	// that points at them has to go too.
	arena.shared_by_hash.clear();
	arena.live_slice_count = 0;
	arena.dedup_hit_count = 0;
	arena.generation = gpu_next_resource_generation();
}

inline bool geometry_arena_is_ready()
{
	return g_geometry_arena.vertex_capacity > 0;
}

// ---------------------------------------------------------------------------
// Deduplication
// ---------------------------------------------------------------------------

// Debug-only cross-check that a hash hit really is the same geometry.
//
// This is no longer collision protection: at 128 bits a collision is far less
// likely than an undetected memory fault. What it still catches is a bug in the
// hashing itself - a wrong byte length, an uncovered struct field, or Vertex
// padding that stops being zero-initialized. Those would silently render the
// wrong geometry, so they are worth an assert in debug builds.
//
// Returns true when the arena is device-local and therefore unreadable; the
// hash is authoritative either way, which is what lets deduplication work on
// platforms where the arena does not live in host-visible memory.
inline bool geometry_arena_slice_matches(
	const MeshArenaSlice& in_slice,
	const Vertex* in_vertices, u32 in_vertex_count,
	const u32* in_indices, u32 in_index_count)
{
	if (in_slice.vertex_count != in_vertex_count || in_slice.index_count != in_index_count)
	{
		return false;
	}

	const Vertex* arena_vertices = g_geometry_arena.vertex_buffer.mapped_elements();
	const u32* arena_indices = g_geometry_arena.index_buffer.mapped_elements();
	if (arena_vertices == nullptr || arena_indices == nullptr)
	{
		return true;
	}

	return memcmp(arena_vertices + in_slice.vertex_offset, in_vertices,
			(size_t) in_vertex_count * sizeof(Vertex)) == 0
		&& memcmp(arena_indices + in_slice.first_index, in_indices,
			(size_t) in_index_count * sizeof(u32)) == 0;
}

// Places one mesh's geometry in the arena. Returns false when the arena is out
// of room; the caller is expected to grow and retry.
inline bool geometry_arena_acquire(
	const Vertex* in_vertices,
	u32 in_vertex_count,
	const u32* in_indices,
	u32 in_index_count,
	MeshArenaSlice& out_slice)
{
	GeometryArena& arena = g_geometry_arena;
	if (in_vertices == nullptr || in_indices == nullptr || in_vertex_count == 0 || in_index_count == 0)
	{
		return false;
	}

	// Share an existing allocation when this geometry is already resident.
	GeometryContentHash content_hash = geometry_arena_content_hash(
		in_vertices, in_vertex_count, in_indices, in_index_count);
	auto shared = arena.shared_by_hash.find(content_hash.low);
	if (shared != arena.shared_by_hash.end())
	{
		if (shared->second.slice.content_hash == content_hash)
		{
			assert(geometry_arena_slice_matches(
				shared->second.slice, in_vertices, in_vertex_count, in_indices, in_index_count)
				&& "geometry content hash matched but the bytes differ - hashing bug");

			shared->second.ref_count += 1;
			out_slice = shared->second.slice;
			arena.dedup_hit_count += 1;
			arena.live_slice_count += 1;
			return true;
		}

		// Only the low 64 bits collided. Give this mesh its own allocation
		// rather than evicting the entry that already owns the bucket.
		content_hash = {};
	}

	u32 vertex_offset = 0;
	if (!geometry_arena_suballocate(
		arena.vertex_free, arena.vertex_high_water, arena.vertex_capacity, in_vertex_count, vertex_offset))
	{
		return false;
	}

	u32 first_index = 0;
	if (!geometry_arena_suballocate(
		arena.index_free, arena.index_high_water, arena.index_capacity, in_index_count, first_index))
	{
		// Hand the vertex range back so a failed acquire leaks nothing.
		geometry_arena_release_block(arena.vertex_free, vertex_offset, in_vertex_count);
		return false;
	}

	arena.vertex_buffer.write_range(vertex_offset, in_vertices, in_vertex_count);
	arena.index_buffer.write_range(first_index, in_indices, in_index_count);

	out_slice = {
		.vertex_offset = vertex_offset,
		.vertex_count = in_vertex_count,
		.first_index = first_index,
		.index_count = in_index_count,
		.content_hash = content_hash,
		.valid = true,
	};
	arena.live_slice_count += 1;

	if (content_hash.is_valid())
	{
		arena.shared_by_hash.insert({ content_hash.low, GeometrySharedEntry{ out_slice, 1 } });
	}
	return true;
}

inline void geometry_arena_release(MeshArenaSlice& in_out_slice)
{
	if (!in_out_slice.valid)
	{
		return;
	}

	GeometryArena& arena = g_geometry_arena;
	if (arena.live_slice_count > 0)
	{
		arena.live_slice_count -= 1;
	}

	// Shared geometry only returns its blocks once the last referencing mesh
	// lets go of it.
	if (in_out_slice.content_hash.is_valid())
	{
		auto shared = arena.shared_by_hash.find(in_out_slice.content_hash.low);
		if (shared != arena.shared_by_hash.end()
			&& shared->second.slice.content_hash == in_out_slice.content_hash)
		{
			shared->second.ref_count -= 1;
			if (shared->second.ref_count > 0)
			{
				in_out_slice = {};
				return;
			}
			arena.shared_by_hash.erase(shared);
		}
	}

	geometry_arena_release_block(arena.vertex_free, in_out_slice.vertex_offset, in_out_slice.vertex_count);
	geometry_arena_release_block(arena.index_free, in_out_slice.first_index, in_out_slice.index_count);
	in_out_slice = {};
}

inline void geometry_arena_update_waste_stats()
{
	GeometryArena& arena = g_geometry_arena;
	arena.wasted_vertex_elements = geometry_arena_free_elements(arena.vertex_free);
	arena.wasted_index_elements = geometry_arena_free_elements(arena.index_free);
}

inline void geometry_arena_shutdown()
{
	GeometryArena& arena = g_geometry_arena;
	arena.vertex_buffer.destroy_gpu_buffer();
	arena.index_buffer.destroy_gpu_buffer();
	arena.vertex_free.reset();
	arena.index_free.reset();
	arena.shared_by_hash.clear();
	arena.vertex_capacity = 0;
	arena.index_capacity = 0;
	arena.vertex_high_water = 0;
	arena.index_high_water = 0;
	arena.live_slice_count = 0;
}
