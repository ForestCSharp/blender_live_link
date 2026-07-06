// Common Shader Code
#include "shader_common.h"
#include "tessellation_common.h"

@cs clear_counters

@include_block tessellation_types

layout(binding=0) buffer ClearCountersBuffer {
	TessellationCounters clear_counters[];
};

layout(local_size_x=32, local_size_y=1, local_size_z=1) in;

/**
 * clear_counters resets the tessellation counter buffer before a new planning pass.
 * Only one invocation writes the shared counter record for the dispatch.
 */
void main()
{
	if (gl_LocalInvocationID.x != 0u)
	{
		return;
	}

	clear_counters[0].patch_count = 0u;
	clear_counters[0].vertex_count = 0u;
	clear_counters[0].index_count = 0u;
	clear_counters[0].wire_index_count = 0u;
	clear_counters[0].source_triangle_count = 0u;
	clear_counters[0].overflowed = 0u;
	clear_counters[0].max_factor_seen = 1u;
	clear_counters[0].padding0 = 0u;
}
@end

@cs measure_mesh_factor

@include_block tessellation_types

layout(binding=0) uniform plan_params {
	mat4 model_matrix;
	vec4 camera_position;
	int source_triangle_count;
	int base_triangle_index;
	int max_patch_count;
	int max_vertex_count;
	int max_index_count;
	int max_factor;
	int virtual_patches_enabled;
	int virtual_patch_max_depth;
	int tessellation_mode;
	int fixed_factor;
	int plan_padding0;
	int plan_padding1;
	float fov_radians;
	float render_height;
	float target_pixels_per_segment;
	float padding0;
};

layout(binding=0) readonly buffer MeasureSourceVerticesBuffer {
	TessellationVertex measure_source_vertices[];
};

layout(binding=1) readonly buffer MeasureSourceIndicesBuffer {
	TessellationIndex measure_source_indices[];
};

layout(binding=2) buffer MeasureCountersBuffer {
	TessellationCounters measure_counters[];
};

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

/**
 * Transforms a source vertex from mesh-local space into world space for screen-size LOD measurement.
 */
vec3 measure_world_position(vec3 local_position)
{
	return (model_matrix * vec4(local_position, 1.0)).xyz;
}

/**
 * Estimates the tessellation LOD for an edge from its world-space length and distance to the camera.
 * The result is expressed as desired segments based on approximate projected pixel length.
 */
float measure_raw_lod_for_edge_at_distance(float edge_length, float distance_to_camera)
{
	float safe_distance = max(distance_to_camera, 0.001);
	float sine_half_angle = clamp(edge_length / (2.0 * safe_distance), 0.0, 1.0);
	float angular_length = 2.0 * asin(sine_half_angle);
	float pixel_length = (angular_length / max(fov_radians, 0.001)) * render_height;
	return max(pixel_length / max(target_pixels_per_segment, 1.0), 1.0);
}

/**
 * Estimates the tessellation LOD for a world-space edge using the edge midpoint as the camera-distance sample.
 */
float measure_raw_lod_for_edge(vec3 a, vec3 b)
{
	vec3 midpoint = (a + b) * 0.5;
	float distance_to_camera = max(length(midpoint - camera_position.xyz), 0.001);
	return measure_raw_lod_for_edge_at_distance(length(b - a), distance_to_camera);
}

/**
 * measure_mesh_factor scans source triangles and records the highest adaptive factor needed by this mesh.
 * The planner later uses this value for adaptive per-mesh tessellation.
 */
void main()
{
	uint source_triangle_index = uint(base_triangle_index) + gl_WorkGroupID.x;
	if (source_triangle_index >= uint(max(source_triangle_count, 0)) || gl_LocalInvocationID.x != 0u)
	{
		return;
	}

	uint source_index_offset = source_triangle_index * 3u;
	uint i0 = measure_source_indices[source_index_offset + 0u].value;
	uint i1 = measure_source_indices[source_index_offset + 1u].value;
	uint i2 = measure_source_indices[source_index_offset + 2u].value;
	vec3 p0 = measure_world_position(measure_source_vertices[i0].position.xyz);
	vec3 p1 = measure_world_position(measure_source_vertices[i1].position.xyz);
	vec3 p2 = measure_world_position(measure_source_vertices[i2].position.xyz);
	uint max_factor_u = uint(clamp(max_factor, 1, 31));
	float max_lod = max(measure_raw_lod_for_edge(p0, p1), max(measure_raw_lod_for_edge(p1, p2), measure_raw_lod_for_edge(p2, p0)));
	uint factor = clamp(uint(ceil(max_lod)), 1u, max_factor_u);
	atomicMax(measure_counters[0].max_factor_seen, factor);
}
@end

@cs plan_patches

@include_block tessellation_types

layout(binding=0) uniform plan_params {
	mat4 model_matrix;
	vec4 camera_position;
	int source_triangle_count;
	int base_triangle_index;
	int max_patch_count;
	int max_vertex_count;
	int max_index_count;
	int max_factor;
	int virtual_patches_enabled;
	int virtual_patch_max_depth;
	int tessellation_mode;
	int fixed_factor;
	int plan_padding0;
	int plan_padding1;
	float fov_radians;
	float render_height;
	float target_pixels_per_segment;
	float padding0;
};

layout(binding=0) readonly buffer PlanSourceVerticesBuffer {
	TessellationVertex plan_source_vertices[];
};

layout(binding=1) readonly buffer PlanSourceIndicesBuffer {
	TessellationIndex plan_source_indices[];
};

layout(binding=2) buffer PlanPatchesBuffer {
	TessellationPatch plan_patches[];
};

layout(binding=3) buffer PlanCountersBuffer {
	TessellationCounters plan_counters[];
};

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

const int TESS_MODE_FIXED = 0;
const int TESS_MODE_ADAPTIVE_PER_MESH = 1;
const int TESS_MODE_ADAPTIVE_PER_TRIANGLE = 2;

/**
 * Transforms a source vertex from mesh-local space into world space for patch planning.
 */
vec3 plan_world_position(vec3 local_position)
{
	return (model_matrix * vec4(local_position, 1.0)).xyz;
}

/**
 * Estimates the tessellation LOD for an edge from its world-space length and distance to the camera.
 * The result is expressed as desired segments based on approximate projected pixel length.
 */
float plan_raw_lod_for_edge_at_distance(float edge_length, float distance_to_camera)
{
	float safe_distance = max(distance_to_camera, 0.001);
	float sine_half_angle = clamp(edge_length / (2.0 * safe_distance), 0.0, 1.0);
	float angular_length = 2.0 * asin(sine_half_angle);
	float pixel_length = (angular_length / max(fov_radians, 0.001)) * render_height;
	return max(pixel_length / max(target_pixels_per_segment, 1.0), 1.0);
}

/**
 * Estimates the tessellation LOD for a world-space edge using the edge midpoint as the camera-distance sample.
 */
float plan_raw_lod_for_edge(vec3 a, vec3 b)
{
	vec3 midpoint = (a + b) * 0.5;
	float distance_to_camera = max(length(midpoint - camera_position.xyz), 0.001);
	return plan_raw_lod_for_edge_at_distance(length(b - a), distance_to_camera);
}

/**
 * Finds the closest point on a triangle to the camera or another world-space sample point.
 * Virtual patch planning uses this to account for large triangles whose closest point is not on an edge midpoint.
 */
vec3 plan_closest_point_on_triangle(vec3 point, vec3 a, vec3 b, vec3 c)
{
	vec3 ab = b - a;
	vec3 ac = c - a;
	vec3 ap = point - a;
	float d1 = dot(ab, ap);
	float d2 = dot(ac, ap);
	if (d1 <= 0.0 && d2 <= 0.0) { return a; }

	vec3 bp = point - b;
	float d3 = dot(ab, bp);
	float d4 = dot(ac, bp);
	if (d3 >= 0.0 && d4 <= d3) { return b; }

	float vc = d1 * d4 - d3 * d2;
	if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
	{
		float v = d1 / (d1 - d3);
		return a + ab * v;
	}

	vec3 cp = point - c;
	float d5 = dot(ab, cp);
	float d6 = dot(ac, cp);
	if (d6 >= 0.0 && d5 <= d6) { return c; }

	float vb = d5 * d2 - d1 * d6;
	if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
	{
		float w = d2 / (d2 - d6);
		return a + ac * w;
	}

	float va = d3 * d6 - d5 * d4;
	if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
	{
		float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		return b + (c - b) * w;
	}

	float denom = 1.0 / max(va + vb + vc, 0.000001);
	float v = vb * denom;
	float w = vc * denom;
	return a + ab * v + ac * w;
}

/**
 * Rounds a positive segment request up to the next power of two for regular virtual-patch subdivision.
 */
uint plan_next_power_of_two(uint value)
{
	uint result = 1u;
	while (result < value)
	{
		result <<= 1u;
	}
	return result;
}

/**
 * Chooses how many virtual segments should split a source triangle before per-patch tessellation.
 * Splitting is only enabled for adaptive per-triangle mode and is capped by the configured virtual patch depth.
 */
uint plan_split_segments(vec3 p0, vec3 p1, vec3 p2)
{
	if (tessellation_mode != TESS_MODE_ADAPTIVE_PER_TRIANGLE || virtual_patches_enabled == 0)
	{
		return 1u;
	}

	uint max_depth = uint(clamp(virtual_patch_max_depth, 0, 4));
	if (max_depth == 0u)
	{
		return 1u;
	}

	float edge_lod = max(plan_raw_lod_for_edge(p0, p1), max(plan_raw_lod_for_edge(p1, p2), plan_raw_lod_for_edge(p2, p0)));
	vec3 closest_point = plan_closest_point_on_triangle(camera_position.xyz, p0, p1, p2);
	float closest_distance = length(closest_point - camera_position.xyz);
	float max_edge_length = max(length(p1 - p0), max(length(p2 - p1), length(p0 - p2)));
	float closest_lod = plan_raw_lod_for_edge_at_distance(max_edge_length, closest_distance);
	float raw_lod = max(edge_lod, closest_lod);

	uint max_factor_u = uint(clamp(max_factor, 1, 31));
	uint requested_segments = max(uint(ceil(raw_lod / float(max_factor_u))), 1u);
	uint max_segments = 1u << max_depth;
	return min(plan_next_power_of_two(requested_segments), max_segments);
}

/**
 * Converts a virtual-patch grid coordinate into UV coordinates inside the source triangle domain.
 */
vec2 plan_domain_grid_point(uint split_segments, uint row, uint col)
{
	float inv_segments = 1.0 / float(max(split_segments, 1u));
	return vec2(float(col) * inv_segments, float(row) * inv_segments);
}

/**
 * Builds the domain triangle for one virtual patch inside a split source triangle.
 * The returned TessellationPatch carries only domain coordinates; the planner fills offsets, counts, and LODs later.
 */
TessellationPatch plan_virtual_patch_domain(uint split_segments, uint patch_index)
{
	uint row = 0u;
	uint tri_start = 0u;
	for (uint r = 0u; r < 16u; ++r)
	{
		if (r >= split_segments)
		{
			break;
		}
		uint row_tri_count = 2u * (split_segments - r) - 1u;
		if (patch_index < tri_start + row_tri_count)
		{
			row = r;
			break;
		}
		tri_start += row_tri_count;
	}

	uint tri_in_row = patch_index - tri_start;
	uint col = tri_in_row / 2u;
	bool upper = (tri_in_row & 1u) == 0u;
	vec2 d0;
	vec2 d1;
	vec2 d2;
	if (upper)
	{
		d0 = plan_domain_grid_point(split_segments, row, col);
		d1 = plan_domain_grid_point(split_segments, row, col + 1u);
		d2 = plan_domain_grid_point(split_segments, row + 1u, col);
	}
	else
	{
		d0 = plan_domain_grid_point(split_segments, row, col + 1u);
		d1 = plan_domain_grid_point(split_segments, row + 1u, col + 1u);
		d2 = plan_domain_grid_point(split_segments, row + 1u, col);
	}

	TessellationPatch out_patch;
	out_patch.domain_u0 = d0.x;
	out_patch.domain_v0 = d0.y;
	out_patch.domain_u1 = d1.x;
	out_patch.domain_v1 = d1.y;
	out_patch.domain_u2 = d2.x;
	out_patch.domain_v2 = d2.y;
	return out_patch;
}

/**
 * Interpolates a source-triangle position from UV coordinates in that triangle's barycentric domain.
 */
vec3 plan_source_position_from_domain(vec3 p0, vec3 p1, vec3 p2, vec2 uv)
{
	float b1 = uv.x;
	float b2 = uv.y;
	float b0 = 1.0 - b1 - b2;
	return p0 * b0 + p1 * b1 + p2 * b2;
}

/**
 * Fills a planned patch with edge LODs, tessellation factor, and generated vertex/index counts.
 * Fixed and per-mesh modes use uniform factors, while per-triangle mode measures each patch domain edge.
 */
void plan_fill_patch_lods(inout TessellationPatch out_patch, vec3 source_p0, vec3 source_p1, vec3 source_p2)
{
	uint max_factor_u = uint(clamp(max_factor, 1, 31));
	if (tessellation_mode == TESS_MODE_FIXED)
	{
		uint factor = uint(clamp(fixed_factor, 1, max_factor));
		out_patch.edge_lod0 = float(factor);
		out_patch.edge_lod1 = float(factor);
		out_patch.edge_lod2 = float(factor);
		out_patch.tess_factor = factor;
		out_patch.vertex_count = tess_vertex_count_for_factor(out_patch.tess_factor);
		out_patch.index_count = tess_index_count_for_factor(out_patch.tess_factor);
		return;
	}

	if (tessellation_mode == TESS_MODE_ADAPTIVE_PER_MESH)
	{
		uint factor = clamp(plan_counters[0].max_factor_seen, 1u, max_factor_u);
		out_patch.edge_lod0 = float(factor);
		out_patch.edge_lod1 = float(factor);
		out_patch.edge_lod2 = float(factor);
		out_patch.tess_factor = factor;
		out_patch.vertex_count = tess_vertex_count_for_factor(out_patch.tess_factor);
		out_patch.index_count = tess_index_count_for_factor(out_patch.tess_factor);
		return;
	}

	vec3 p0 = plan_source_position_from_domain(source_p0, source_p1, source_p2, vec2(out_patch.domain_u0, out_patch.domain_v0));
	vec3 p1 = plan_source_position_from_domain(source_p0, source_p1, source_p2, vec2(out_patch.domain_u1, out_patch.domain_v1));
	vec3 p2 = plan_source_position_from_domain(source_p0, source_p1, source_p2, vec2(out_patch.domain_u2, out_patch.domain_v2));
	out_patch.edge_lod0 = clamp(plan_raw_lod_for_edge(p0, p1), 1.0, float(max_factor_u));
	out_patch.edge_lod1 = clamp(plan_raw_lod_for_edge(p1, p2), 1.0, float(max_factor_u));
	out_patch.edge_lod2 = clamp(plan_raw_lod_for_edge(p2, p0), 1.0, float(max_factor_u));
	float max_edge_lod = max(out_patch.edge_lod0, max(out_patch.edge_lod1, out_patch.edge_lod2));
	out_patch.tess_factor = clamp(uint(ceil(max_edge_lod)), 1u, max_factor_u);
	out_patch.vertex_count = tess_vertex_count_for_factor(out_patch.tess_factor);
	out_patch.index_count = tess_index_count_for_factor(out_patch.tess_factor);
}

/**
 * plan_patches emits TessellationPatch records for source triangles or their virtual subpatches.
 * It atomically reserves generated buffer ranges and marks overflow when the configured budgets are exceeded.
 */
void main()
{
	uint source_triangle_index = uint(base_triangle_index) + gl_WorkGroupID.x;
	if (source_triangle_index >= uint(max(source_triangle_count, 0)))
	{
		return;
	}

	if (gl_LocalInvocationID.x == 0u)
	{
		atomicAdd(plan_counters[0].source_triangle_count, 1u);
	}

	uint source_index_offset = source_triangle_index * 3u;
	uint i0 = plan_source_indices[source_index_offset + 0u].value;
	uint i1 = plan_source_indices[source_index_offset + 1u].value;
	uint i2 = plan_source_indices[source_index_offset + 2u].value;
	vec3 p0 = plan_world_position(plan_source_vertices[i0].position.xyz);
	vec3 p1 = plan_world_position(plan_source_vertices[i1].position.xyz);
	vec3 p2 = plan_world_position(plan_source_vertices[i2].position.xyz);
	uint split_segments = plan_split_segments(p0, p1, p2);
	uint virtual_patch_count = split_segments * split_segments;
	uint max_patch_count_u = uint(max(max_patch_count, 0));
	uint max_vertex_count_u = uint(max(max_vertex_count, 0));
	uint max_index_count_u = uint(max(max_index_count, 0));

	for (uint virtual_patch_index = gl_LocalInvocationID.x; virtual_patch_index < virtual_patch_count; virtual_patch_index += gl_WorkGroupSize.x)
	{
		TessellationPatch out_patch = split_segments > 1u
			? plan_virtual_patch_domain(split_segments, virtual_patch_index)
			: TessellationPatch(0u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
		plan_fill_patch_lods(out_patch, p0, p1, p2);
		out_patch.source_index_offset = source_index_offset;
		out_patch.source_triangle_index = source_triangle_index;

		uint patch_slot = atomicAdd(plan_counters[0].patch_count, 1u);
		uint vertex_offset = atomicAdd(plan_counters[0].vertex_count, out_patch.vertex_count);
		uint index_offset = atomicAdd(plan_counters[0].index_count, out_patch.index_count);
		uint wire_index_offset = atomicAdd(plan_counters[0].wire_index_count, out_patch.index_count * 2u);
		atomicMax(plan_counters[0].max_factor_seen, out_patch.tess_factor);

		bool overflowed =
			patch_slot >= max_patch_count_u ||
			vertex_offset + out_patch.vertex_count > max_vertex_count_u ||
			index_offset + out_patch.index_count > max_index_count_u ||
			wire_index_offset + out_patch.index_count * 2u > max_index_count_u * 2u;
		if (overflowed)
		{
			atomicExchange(plan_counters[0].overflowed, 1u);
			if (patch_slot < max_patch_count_u)
			{
				out_patch.generated_vertex_offset = 0u;
				out_patch.generated_index_offset = 0u;
				out_patch.vertex_count = 0u;
				out_patch.index_count = 0u;
				plan_patches[patch_slot] = out_patch;
			}
			continue;
		}

		out_patch.generated_vertex_offset = vertex_offset;
		out_patch.generated_index_offset = index_offset;
		plan_patches[patch_slot] = out_patch;
	}
}
@end

@cs emit_vertices_gpu

@include_block tessellation_types

layout(binding=0) uniform cs_params {
	int count;
	int base_index;
	float phong_strength;
	int padding0;
};

layout(binding=0) readonly buffer GpuSourceVerticesBuffer {
	TessellationVertex gpu_source_vertices[];
};

layout(binding=1) readonly buffer GpuSourceIndicesBuffer {
	TessellationIndex gpu_source_indices[];
};

layout(binding=2) readonly buffer GpuPatchesBuffer {
	TessellationPatch gpu_patches[];
};

layout(binding=3) buffer GpuGeneratedVerticesBuffer {
	TessellationVertex gpu_generated_vertices[];
};

layout(binding=4) readonly buffer GpuEmitCountersBuffer {
	TessellationCounters gpu_emit_counters[];
};

layout(local_size_x=128, local_size_y=1, local_size_z=1) in;

/**
 * emit_vertices_gpu builds the generated vertex grid for each planned patch.
 * It remaps patch-local barycentrics through the patch domain, applies edge LOD morphing, and writes interpolated vertex attributes.
 */
void main()
{
	uint patch_index = uint(base_index) + gl_WorkGroupID.x;
	if (patch_index >= min(uint(count), gpu_emit_counters[0].patch_count))
	{
		return;
	}

	TessellationPatch tess_patch = gpu_patches[patch_index];
	if (tess_patch.vertex_count == 0u)
	{
		return;
	}

	uint n = max(tess_patch.tess_factor, 1u);
	uint source_index_offset = tess_patch.source_index_offset;

	uint i0 = gpu_source_indices[source_index_offset + 0u].value;
	uint i1 = gpu_source_indices[source_index_offset + 1u].value;
	uint i2 = gpu_source_indices[source_index_offset + 2u].value;

	TessellationVertex v0 = gpu_source_vertices[i0];
	TessellationVertex v1 = gpu_source_vertices[i1];
	TessellationVertex v2 = gpu_source_vertices[i2];
	vec3 p0 = v0.position.xyz;
	vec3 p1 = v1.position.xyz;
	vec3 p2 = v2.position.xyz;
	vec3 domain_p0 = tess_source_position_from_uv(p0, p1, p2, vec2(tess_patch.domain_u0, tess_patch.domain_v0));
	vec3 domain_p1 = tess_source_position_from_uv(p0, p1, p2, vec2(tess_patch.domain_u1, tess_patch.domain_v1));
	vec3 domain_p2 = tess_source_position_from_uv(p0, p1, p2, vec2(tess_patch.domain_u2, tess_patch.domain_v2));

	for (uint local_vertex_index = gl_LocalInvocationID.x; local_vertex_index < tess_patch.vertex_count; local_vertex_index += gl_WorkGroupSize.x)
	{
		uint row, col;
		float b0, b1, b2;
		tess_grid_from_index(n, local_vertex_index, row, col);
		tess_barycentric_from_grid(n, row, col, b0, b1, b2);
		tess_apply_edge_lod_morph(n, row, col, domain_p0, domain_p1, domain_p2, tess_patch.edge_lod0, tess_patch.edge_lod1, tess_patch.edge_lod2, b0, b1, b2);

		vec2 source_uv = tess_patch_domain_uv(tess_patch, b0, b1, b2);
		float source_b0, source_b1, source_b2;
		tess_source_barycentric_from_uv(source_uv, source_b0, source_b1, source_b2);
		vec3 linear_position = p0 * source_b0 + p1 * source_b1 + p2 * source_b2;
		vec3 projected_position =
			phong_project(linear_position, p0, v0.normal.xyz) * source_b0 +
			phong_project(linear_position, p1, v1.normal.xyz) * source_b1 +
			phong_project(linear_position, p2, v2.normal.xyz) * source_b2;

		TessellationVertex out_vertex;
		out_vertex.position = vec4(mix(linear_position, projected_position, clamp(phong_strength, 0.0, 1.0)), 1.0);
		out_vertex.normal = vec4(normalize(v0.normal.xyz * source_b0 + v1.normal.xyz * source_b1 + v2.normal.xyz * source_b2), 0.0);
		out_vertex.texcoord = v0.texcoord * source_b0 + v1.texcoord * source_b1 + v2.texcoord * source_b2;
		out_vertex.padding0 = 0.0;
		out_vertex.padding1 = 0.0;
		gpu_generated_vertices[tess_patch.generated_vertex_offset + local_vertex_index] = out_vertex;
	}
}
@end

@cs emit_indices_gpu

@include_block tessellation_types

layout(binding=0) uniform cs_params {
	int count;
	int base_index;
	float phong_strength;
	int padding0;
};

layout(binding=0) readonly buffer GpuIndexPatchesBuffer {
	TessellationPatch gpu_index_patches[];
};

layout(binding=1) buffer GpuGeneratedIndicesBuffer {
	TessellationIndex gpu_generated_indices[];
};

layout(binding=2) buffer GpuGeneratedWireIndicesBuffer {
	TessellationIndex gpu_generated_wire_indices[];
};

layout(binding=3) readonly buffer GpuIndexCountersBuffer {
	TessellationCounters gpu_index_counters[];
};

layout(local_size_x=128, local_size_y=1, local_size_z=1) in;

/**
 * emit_indices_gpu writes triangle and wireframe index buffers for each generated patch grid.
 * Each patch produces tess_factor * tess_factor small triangles in the same compact grid ordering used by vertex emission.
 */
void main()
{
	uint patch_index = uint(base_index) + gl_WorkGroupID.x;
	if (patch_index >= min(uint(count), gpu_index_counters[0].patch_count))
	{
		return;
	}

	TessellationPatch tess_patch = gpu_index_patches[patch_index];
	if (tess_patch.index_count == 0u)
	{
		return;
	}
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

		uint a, b, c;
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
		gpu_generated_indices[out_index + 0u].value = vertex_offset + a;
		gpu_generated_indices[out_index + 1u].value = vertex_offset + b;
		gpu_generated_indices[out_index + 2u].value = vertex_offset + c;

		uint out_wire_index = tess_patch.generated_index_offset * 2u + local_tri_index * 6u;
		gpu_generated_wire_indices[out_wire_index + 0u].value = vertex_offset + a;
		gpu_generated_wire_indices[out_wire_index + 1u].value = vertex_offset + b;
		gpu_generated_wire_indices[out_wire_index + 2u].value = vertex_offset + b;
		gpu_generated_wire_indices[out_wire_index + 3u].value = vertex_offset + c;
		gpu_generated_wire_indices[out_wire_index + 4u].value = vertex_offset + c;
		gpu_generated_wire_indices[out_wire_index + 5u].value = vertex_offset + a;
	}
}
@end

@program clear_counters clear_counters
@program measure_mesh_factor measure_mesh_factor
@program plan_patches plan_patches
@program emit_vertices_gpu emit_vertices_gpu
@program emit_indices_gpu emit_indices_gpu
