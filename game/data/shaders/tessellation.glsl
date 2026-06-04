// Common Shader Code
#include "shader_common.h"
#include "tessellation_common.h"

@cs emit_vertices

@include_block tessellation_types

layout(binding=0) uniform cs_params {
	int count;
	int base_index;
	float phong_strength;
	int padding0;
};

layout(binding=0) readonly buffer SourceVerticesBuffer {
	TessellationVertex source_vertices[];
};

layout(binding=1) readonly buffer SourceIndicesBuffer {
	TessellationIndex source_indices[];
};

layout(binding=2) readonly buffer PatchesBuffer {
	TessellationPatch patches[];
};

layout(binding=3) buffer GeneratedVerticesBuffer {
	TessellationVertex generated_vertices[];
};

layout(local_size_x=128, local_size_y=1, local_size_z=1) in;

void main()
{
	uint patch_index = uint(base_index) + gl_WorkGroupID.x;
	if (patch_index >= uint(count))
	{
		return;
	}

	TessellationPatch tess_patch = patches[patch_index];
	uint n = max(tess_patch.tess_factor, 1u);
	uint source_index_offset = tess_patch.source_index_offset;

	uint i0 = source_indices[source_index_offset + 0u].value;
	uint i1 = source_indices[source_index_offset + 1u].value;
	uint i2 = source_indices[source_index_offset + 2u].value;

	TessellationVertex v0 = source_vertices[i0];
	TessellationVertex v1 = source_vertices[i1];
	TessellationVertex v2 = source_vertices[i2];

	for (uint local_vertex_index = gl_LocalInvocationID.x; local_vertex_index < tess_patch.vertex_count; local_vertex_index += gl_WorkGroupSize.x)
	{
		float b0;
		float b1;
		float b2;
		tess_barycentric_from_index(n, local_vertex_index, b0, b1, b2);

		vec3 p0 = v0.position.xyz;
		vec3 p1 = v1.position.xyz;
		vec3 p2 = v2.position.xyz;

		vec3 linear_position = p0 * b0 + p1 * b1 + p2 * b2;
		vec3 projected_position =
			phong_project(linear_position, p0, v0.normal.xyz) * b0 +
			phong_project(linear_position, p1, v1.normal.xyz) * b1 +
			phong_project(linear_position, p2, v2.normal.xyz) * b2;

		TessellationVertex out_vertex;
		out_vertex.position = vec4(mix(linear_position, projected_position, clamp(phong_strength, 0.0, 1.0)), 1.0);
		out_vertex.normal = vec4(normalize(v0.normal.xyz * b0 + v1.normal.xyz * b1 + v2.normal.xyz * b2), 0.0);
		out_vertex.texcoord = v0.texcoord * b0 + v1.texcoord * b1 + v2.texcoord * b2;
		out_vertex.padding0 = 0.0;
		out_vertex.padding1 = 0.0;

		generated_vertices[tess_patch.generated_vertex_offset + local_vertex_index] = out_vertex;
	}
}
@end

@cs emit_indices

@include_block tessellation_types

layout(binding=0) uniform cs_params {
	int count;
	int base_index;
	float phong_strength;
	int padding0;
};

layout(binding=0) readonly buffer IndexPatchesBuffer {
	TessellationPatch index_patches[];
};

layout(binding=1) buffer GeneratedIndicesBuffer {
	TessellationIndex generated_indices[];
};

layout(binding=2) buffer GeneratedWireIndicesBuffer {
	TessellationIndex generated_wire_indices[];
};

layout(local_size_x=128, local_size_y=1, local_size_z=1) in;

void main()
{
	uint patch_index = uint(base_index) + gl_WorkGroupID.x;
	if (patch_index >= uint(count))
	{
		return;
	}

	TessellationPatch tess_patch = index_patches[patch_index];
	uint n = max(tess_patch.tess_factor, 1u);
	uint tri_count = n * n;

	for (uint local_tri_index = gl_LocalInvocationID.x; local_tri_index < tri_count; local_tri_index += gl_WorkGroupSize.x)
	{
		uint row = 0u;
		uint tri_start = 0u;
		for (uint r = 0u; r < 31u; ++r)
		{
			uint row_tri_count = 2u * (n - r) - 1u;
			if (local_tri_index < tri_start + row_tri_count)
			{
				row = r;
				break;
			}
			tri_start += row_tri_count;
		}

		uint tri_in_row = local_tri_index - tri_start;
		uint col = tri_in_row / 2u;
		bool upper = (tri_in_row & 1u) == 0u;

		uint a;
		uint b;
		uint c;
		if (upper)
		{
			a = tess_vertex_index(n, row, col);
			b = tess_vertex_index(n, row, col + 1u);
			c = tess_vertex_index(n, row + 1u, col);
		}
		else
		{
			a = tess_vertex_index(n, row, col + 1u);
			b = tess_vertex_index(n, row + 1u, col + 1u);
			c = tess_vertex_index(n, row + 1u, col);
		}

		uint out_index = tess_patch.generated_index_offset + local_tri_index * 3u;
		uint vertex_offset = tess_patch.generated_vertex_offset;
		generated_indices[out_index + 0u].value = vertex_offset + a;
		generated_indices[out_index + 1u].value = vertex_offset + b;
		generated_indices[out_index + 2u].value = vertex_offset + c;

		uint out_wire_index = tess_patch.generated_index_offset * 2u + local_tri_index * 6u;
		generated_wire_indices[out_wire_index + 0u].value = vertex_offset + a;
		generated_wire_indices[out_wire_index + 1u].value = vertex_offset + b;
		generated_wire_indices[out_wire_index + 2u].value = vertex_offset + b;
		generated_wire_indices[out_wire_index + 3u].value = vertex_offset + c;
		generated_wire_indices[out_wire_index + 4u].value = vertex_offset + c;
		generated_wire_indices[out_wire_index + 5u].value = vertex_offset + a;
	}
}
@end

@cs weld_edges

@include_block tessellation_types

layout(binding=0) uniform cs_params {
	int count;
	int base_index;
	float phong_strength;
	int padding0;
};

layout(binding=0) buffer WeldVerticesBuffer {
	TessellationVertex weld_vertices[];
};

layout(binding=1) readonly buffer WeldPairsBuffer {
	TessellationWeldPair weld_pairs[];
};

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

void main()
{
	uint pair_index = uint(base_index) + gl_GlobalInvocationID.x;
	if (pair_index >= uint(count))
	{
		return;
	}

	TessellationWeldPair pair = weld_pairs[pair_index];
	TessellationVertex a = weld_vertices[pair.vertex_index_a];
	TessellationVertex b = weld_vertices[pair.vertex_index_b];

	vec3 normal_sum = a.normal.xyz + b.normal.xyz;
	if (dot(normal_sum, normal_sum) < 0.000001)
	{
		normal_sum = a.normal.xyz;
	}
	vec4 average_position = vec4((a.position.xyz + b.position.xyz) * 0.5, 1.0);
	vec4 average_normal = vec4(normalize(normal_sum), 0.0);

	a.position = average_position;
	b.position = average_position;
	a.normal = average_normal;
	b.normal = average_normal;

	weld_vertices[pair.vertex_index_a] = a;
	weld_vertices[pair.vertex_index_b] = b;
}
@end

@cs weld_corners

@include_block tessellation_types

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

void main()
{
}
@end

@program emit_vertices emit_vertices
@program emit_indices emit_indices
@program weld_edges weld_edges
@program weld_corners weld_corners
