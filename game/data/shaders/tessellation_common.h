#ifndef GAME2_TESSELLATION_COMMON_H
#define GAME2_TESSELLATION_COMMON_H

// Tessellation compute shaders own descriptor set 0 for their storage buffers,
// so the shared scene resources (materials + bindless textures, needed for
// displacement sampling) are bound at set 1 instead. Must be defined before
// shader_common.h declares them. No effect in the C++ branch.
#if !defined(__cplusplus)
#define SHADER_COMMON_SET 1
#endif

#include "shader_common.h"

#if defined(__cplusplus)
#include "core/types.h"
#endif

#if !defined(__cplusplus)
#define TESS_SHADER 1
#define TESS_U32 uint
#define TESS_I32 int
#define TESS_F32 float
#define TESS_INIT(value)
#else
#define TESS_SHADER 0
#define TESS_U32 u32
#define TESS_I32 i32
#define TESS_F32 f32
#define TESS_INIT(value) = value
#endif

#if TESS_SHADER

/**
 * TessellationVertex mirrors the runtime vertex layout used by tessellation compute shaders.
 * Generated tessellation vertices are written in this format so normal render passes can draw them directly.
 */
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
 * TessellationCounters stores per-dispatch totals and overflow state for the tessellation pipeline.
 * The planner increments these values while reserving patch, vertex, triangle-index, and wire-index ranges.
 */
struct TessellationCounters
{
	TESS_U32 patch_count TESS_INIT(0);
	TESS_U32 vertex_count TESS_INIT(0);
	TESS_U32 index_count TESS_INIT(0);
	TESS_U32 wire_index_count TESS_INIT(0);
	TESS_U32 source_triangle_count TESS_INIT(0);
	TESS_U32 overflowed TESS_INIT(0);
	TESS_U32 max_factor_seen TESS_INIT(1);
	TESS_U32 padding0 TESS_INIT(0);
};

#if TESS_SHADER
/**
 * TessellationIndex wraps a single index value so index buffers can use the same storage-buffer conventions as other tessellation data.
 */
struct TessellationIndex
{
	uint value;
};

vec3 tess_world_position(mat4 model_matrix, vec3 local_position)
{
	return (model_matrix * vec4(local_position, 1.0)).xyz;
}

float tess_raw_lod_for_edge_at_distance(
	float edge_length,
	float distance_to_camera,
	vec3 projection_parameters)
{
	float safe_distance = max(distance_to_camera, 0.001);
	float sine_half_angle = clamp(
		edge_length / (2.0 * safe_distance), 0.0, 1.0);
	float angular_length = 2.0 * asin(sine_half_angle);
	float pixel_length = (angular_length / max(projection_parameters.x, 0.001))
		* projection_parameters.y;
	return max(
		pixel_length / max(projection_parameters.z, 1.0), 1.0);
}

float tess_raw_lod_for_edge(
	vec3 a,
	vec3 b,
	vec3 camera_position,
	vec3 projection_parameters)
{
	vec3 midpoint = (a + b) * 0.5;
	float distance_to_camera = max(
		length(midpoint - camera_position), 0.001);
	return tess_raw_lod_for_edge_at_distance(
		length(b - a),
		distance_to_camera,
		projection_parameters);
}

/**
 * Returns the number of vertices in a triangular tessellation grid for the requested factor.
 * The factor is clamped to the supported shader range before the triangular-number count is computed.
 */
uint tess_vertex_count_for_factor(uint factor)
{
	uint n = clamp(factor, 1u, 31u);
	return ((n + 1u) * (n + 2u)) / 2u;
}

/**
 * Returns the number of triangle indices needed for a tessellation grid with the requested factor.
 * A factor of n produces n * n generated triangles, each with three indices.
 */
uint tess_index_count_for_factor(uint factor)
{
	uint n = clamp(factor, 1u, 31u);
	return n * n * 3u;
}

/**
 * Returns the first vertex index for a row in the compact triangular grid ordering.
 * Rows shrink by one vertex as they move from the base edge toward the opposite vertex.
 */
uint tess_row_prefix(uint n, uint row)
{
	return row * (n + 1u) - (row * (row - 1u)) / 2u;
}

/**
 * Converts a triangular-grid row and column into the compact patch-local vertex index.
 */
uint tess_vertex_index(uint n, uint row, uint col)
{
	return tess_row_prefix(n, row) + col;
}

/**
 * Converts a compact patch-local vertex index back into triangular-grid row and column coordinates.
 */
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

/**
 * Converts triangular-grid row and column coordinates into barycentric weights for the patch domain.
 */
void tess_barycentric_from_grid(uint n, uint row, uint col, out float b0, out float b1, out float b2)
{
	float inv_n = 1.0 / float(max(n, 1u));
	b1 = float(col) * inv_n;
	b2 = float(row) * inv_n;
	b0 = 1.0 - b1 - b2;
}

/**
 * Converts a compact patch-local vertex index directly into barycentric weights for the patch domain.
 */
void tess_barycentric_from_index(uint n, uint vertex_index, out float b0, out float b1, out float b2)
{
	uint row, col;
	tess_grid_from_index(n, vertex_index, row, col);
	tess_barycentric_from_grid(n, row, col, b0, b1, b2);
}

/**
 * Maps patch-local barycentric weights through a TessellationPatch domain triangle into source-triangle UV space.
 */
vec2 tess_patch_domain_uv(TessellationPatch tess_patch, float b0, float b1, float b2)
{
	return
		vec2(tess_patch.domain_u0, tess_patch.domain_v0) * b0 +
		vec2(tess_patch.domain_u1, tess_patch.domain_v1) * b1 +
		vec2(tess_patch.domain_u2, tess_patch.domain_v2) * b2;
}

/**
 * Converts source-triangle UV coordinates into barycentric weights for the original source triangle.
 */
void tess_source_barycentric_from_uv(vec2 uv, out float b0, out float b1, out float b2)
{
	b1 = uv.x;
	b2 = uv.y;
	b0 = 1.0 - b1 - b2;
}

/**
 * Interpolates a source-triangle position from UV coordinates in that triangle's barycentric domain.
 */
vec3 tess_source_position_from_uv(vec3 p0, vec3 p1, vec3 p2, vec2 uv)
{
	float b0, b1, b2;
	tess_source_barycentric_from_uv(uv, b0, b1, b2);
	return p0 * b0 + p1 * b1 + p2 * b2;
}

/**
 * Snaps a normalized edge coordinate to the nearest point on an edge divided into segment_count segments.
 */
float tess_snap_coord_to_segments(float coord, float segment_count)
{
	float safe_segment_count = max(segment_count, 1.0);
	return floor(clamp(coord, 0.0, 1.0) * safe_segment_count + 0.5) / safe_segment_count;
}

/**
 * Morphs an edge coordinate between neighboring integer segment grids for fractional edge LOD.
 * This lets boundary vertices slide toward the lower-density grid instead of popping between factors.
 */
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

/**
 * Provides a stable approximate ordering for positions so shared edges choose the same canonical direction.
 */
bool tess_position_less(vec3 a, vec3 b)
{
	const float epsilon = 0.000001;
	if (abs(a.x - b.x) > epsilon) { return a.x < b.x; }
	if (abs(a.y - b.y) > epsilon) { return a.y < b.y; }
	return a.z < b.z;
}

/**
 * Morphs an edge coordinate in a canonical orientation so neighboring patches agree on shared-edge vertex placement.
 */
float tess_morph_oriented_edge_coord(float coord, float edge_lod, uint patch_factor, vec3 edge_start, vec3 edge_end)
{
	bool forward_matches_canonical = !tess_position_less(edge_end, edge_start);
	float canonical_coord = forward_matches_canonical ? coord : 1.0 - coord;
	float morphed_coord = tess_morph_edge_coord(canonical_coord, edge_lod, patch_factor);
	return forward_matches_canonical ? morphed_coord : 1.0 - morphed_coord;
}

/**
 * Applies fractional edge LOD morphing to patch-boundary barycentric coordinates.
 * Interior vertices stay on the patch's full-resolution grid while edge vertices align to their edge LOD.
 */
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

/**
 * Projects a generated point onto the plane defined by a source vertex and normal for Phong tessellation smoothing.
 */
vec3 phong_project(vec3 p, vec3 source_position, vec3 source_normal)
{
	vec3 n = normalize(source_normal);
	return p - dot(p - source_position, n) * n;
}

/**
 * Displaces a generated vertex along its normal by the material's height map.
 *
 * The offset is applied in local space while displacement_scale is authored in
 * world units, so the model matrix's scale is divided out. Without this a
 * scaled object would displace by its scale factor, and non-uniformly scaled
 * objects would displace differently per axis.
 *
 * strength is the global scene-wide multiplier on top of the material's own
 * scale, letting the effect be tuned or switched off without editing materials.
 */
vec3 tess_apply_displacement(
	vec3 local_position,
	vec3 local_normal,
	vec2 texcoord,
	Material material,
	float object_scale,
	float strength)
{
	float height = material_sample_displacement(material, texcoord) * strength;
	if (height == 0.0)
	{
		return local_position;
	}
	return local_position
		+ normalize(local_normal) * (height / max(object_scale, 0.0001));
}

/**
 * Rebuilds a normal for a displaced vertex by finite-differencing the height
 * map across the surface tangent frame.
 *
 * Displacement that moves positions but leaves normals alone produces visible
 * geometry with flat lighting - the detail is in the silhouette but not in the
 * shading. Sampling two neighbor heights in UV space recovers the slope the
 * displacement introduced.
 *
 * texel_step is the UV distance to the neighbor samples; it should be about one
 * texel of the height map so the gradient matches the map's real frequency.
 */
vec3 tess_displaced_normal(
	vec3 local_normal,
	vec2 texcoord,
	vec3 tangent_u,
	vec3 tangent_v,
	Material material,
	float object_scale,
	float strength,
	float texel_step)
{
	if (!material_has_displacement(material) || strength == 0.0)
	{
		return normalize(local_normal);
	}

	float height_multiplier = strength / max(object_scale, 0.0001);
	float step_uv = max(texel_step, 0.0001);
	float height_center = material_sample_displacement(material, texcoord) * height_multiplier;
	float height_u = material_sample_displacement(
		material, texcoord + vec2(step_uv, 0.0)) * height_multiplier;
	float height_v = material_sample_displacement(
		material, texcoord + vec2(0.0, step_uv)) * height_multiplier;

	vec3 n = normalize(local_normal);
	// Degenerate UVs (zero-area in texture space) leave no frame to differentiate
	// against; keep the interpolated normal rather than producing garbage.
	if (dot(tangent_u, tangent_u) < 1e-12 || dot(tangent_v, tangent_v) < 1e-12)
	{
		return n;
	}

	// Walk along each tangent to the neighbor sample and lift by its height.
	// The cross product of the two resulting edges is the displaced normal.
	vec3 edge_u = tangent_u * step_uv + n * (height_u - height_center);
	vec3 edge_v = tangent_v * step_uv + n * (height_v - height_center);
	vec3 displaced = cross(edge_u, edge_v);
	if (dot(displaced, displaced) < 1e-16)
	{
		return n;
	}
	displaced = normalize(displaced);
	// Keep the original facing: cross order depends on UV winding, which can be
	// mirrored per triangle.
	return dot(displaced, n) < 0.0 ? -displaced : displaced;
}

/**
 * Surface tangents for a source triangle, i.e. the local-space directions that
 * one unit of U and one unit of V move along the surface.
 *
 * Solves the standard 2x2 UV-to-position system. Returns false when the
 * triangle has no area in UV space, leaving the outputs untouched.
 */
bool tess_source_tangents(
	vec3 p0, vec3 p1, vec3 p2,
	vec2 uv0, vec2 uv1, vec2 uv2,
	out vec3 tangent_u,
	out vec3 tangent_v)
{
	vec3 edge1 = p1 - p0;
	vec3 edge2 = p2 - p0;
	vec2 duv1 = uv1 - uv0;
	vec2 duv2 = uv2 - uv0;

	float determinant = duv1.x * duv2.y - duv2.x * duv1.y;
	if (abs(determinant) < 1e-12)
	{
		tangent_u = vec3(0.0);
		tangent_v = vec3(0.0);
		return false;
	}

	float inv_determinant = 1.0 / determinant;
	tangent_u = (edge1 * duv2.y - edge2 * duv1.y) * inv_determinant;
	tangent_v = (edge2 * duv1.x - edge1 * duv2.x) * inv_determinant;
	return true;
}

/**
 * Peak-to-trough displacement relief along a single edge, in world units.
 *
 * Deliberately a function of the edge's two endpoints only. Edge LODs decide
 * where boundary vertices land (tess_morph_oriented_edge_coord), so two patches
 * sharing an edge must compute the identical value or the shared boundary
 * cracks. Sampling the edge alone - rather than the triangle it belongs to -
 * is what makes the two agree.
 *
 * Direction-invariance is structural, not incidental: a neighbour traverses the
 * shared edge with uv_a/uv_b swapped, and mix() is not bitwise symmetric, so
 * individual tap coordinates DO differ by an ULP or so. It does not matter
 * because the tap parameters (0, .25, .5, .75, 1) are symmetric about 0.5 -
 * both directions visit the same point set - and min/max over a set is
 * order-free. Keep the taps symmetric and the endpoint count odd if this is
 * ever retuned; an asymmetric pattern would reintroduce cracks.
 */
float tess_displacement_relief_edge(Material material, vec2 uv_a, vec2 uv_b)
{
	if (!material_has_displacement(material))
	{
		return 0.0;
	}

	float lowest = 0.0;
	float highest = 0.0;
	// 5 evenly spaced taps including both endpoints: symmetric under a<->b.
	for (int tap_index = 0; tap_index < 5; ++tap_index)
	{
		float t = float(tap_index) * 0.25;
		float height = material_sample_displacement(material, mix(uv_a, uv_b, t));
		lowest = min(lowest, height);
		highest = max(highest, height);
	}
	return highest - lowest;
}

/**
 * Peak absolute displacement height across a source triangle, in world units.
 *
 * Used by the planner to decide how much tessellation a triangle deserves: a
 * triangle whose height map is flat gains nothing from subdivision regardless
 * of how large it is on screen, while one crossing a tall feature needs enough
 * vertices to actually resolve it. Samples a small fixed barycentric pattern -
 * cheap, and enough to catch whether any relief exists at all.
 *
 * Unlike the per-edge version this covers patch interiors, so it may only ever
 * raise tess_factor, never an edge LOD.
 */
float tess_displacement_relief(
	Material material,
	vec2 uv0,
	vec2 uv1,
	vec2 uv2)
{
	if (!material_has_displacement(material))
	{
		return 0.0;
	}

	// Corners, edge midpoints, and the centroid: 7 taps over the triangle.
	const vec3 barycentric_taps[7] = vec3[7](
		vec3(1.0, 0.0, 0.0),
		vec3(0.0, 1.0, 0.0),
		vec3(0.0, 0.0, 1.0),
		vec3(0.5, 0.5, 0.0),
		vec3(0.0, 0.5, 0.5),
		vec3(0.5, 0.0, 0.5),
		vec3(0.3333333, 0.3333333, 0.3333333));

	float lowest = 0.0;
	float highest = 0.0;
	for (int tap_index = 0; tap_index < 7; ++tap_index)
	{
		vec3 weights = barycentric_taps[tap_index];
		vec2 uv = uv0 * weights.x + uv1 * weights.y + uv2 * weights.z;
		float height = material_sample_displacement(material, uv);
		lowest = min(lowest, height);
		highest = max(highest, height);
	}
	// The spread matters, not the absolute offset: a triangle sitting on a
	// uniformly raised plateau is still flat and still needs no subdivision.
	return highest - lowest;
}

#endif

#undef TESS_INIT
#undef TESS_F32
#undef TESS_U32
#undef TESS_SHADER

#endif // GAME2_TESSELLATION_COMMON_H
