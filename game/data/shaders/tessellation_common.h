#pragma once

#include "shader_common.h"

#if defined(__cplusplus) && defined(__STDC__)
#include "core/types.h"
#endif

#if !defined(__cplusplus) || !defined(__STDC__)
#define TESS_SHADER 1
#define TESS_U32 uint
#define TESS_F32 float
#define TESS_INIT(value)
#else
#define TESS_SHADER 0
#define TESS_U32 u32
#define TESS_F32 f32
#define TESS_INIT(value) = value
#endif

#if TESS_SHADER
@block tessellation_types

struct TessellationVertex
{
	vec4 position;
	vec4 normal;
	vec2 texcoord;
	float padding0;
	float padding1;
};
#endif

/**
 * TessellationPatch represents a single patch of tessellated geometry that will be emitted by the GPU.
 * Each patch corresponds to a single triangle in the source mesh, and contains metadata about where to read source data and write generated data.
 */
struct TessellationPatch
{
	TESS_U32 source_index_offset TESS_INIT(0);
	TESS_U32 source_triangle_index TESS_INIT(0);
	TESS_U32 generated_vertex_offset TESS_INIT(0);
	TESS_U32 generated_index_offset TESS_INIT(0);
	TESS_U32 tess_factor TESS_INIT(1);
	TESS_U32 vertex_count TESS_INIT(0);
	TESS_U32 index_count TESS_INIT(0);
	TESS_U32 padding0 TESS_INIT(0);
	TESS_F32 edge_lod0 TESS_INIT(1.0f);
	TESS_F32 edge_lod1 TESS_INIT(1.0f);
	TESS_F32 edge_lod2 TESS_INIT(1.0f);
	TESS_F32 padding1 TESS_INIT(0.0f);
	TESS_F32 domain_u0 TESS_INIT(0.0f);
	TESS_F32 domain_v0 TESS_INIT(0.0f);
	TESS_F32 domain_u1 TESS_INIT(1.0f);
	TESS_F32 domain_v1 TESS_INIT(0.0f);
	TESS_F32 domain_u2 TESS_INIT(0.0f);
	TESS_F32 domain_v2 TESS_INIT(1.0f);
	TESS_F32 padding2 TESS_INIT(0.0f);
	TESS_F32 padding3 TESS_INIT(0.0f);
};

/**
 * TessellationWeldPair represents a pair of vertices that should be welded together during tessellation to ensure a watertight result.
 * Each edge in the source mesh that is shared by two triangles will produce one weld pair, and the GPU will use these pairs to identify which vertices to weld.
 */
struct TessellationWeldPair
{
	TESS_U32 vertex_index_a TESS_INIT(0);
	TESS_U32 vertex_index_b TESS_INIT(0);
	TESS_U32 padding0 TESS_INIT(0);
	TESS_U32 padding1 TESS_INIT(0);
};

#if TESS_SHADER
struct TessellationIndex
{
	uint value;
};

uint tess_row_prefix(uint n, uint row)
{
	return row * (n + 1u) - (row * (row - 1u)) / 2u;
}

uint tess_vertex_index(uint n, uint row, uint col)
{
	return tess_row_prefix(n, row) + col;
}

void tess_grid_from_index(uint n, uint vertex_index, out uint row, out uint col)
{
	uint row_start = 0u;
	row = 0u;
	col = 0u;
	for (uint r = 0u; r <= 31u; ++r)
	{
		uint row_count = n - r + 1u;
		if (vertex_index < row_start + row_count)
		{
			row = r;
			break;
		}
		row_start += row_count;
	}

	col = vertex_index - row_start;
}

void tess_barycentric_from_grid(uint n, uint row, uint col, out float b0, out float b1, out float b2)
{
	float inv_n = 1.0 / float(max(n, 1u));
	b1 = float(col) * inv_n;
	b2 = float(row) * inv_n;
	b0 = 1.0 - b1 - b2;
}

void tess_barycentric_from_index(uint n, uint vertex_index, out float b0, out float b1, out float b2)
{
	uint row, col;
	tess_grid_from_index(n, vertex_index, row, col);
	tess_barycentric_from_grid(n, row, col, b0, b1, b2);
}

vec2 tess_patch_domain_uv(TessellationPatch tess_patch, float b0, float b1, float b2)
{
	return
		vec2(tess_patch.domain_u0, tess_patch.domain_v0) * b0 +
		vec2(tess_patch.domain_u1, tess_patch.domain_v1) * b1 +
		vec2(tess_patch.domain_u2, tess_patch.domain_v2) * b2;
}

void tess_source_barycentric_from_uv(vec2 uv, out float b0, out float b1, out float b2)
{
	b1 = uv.x;
	b2 = uv.y;
	b0 = 1.0 - b1 - b2;
}

vec3 tess_source_position_from_uv(vec3 p0, vec3 p1, vec3 p2, vec2 uv)
{
	float b0, b1, b2;
	tess_source_barycentric_from_uv(uv, b0, b1, b2);
	return p0 * b0 + p1 * b1 + p2 * b2;
}

float tess_snap_coord_to_segments(float coord, float segment_count)
{
	float safe_segment_count = max(segment_count, 1.0);
	return floor(clamp(coord, 0.0, 1.0) * safe_segment_count + 0.5) / safe_segment_count;
}

float tess_morph_edge_coord(float coord, float edge_lod, uint patch_factor)
{
	float clamped_lod = clamp(edge_lod, 1.0, float(max(patch_factor, 1u)));
	float lower_segments = max(floor(clamped_lod), 1.0);
	float upper_segments = max(ceil(clamped_lod), lower_segments);
	float lod_blend = clamped_lod - lower_segments;
	float lower_coord = tess_snap_coord_to_segments(coord, lower_segments);
	float upper_coord = tess_snap_coord_to_segments(coord, upper_segments);
	return mix(lower_coord, upper_coord, lod_blend);
}

bool tess_position_less(vec3 a, vec3 b)
{
	const float epsilon = 0.000001;
	if (abs(a.x - b.x) > epsilon) { return a.x < b.x; }
	if (abs(a.y - b.y) > epsilon) { return a.y < b.y; }
	return a.z < b.z;
}

float tess_morph_oriented_edge_coord(float coord, float edge_lod, uint patch_factor, vec3 edge_start, vec3 edge_end)
{
	bool forward_matches_canonical = !tess_position_less(edge_end, edge_start);
	float canonical_coord = forward_matches_canonical ? coord : 1.0 - coord;
	float morphed_coord = tess_morph_edge_coord(canonical_coord, edge_lod, patch_factor);
	return forward_matches_canonical ? morphed_coord : 1.0 - morphed_coord;
}

void tess_apply_edge_lod_morph(
	uint n,
	uint row,
	uint col,
	vec3 p0,
	vec3 p1,
	vec3 p2,
	float edge_lod0,
	float edge_lod1,
	float edge_lod2,
	inout float b0,
	inout float b1,
	inout float b2)
{
	if (n <= 1u)
	{
		return;
	}

	if (row == 0u)
	{
		float coord = tess_morph_oriented_edge_coord(b1, edge_lod0, n, p0, p1);
		b0 = 1.0 - coord;
		b1 = coord;
		b2 = 0.0;
		return;
	}

	if (row + col == n)
	{
		float coord = tess_morph_oriented_edge_coord(b2, edge_lod1, n, p1, p2);
		b0 = 0.0;
		b1 = 1.0 - coord;
		b2 = coord;
		return;
	}

	if (col == 0u)
	{
		float coord = tess_morph_oriented_edge_coord(b0, edge_lod2, n, p2, p0);
		b0 = coord;
		b1 = 0.0;
		b2 = 1.0 - coord;
	}
}

vec3 phong_project(vec3 p, vec3 source_position, vec3 source_normal)
{
	vec3 n = normalize(source_normal);
	return p - dot(p - source_position, n) * n;
}

@end // @block tessellation_types
#endif

#undef TESS_INIT
#undef TESS_F32
#undef TESS_U32
#undef TESS_SHADER
