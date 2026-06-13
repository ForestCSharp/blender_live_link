#pragma once

#include "shader_common.h"

#if defined(__cplusplus) && defined(__STDC__)
#include "core/types.h"
#endif

#if !defined(__cplusplus) || !defined(__STDC__)
#define TESS_SHADER 1
#define TESS_U32 uint
#define TESS_INIT(value)
#else
#define TESS_SHADER 0
#define TESS_U32 u32
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

void tess_barycentric_from_index(uint n, uint vertex_index, out float b0, out float b1, out float b2)
{
	uint row = 0u;
	uint row_start = 0u;
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

	uint col = vertex_index - row_start;
	float inv_n = 1.0 / float(max(n, 1u));
	b1 = float(col) * inv_n;
	b2 = float(row) * inv_n;
	b0 = 1.0 - b1 - b2;
}

vec3 phong_project(vec3 p, vec3 source_position, vec3 source_normal)
{
	vec3 n = normalize(source_normal);
	return p - dot(p - source_position, n) * n;
}

@end // @block tessellation_types
#endif

#undef TESS_INIT
#undef TESS_U32
#undef TESS_SHADER
