#pragma once

#include <cmath>
#include <cstddef>
#include <optional>

// Ankerl's Segmented Vector and Fast Unordered Hash Math
#include "ankerl/unordered_dense.h"
using ankerl::unordered_dense::map;

#include "tessellation.compiled.h"

#include "core/timings.h"
#include "render/sokol_helpers.h"
#include "state/state.h"

namespace Tessellation
{
	static constexpr u32 MAX_FACTOR = 31;
	static constexpr u32 MAX_COMPUTE_GROUPS_PER_DISPATCH = 65535;

	struct FactorLookupEntry
	{
		u32 factor = 1;
		u32 vertex_count = 3;
		u32 index_count = 3;
		u32 _padding0 = 0;
	};

	struct ComputeParams
	{
		i32 count = 0;
		i32 base_index = 0;
		f32 phong_strength = 0.0f;
		i32 _padding0 = 0;
	};

	struct QuantizedPosition
	{
		i32 x = 0;
		i32 y = 0;
		i32 z = 0;
	};

	struct EdgeKey
	{
		QuantizedPosition a;
		QuantizedPosition b;
	};

	struct EdgeRef
	{
		u32 patch_index = 0;
		u32 local_edge = 0;
		bool forward_matches_key = true;
		u32 tess_factor = 1;
	};

	struct PatchFactors
	{
		u32 tess_factor = 1;
		f32 edge_lods[3] = { 1.0f, 1.0f, 1.0f };
	};

	struct SourceBarycentric
	{
		f32 u = 0.0f;
		f32 v = 0.0f;
	};

	struct PatchDomain
	{
		SourceBarycentric corners[3] = {
			{ 0.0f, 0.0f },
			{ 1.0f, 0.0f },
			{ 0.0f, 1.0f },
		};
	};

	struct EdgeKeyHash
	{
		size_t operator()(const EdgeKey& key) const
		{
			size_t h = 1469598103934665603ull;
			auto mix = [&](i32 value)
			{
				h ^= (size_t) value;
				h *= 1099511628211ull;
			};
			mix(key.a.x);
			mix(key.a.y);
			mix(key.a.z);
			mix(key.b.x);
			mix(key.b.y);
			mix(key.b.z);
			return h;
		}
	};

	inline bool operator==(const QuantizedPosition& lhs, const QuantizedPosition& rhs)
	{
		return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
	}

	inline bool operator==(const EdgeKey& lhs, const EdgeKey& rhs)
	{
		return lhs.a == rhs.a && lhs.b == rhs.b;
	}

	std::optional<sg_shader> emit_vertices_shader;
	std::optional<sg_shader> emit_indices_shader;
	std::optional<sg_shader> weld_edges_shader;

	sg_pipeline emit_vertices_pipeline = {};
	sg_pipeline emit_indices_pipeline = {};
	sg_pipeline weld_edges_pipeline = {};

	FactorLookupEntry factor_lookup_entries[MAX_FACTOR + 1] = {};
	GpuBuffer<FactorLookupEntry> factor_lookup_buffer;
	bool initialized = false;

	u32 vertex_count_for_factor(u32 factor)
	{
		factor = CLAMP(factor, 1u, MAX_FACTOR);
		return ((factor + 1u) * (factor + 2u)) / 2u;
	}

	u32 index_count_for_factor(u32 factor)
	{
		factor = CLAMP(factor, 1u, MAX_FACTOR);
		return factor * factor * 3u;
	}

	u32 row_prefix(u32 factor, u32 row)
	{
		return row * (factor + 1u) - (row * (row - 1u)) / 2u;
	}

	u32 local_vertex_index(u32 factor, u32 row, u32 col)
	{
		return row_prefix(factor, row) + col;
	}

	u32 local_edge_vertex_index(u32 factor, u32 local_edge, u32 edge_t)
	{
		switch (local_edge)
		{
			case 0: return local_vertex_index(factor, 0, edge_t);
			case 1: return local_vertex_index(factor, edge_t, factor - edge_t);
			case 2: return local_vertex_index(factor, factor - edge_t, 0);
			default: assert(false); return 0;
		}
	}

	bool position_less(const QuantizedPosition& lhs, const QuantizedPosition& rhs)
	{
		if (lhs.x != rhs.x) { return lhs.x < rhs.x; }
		if (lhs.y != rhs.y) { return lhs.y < rhs.y; }
		return lhs.z < rhs.z;
	}

	QuantizedPosition quantize_position(HMM_Vec3 position)
	{
		const f32 scale = 10000.0f;
		return (QuantizedPosition) {
			.x = (i32) lroundf(position.X * scale),
			.y = (i32) lroundf(position.Y * scale),
			.z = (i32) lroundf(position.Z * scale),
		};
	}

	EdgeKey make_edge_key(const QuantizedPosition& a, const QuantizedPosition& b)
	{
		if (position_less(b, a))
		{
			return (EdgeKey) { .a = b, .b = a };
		}
		return (EdgeKey) { .a = a, .b = b };
	}

	HMM_Vec3 transform_position(const Transform& transform, HMM_Vec3 local_position)
	{
		local_position.X *= transform.scale.X;
		local_position.Y *= transform.scale.Y;
		local_position.Z *= transform.scale.Z;
		HMM_Vec3 rotated_position = HMM_RotateV3Q(local_position, transform.rotation);
		return rotated_position + transform.location.XYZ;
	}

	PatchDomain full_patch_domain()
	{
		return (PatchDomain) {};
	}

	PatchDomain patch_domain_from_patch(const TessellationPatch& patch)
	{
		PatchDomain domain = {};
		domain.corners[0] = (SourceBarycentric) { .u = patch.domain_u0, .v = patch.domain_v0 };
		domain.corners[1] = (SourceBarycentric) { .u = patch.domain_u1, .v = patch.domain_v1 };
		domain.corners[2] = (SourceBarycentric) { .u = patch.domain_u2, .v = patch.domain_v2 };
		return domain;
	}

	SourceBarycentric domain_grid_point(const u32 split_segments, const u32 row, const u32 col)
	{
		const f32 inv_segments = 1.0f / (f32) MAX(split_segments, 1u);
		return (SourceBarycentric) {
			.u = (f32) col * inv_segments,
			.v = (f32) row * inv_segments,
		};
	}

	PatchDomain virtual_patch_domain(const u32 split_segments, const u32 patch_index)
	{
		u32 row = 0;
		u32 tri_start = 0;
		for (u32 r = 0; r < split_segments; ++r)
		{
			const u32 row_tri_count = 2u * (split_segments - r) - 1u;
			if (patch_index < tri_start + row_tri_count)
			{
				row = r;
				break;
			}
			tri_start += row_tri_count;
		}

		const u32 tri_in_row = patch_index - tri_start;
		const u32 col = tri_in_row / 2u;
		const bool upper = (tri_in_row & 1u) == 0u;

		PatchDomain domain = {};
		if (upper)
		{
			domain.corners[0] = domain_grid_point(split_segments, row, col);
			domain.corners[1] = domain_grid_point(split_segments, row, col + 1u);
			domain.corners[2] = domain_grid_point(split_segments, row + 1u, col);
		}
		else
		{
			domain.corners[0] = domain_grid_point(split_segments, row, col + 1u);
			domain.corners[1] = domain_grid_point(split_segments, row + 1u, col + 1u);
			domain.corners[2] = domain_grid_point(split_segments, row + 1u, col);
		}
		return domain;
	}

	HMM_Vec3 source_triangle_local_position(const Mesh& mesh, const u32 source_index_offset, const SourceBarycentric barycentric)
	{
		const u32 i0 = mesh.indices[source_index_offset + 0];
		const u32 i1 = mesh.indices[source_index_offset + 1];
		const u32 i2 = mesh.indices[source_index_offset + 2];

		const f32 b1 = barycentric.u;
		const f32 b2 = barycentric.v;
		const f32 b0 = 1.0f - b1 - b2;

		const HMM_Vec3 p0 = mesh.vertices[i0].position.XYZ;
		const HMM_Vec3 p1 = mesh.vertices[i1].position.XYZ;
		const HMM_Vec3 p2 = mesh.vertices[i2].position.XYZ;
		return p0 * b0 + p1 * b1 + p2 * b2;
	}

	HMM_Vec3 patch_domain_local_position(const Mesh& mesh, const u32 source_index_offset, const PatchDomain& domain, const u32 corner_index)
	{
		return source_triangle_local_position(mesh, source_index_offset, domain.corners[corner_index]);
	}

	HMM_Vec3 patch_domain_world_position(const Object& object, const u32 source_index_offset, const PatchDomain& domain, const u32 corner_index)
	{
		return transform_position(object.current_transform, patch_domain_local_position(object.mesh, source_index_offset, domain, corner_index));
	}

	f32 raw_angular_lod_for_edge_at_distance(
		const f32 edge_length,
		const f32 distance_to_camera,
		const f32 fov_radians,
		const f32 render_height,
		const f32 target_pixels_per_segment)
	{
		const f32 safe_distance = fmaxf(distance_to_camera, 0.001f);
		const f32 sine_half_angle = CLAMP(edge_length / (2.0f * safe_distance), 0.0f, 1.0f);
		const f32 angular_length = 2.0f * asinf(sine_half_angle);
		const f32 pixel_length = (angular_length / fmaxf(fov_radians, 0.001f)) * render_height;
		const f32 target_pixels = fmaxf(target_pixels_per_segment, 1.0f);
		return fmaxf(pixel_length / target_pixels, 1.0f);
	}

	f32 raw_angular_lod_for_edge(
		const HMM_Vec3 a,
		const HMM_Vec3 b,
		const Camera& camera,
		const f32 fov_radians,
		const f32 render_height,
		const f32 target_pixels_per_segment)
	{
		HMM_Vec3 midpoint = (a + b) * 0.5f;
		const f32 distance_to_camera = fmaxf(HMM_LenV3(midpoint - camera.location), 0.001f);
		const f32 edge_length = HMM_LenV3(b - a);
		return raw_angular_lod_for_edge_at_distance(edge_length, distance_to_camera, fov_radians, render_height, target_pixels_per_segment);
	}

	f32 angular_lod_for_edge(
		const HMM_Vec3 a,
		const HMM_Vec3 b,
		const Camera& camera,
		const f32 fov_radians,
		const f32 render_height,
		const f32 target_pixels_per_segment,
		const u32 max_factor)
	{
		return CLAMP(raw_angular_lod_for_edge(a, b, camera, fov_radians, render_height, target_pixels_per_segment), 1.0f, (f32) max_factor);
	}

	u32 factor_for_lod(const f32 lod, const u32 max_factor)
	{
		return CLAMP((u32) ceilf(lod), 1u, max_factor);
	}

	HMM_Vec3 closest_point_on_triangle(const HMM_Vec3 point, const HMM_Vec3 a, const HMM_Vec3 b, const HMM_Vec3 c)
	{
		const HMM_Vec3 ab = b - a;
		const HMM_Vec3 ac = c - a;
		const HMM_Vec3 ap = point - a;
		const f32 d1 = HMM_DotV3(ab, ap);
		const f32 d2 = HMM_DotV3(ac, ap);
		if (d1 <= 0.0f && d2 <= 0.0f) { return a; }

		const HMM_Vec3 bp = point - b;
		const f32 d3 = HMM_DotV3(ab, bp);
		const f32 d4 = HMM_DotV3(ac, bp);
		if (d3 >= 0.0f && d4 <= d3) { return b; }

		const f32 vc = d1 * d4 - d3 * d2;
		if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
		{
			const f32 v = d1 / (d1 - d3);
			return a + ab * v;
		}

		const HMM_Vec3 cp = point - c;
		const f32 d5 = HMM_DotV3(ab, cp);
		const f32 d6 = HMM_DotV3(ac, cp);
		if (d6 >= 0.0f && d5 <= d6) { return c; }

		const f32 vb = d5 * d2 - d1 * d6;
		if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
		{
			const f32 w = d2 / (d2 - d6);
			return a + ac * w;
		}

		const f32 va = d3 * d6 - d5 * d4;
		if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
		{
			const f32 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
			return b + (c - b) * w;
		}

		const f32 denom = 1.0f / fmaxf(va + vb + vc, 0.000001f);
		const f32 v = vb * denom;
		const f32 w = vc * denom;
		return a + ab * v + ac * w;
	}

	u32 next_power_of_two(const u32 value)
	{
		u32 result = 1;
		while (result < value)
		{
			result <<= 1u;
		}
		return result;
	}

	f32 raw_triangle_split_lod(
		const Object& object,
		const State& state,
		const Camera& camera,
		const f32 fov_radians,
		const u32 source_index_offset)
	{
		const Mesh& mesh = object.mesh;
		const u32 i0 = mesh.indices[source_index_offset + 0];
		const u32 i1 = mesh.indices[source_index_offset + 1];
		const u32 i2 = mesh.indices[source_index_offset + 2];

		const HMM_Vec3 p0 = transform_position(object.current_transform, mesh.vertices[i0].position.XYZ);
		const HMM_Vec3 p1 = transform_position(object.current_transform, mesh.vertices[i1].position.XYZ);
		const HMM_Vec3 p2 = transform_position(object.current_transform, mesh.vertices[i2].position.XYZ);
		const f32 render_height = (f32) state.window.render_height;

		const f32 edge_lod = fmaxf(
			raw_angular_lod_for_edge(p0, p1, camera, fov_radians, render_height, state.tessellation.target_pixels_per_segment),
			fmaxf(
				raw_angular_lod_for_edge(p1, p2, camera, fov_radians, render_height, state.tessellation.target_pixels_per_segment),
				raw_angular_lod_for_edge(p2, p0, camera, fov_radians, render_height, state.tessellation.target_pixels_per_segment)));

		const HMM_Vec3 closest_point = closest_point_on_triangle(camera.location, p0, p1, p2);
		const f32 closest_distance = HMM_LenV3(closest_point - camera.location);
		const f32 max_edge_length = fmaxf(HMM_LenV3(p1 - p0), fmaxf(HMM_LenV3(p2 - p1), HMM_LenV3(p0 - p2)));
		const f32 closest_lod = raw_angular_lod_for_edge_at_distance(
			max_edge_length,
			closest_distance,
			fov_radians,
			render_height,
			state.tessellation.target_pixels_per_segment);

		return fmaxf(edge_lod, closest_lod);
	}

	u32 virtual_patch_split_segments(
		const Object& object,
		const State& state,
		const Camera& camera,
		const f32 fov_radians,
		const u32 source_index_offset)
	{
		if (state.tessellation.mode != ETessellationMode::AdaptiveAngularPerTriangle || !state.tessellation.virtual_patches_enabled)
		{
			return 1;
		}

		const u32 max_depth = (u32) CLAMP(state.tessellation.virtual_patch_max_depth, 0, 4);
		if (max_depth == 0)
		{
			return 1;
		}

		const u32 max_factor = CLAMP((u32) state.tessellation.max_factor, 1u, MAX_FACTOR);
		const f32 raw_lod = raw_triangle_split_lod(object, state, camera, fov_radians, source_index_offset);
		const u32 requested_segments = MAX((u32) ceilf(raw_lod / (f32) max_factor), 1u);
		const u32 max_segments = 1u << max_depth;
		return MIN(next_power_of_two(requested_segments), max_segments);
	}

	PatchFactors uniform_patch_factors(const u32 factor)
	{
		PatchFactors factors = {
			.tess_factor = factor,
			.edge_lods = { (f32) factor, (f32) factor, (f32) factor },
		};
		return factors;
	}

	PatchFactors domain_tessellation_factors(
		const Object& object,
		const State& state,
		const Camera& camera,
		const f32 fov_radians,
		const u32 source_index_offset,
		const PatchDomain& domain)
	{
		const u32 max_factor = CLAMP((u32) state.tessellation.max_factor, 1u, MAX_FACTOR);
		const f32 render_height = (f32) state.window.render_height;
		const HMM_Vec3 p0 = patch_domain_world_position(object, source_index_offset, domain, 0);
		const HMM_Vec3 p1 = patch_domain_world_position(object, source_index_offset, domain, 1);
		const HMM_Vec3 p2 = patch_domain_world_position(object, source_index_offset, domain, 2);

		PatchFactors factors = {};
		factors.edge_lods[0] = angular_lod_for_edge(p0, p1, camera, fov_radians, render_height, state.tessellation.target_pixels_per_segment, max_factor);
		factors.edge_lods[1] = angular_lod_for_edge(p1, p2, camera, fov_radians, render_height, state.tessellation.target_pixels_per_segment, max_factor);
		factors.edge_lods[2] = angular_lod_for_edge(p2, p0, camera, fov_radians, render_height, state.tessellation.target_pixels_per_segment, max_factor);
		const f32 max_edge_lod = fmaxf(factors.edge_lods[0], fmaxf(factors.edge_lods[1], factors.edge_lods[2]));
		factors.tess_factor = factor_for_lod(max_edge_lod, max_factor);
		return factors;
	}

    // FCS TODO: this looks expensive. Should either use some other heuristic or do in a compute shader
	PatchFactors object_tessellation_factors(
		const Object& object,
		const State& state,
		const Camera& camera,
		const f32 fov_radians)
	{
		const u32 max_factor = CLAMP((u32) state.tessellation.max_factor, 1u, MAX_FACTOR);
		if (state.tessellation.mode == ETessellationMode::Fixed)
		{
			return uniform_patch_factors(CLAMP((u32) state.tessellation.fixed_factor, 1u, max_factor));
		}

		const Mesh& mesh = object.mesh;
		const f32 render_height = (f32) state.window.render_height;
		f32 max_lod = 1.0f;
		for (u32 index_idx = 0; index_idx + 2 < mesh.index_count; index_idx += 3)
		{
			const u32 i0 = mesh.indices[index_idx + 0];
			const u32 i1 = mesh.indices[index_idx + 1];
			const u32 i2 = mesh.indices[index_idx + 2];

			const HMM_Vec3 p0 = transform_position(object.current_transform, mesh.vertices[i0].position.XYZ);
			const HMM_Vec3 p1 = transform_position(object.current_transform, mesh.vertices[i1].position.XYZ);
			const HMM_Vec3 p2 = transform_position(object.current_transform, mesh.vertices[i2].position.XYZ);

			max_lod = fmaxf(max_lod, angular_lod_for_edge(p0, p1, camera, fov_radians, render_height, state.tessellation.target_pixels_per_segment, max_factor));
			max_lod = fmaxf(max_lod, angular_lod_for_edge(p1, p2, camera, fov_radians, render_height, state.tessellation.target_pixels_per_segment, max_factor));
			max_lod = fmaxf(max_lod, angular_lod_for_edge(p2, p0, camera, fov_radians, render_height, state.tessellation.target_pixels_per_segment, max_factor));
		}
		return uniform_patch_factors(factor_for_lod(max_lod, max_factor));
	}

	void init()
	{
		if (initialized)
		{
			return;
		}

		for (u32 factor = 1; factor <= MAX_FACTOR; ++factor)
		{
			factor_lookup_entries[factor] = (FactorLookupEntry) {
				.factor = factor,
				.vertex_count = vertex_count_for_factor(factor),
				.index_count = index_count_for_factor(factor),
			};
		}

		factor_lookup_buffer = GpuBuffer((GpuBufferDesc<FactorLookupEntry>) {
			.data = factor_lookup_entries,
			.size = sizeof(FactorLookupEntry) * (MAX_FACTOR + 1),
			.usage = {
				.storage_buffer = true,
			},
			.label = "Tessellation::factor_lookup_buffer",
		});
		factor_lookup_buffer.get_storage_view();

		emit_vertices_shader = sg_make_shader(tessellation_emit_vertices_shader_desc(sg_query_backend()));
		emit_indices_shader = sg_make_shader(tessellation_emit_indices_shader_desc(sg_query_backend()));
		weld_edges_shader = sg_make_shader(tessellation_weld_edges_shader_desc(sg_query_backend()));

		emit_vertices_pipeline = sg_make_pipeline((sg_pipeline_desc) {
			.compute = true,
			.shader = emit_vertices_shader.value(),
			.label = "tessellation-emit-vertices-pipeline",
		});
		emit_indices_pipeline = sg_make_pipeline((sg_pipeline_desc) {
			.compute = true,
			.shader = emit_indices_shader.value(),
			.label = "tessellation-emit-indices-pipeline",
		});
		weld_edges_pipeline = sg_make_pipeline((sg_pipeline_desc) {
			.compute = true,
			.shader = weld_edges_shader.value(),
			.label = "tessellation-weld-edges-pipeline",
		});

		initialized = true;
	}

	void reset_stats(State& state)
	{
		state.tessellation.source_triangle_count = 0;
		state.tessellation.patch_count = 0;
		state.tessellation.generated_vertex_count = 0;
		state.tessellation.generated_index_count = 0;
		state.tessellation.edge_weld_pair_count = 0;
		state.tessellation.mesh_count = 0;
		state.tessellation.overflowed_mesh_count = 0;
		state.tessellation.max_factor_seen = 1;
	}

	void disable_all_meshes(State& state)
	{
		for (auto& [unique_id, object] : state.scene.objects)
		{
			if (object.has_mesh)
			{
				object.mesh.tessellated_geometry.active = false;
				object.mesh.tessellated_geometry.overflowed = false;
			}
		}
	}

	void ensure_patch_buffer(TessellatedGeometry& tessellated)
	{
		if (tessellated.patch_capacity >= tessellated.patch_count)
		{
			return;
		}

		tessellated.patch_buffer.cleanup();
		tessellated.patch_capacity = tessellated.patch_count;
		tessellated.patch_buffer = GpuBuffer((GpuBufferDesc<TessellationPatch>) {
			.data = nullptr,
			.size = sizeof(TessellationPatch) * tessellated.patch_capacity,
			.usage = {
				.storage_buffer = true,
				.stream_update = true,
			},
			.label = "TessellatedGeometry::patch_buffer",
		});
	}

	void ensure_edge_weld_pair_buffer(TessellatedGeometry& tessellated)
	{
		if (tessellated.edge_weld_pair_count == 0 || tessellated.edge_weld_pair_capacity >= tessellated.edge_weld_pair_count)
		{
			return;
		}

		tessellated.edge_weld_pair_buffer.cleanup();
		tessellated.edge_weld_pair_capacity = tessellated.edge_weld_pair_count;
		tessellated.edge_weld_pair_buffer = GpuBuffer((GpuBufferDesc<TessellationWeldPair>) {
			.data = nullptr,
			.size = sizeof(TessellationWeldPair) * tessellated.edge_weld_pair_capacity,
			.usage = {
				.storage_buffer = true,
				.stream_update = true,
			},
			.label = "TessellatedGeometry::edge_weld_pair_buffer",
		});
	}

	void ensure_generated_buffers(TessellatedGeometry& tessellated)
	{
		if (tessellated.vertex_capacity < tessellated.vertex_count)
		{
			tessellated.vertex_buffer.cleanup();
			tessellated.vertex_capacity = tessellated.vertex_count;
			tessellated.vertex_buffer = GpuBuffer((GpuBufferDesc<Vertex>) {
				.data = nullptr,
				.size = sizeof(Vertex) * tessellated.vertex_capacity,
				.usage = {
					.vertex_buffer = true,
					.storage_buffer = true,
				},
				.label = "TessellatedGeometry::vertex_buffer",
			});
		}

		if (tessellated.index_capacity < tessellated.index_count)
		{
			tessellated.index_buffer.cleanup();
			tessellated.index_capacity = tessellated.index_count;
			tessellated.index_buffer = GpuBuffer((GpuBufferDesc<u32>) {
				.data = nullptr,
				.size = sizeof(u32) * tessellated.index_capacity,
				.usage = {
					.index_buffer = true,
					.storage_buffer = true,
				},
				.label = "TessellatedGeometry::index_buffer",
			});
		}

		if (tessellated.wire_index_capacity < tessellated.wire_index_count)
		{
			tessellated.wire_index_buffer.cleanup();
			tessellated.wire_index_capacity = tessellated.wire_index_count;
			tessellated.wire_index_buffer = GpuBuffer((GpuBufferDesc<u32>) {
				.data = nullptr,
				.size = sizeof(u32) * tessellated.wire_index_capacity,
				.usage = {
					.index_buffer = true,
					.storage_buffer = true,
				},
				.label = "TessellatedGeometry::wire_index_buffer",
			});
		}
	}

	void build_edge_weld_pairs(Mesh& mesh)
	{
		TessellatedGeometry& tessellated = mesh.tessellated_geometry;
		tessellated.edge_weld_pairs.reset();

		map<EdgeKey, EdgeRef, EdgeKeyHash> edge_map;
		edge_map.reserve(mesh.index_count);

		for (u32 patch_index = 0; patch_index < tessellated.patch_count; ++patch_index)
		{
			const TessellationPatch& patch = tessellated.patches[patch_index];
			const u32 factor = patch.tess_factor;
			if (factor <= 1)
			{
				continue;
			}

			const u32 source_index_offset = patch.source_index_offset;
			const PatchDomain domain = patch_domain_from_patch(patch);
			const HMM_Vec3 edge_positions[3] = {
				patch_domain_local_position(mesh, source_index_offset, domain, 0),
				patch_domain_local_position(mesh, source_index_offset, domain, 1),
				patch_domain_local_position(mesh, source_index_offset, domain, 2),
			};

			for (u32 local_edge = 0; local_edge < 3; ++local_edge)
			{
				const HMM_Vec3 edge_start = edge_positions[local_edge];
				const HMM_Vec3 edge_end = edge_positions[(local_edge + 1u) % 3u];
				const QuantizedPosition qa = quantize_position(edge_start);
				const QuantizedPosition qb = quantize_position(edge_end);
				if (qa == qb)
				{
					continue;
				}

				const EdgeKey key = make_edge_key(qa, qb);
				const bool forward_matches_key = (key.a == qa);
				auto found_edge = edge_map.find(key);
				if (found_edge == edge_map.end())
				{
					edge_map[key] = (EdgeRef) {
						.patch_index = patch_index,
						.local_edge = local_edge,
						.forward_matches_key = forward_matches_key,
						.tess_factor = patch.tess_factor,
					};
					continue;
				}

				const EdgeRef& other_edge = found_edge->second;
				if (other_edge.tess_factor != patch.tess_factor)
				{
					continue;
				}

				const TessellationPatch& other_patch = tessellated.patches[other_edge.patch_index];

                // Setup weld pairs for all corresponding vertices along the edge
				for (u32 canonical_t = 1; canonical_t < factor; ++canonical_t)
				{
					const u32 other_t = other_edge.forward_matches_key ? canonical_t : factor - canonical_t;
					const u32 current_t = forward_matches_key ? canonical_t : factor - canonical_t;
					const u32 other_vertex =
						other_patch.generated_vertex_offset +
						local_edge_vertex_index(factor, other_edge.local_edge, other_t);
					const u32 current_vertex =
						patch.generated_vertex_offset +
						local_edge_vertex_index(factor, local_edge, current_t);

					tessellated.edge_weld_pairs.add((TessellationWeldPair) {
						.vertex_index_a = other_vertex,
						.vertex_index_b = current_vertex,
					});
				}
			}
		}

		tessellated.edge_weld_pair_count = (u32) tessellated.edge_weld_pairs.length();
	}

	bool prepare_mesh(
		State& state,
		Object& object,
		const Camera& camera,
		const f32 fov_radians,
		const u32 remaining_vertex_budget,
		const u32 remaining_index_budget)
	{
		Mesh& mesh = object.mesh;
		TessellatedGeometry& tessellated = mesh.tessellated_geometry;

		tessellated.active = false;
		tessellated.overflowed = false;
		tessellated.patches.reset();
		tessellated.patch_count = 0;
		tessellated.vertex_count = 0;
		tessellated.index_count = 0;
		tessellated.wire_index_count = 0;
		tessellated.edge_weld_pair_count = 0;

		if (mesh.index_count < 3 || mesh.vertex_count == 0)
		{
			return false;
		}

		const u32 source_triangle_count = mesh.index_count / 3;
		state.tessellation.source_triangle_count += source_triangle_count;

		const bool use_per_triangle_factors = state.tessellation.mode == ETessellationMode::AdaptiveAngularPerTriangle;
		const PatchFactors mesh_factors = use_per_triangle_factors
			? PatchFactors {}
			: object_tessellation_factors(object, state, camera, fov_radians);

		u64 generated_vertex_count = 0;
		u64 generated_index_count = 0;
		u32 vertex_offset = 0;
		u32 index_offset = 0;
		bool overflowed = false;
		for (u32 tri_idx = 0; tri_idx < source_triangle_count; ++tri_idx)
		{
			const u32 source_index_offset = tri_idx * 3u;
			const u32 split_segments = use_per_triangle_factors
				? virtual_patch_split_segments(object, state, camera, fov_radians, source_index_offset)
				: 1u;
			const u32 virtual_patch_count = split_segments * split_segments;

			for (u32 virtual_patch_index = 0; virtual_patch_index < virtual_patch_count; ++virtual_patch_index)
			{
				const PatchDomain domain = split_segments > 1u
					? virtual_patch_domain(split_segments, virtual_patch_index)
					: full_patch_domain();
				const PatchFactors patch_factors = use_per_triangle_factors
					? domain_tessellation_factors(object, state, camera, fov_radians, source_index_offset, domain)
					: mesh_factors;
				const u32 vertices_per_patch = vertex_count_for_factor(patch_factors.tess_factor);
				const u32 indices_per_patch = index_count_for_factor(patch_factors.tess_factor);

				state.tessellation.max_factor_seen = MAX(state.tessellation.max_factor_seen, (i32) patch_factors.tess_factor);

				tessellated.patches.add((TessellationPatch) {
					.source_index_offset = source_index_offset,
					.source_triangle_index = tri_idx,
					.generated_vertex_offset = vertex_offset,
					.generated_index_offset = index_offset,
					.tess_factor = patch_factors.tess_factor,
					.vertex_count = vertices_per_patch,
					.index_count = indices_per_patch,
					.edge_lod0 = patch_factors.edge_lods[0],
					.edge_lod1 = patch_factors.edge_lods[1],
					.edge_lod2 = patch_factors.edge_lods[2],
					.domain_u0 = domain.corners[0].u,
					.domain_v0 = domain.corners[0].v,
					.domain_u1 = domain.corners[1].u,
					.domain_v1 = domain.corners[1].v,
					.domain_u2 = domain.corners[2].u,
					.domain_v2 = domain.corners[2].v,
				});

				generated_vertex_count += vertices_per_patch;
				generated_index_count += indices_per_patch;
				vertex_offset += vertices_per_patch;
				index_offset += indices_per_patch;

				if (generated_vertex_count > remaining_vertex_budget ||
					generated_index_count > remaining_index_budget ||
					generated_index_count * 2ull > (u64) state.tessellation.max_generated_indices * 2ull)
				{
					overflowed = true;
					break;
				}
			}

			if (overflowed)
			{
				break;
			}
		}

		tessellated.patch_count = (u32) tessellated.patches.length();
		const u64 generated_wire_index_count = generated_index_count * 2ull;

		if (overflowed ||
			generated_vertex_count == 0 || generated_index_count == 0 ||
			generated_vertex_count > remaining_vertex_budget ||
			generated_index_count > remaining_index_budget ||
			generated_wire_index_count > (u64) state.tessellation.max_generated_indices * 2u)
		{
			tessellated.overflowed = true;
			tessellated.patches.reset();
			tessellated.patch_count = 0;
			tessellated.vertex_count = 0;
			tessellated.index_count = 0;
			tessellated.wire_index_count = 0;
			state.tessellation.overflowed_mesh_count += 1;
			return false;
		}

		tessellated.vertex_count = (u32) generated_vertex_count;
		tessellated.index_count = (u32) generated_index_count;
		tessellated.wire_index_count = (u32) generated_wire_index_count;

		build_edge_weld_pairs(mesh);

		ensure_patch_buffer(tessellated);
		ensure_generated_buffers(tessellated);
		ensure_edge_weld_pair_buffer(tessellated);

		tessellated.patch_buffer.update_gpu_buffer((sg_range) {
			.ptr = tessellated.patches.data(),
			.size = sizeof(TessellationPatch) * tessellated.patch_count,
		});

		if (tessellated.edge_weld_pair_count > 0)
		{
			tessellated.edge_weld_pair_buffer.update_gpu_buffer((sg_range) {
				.ptr = tessellated.edge_weld_pairs.data(),
				.size = sizeof(TessellationWeldPair) * tessellated.edge_weld_pair_count,
			});
		}

		state.tessellation.patch_count += tessellated.patch_count;
		state.tessellation.generated_vertex_count += tessellated.vertex_count;
		state.tessellation.generated_index_count += tessellated.index_count;
		state.tessellation.edge_weld_pair_count += tessellated.edge_weld_pair_count;
		state.tessellation.mesh_count += 1;

		return true;
	}

	void dispatch_patch_pipeline(
		const sg_pipeline pipeline,
		const char* debug_label,
		const u32 patch_count,
		const f32 phong_strength,
		const sg_bindings& bindings)
	{
		for (u32 base_patch = 0; base_patch < patch_count; base_patch += MAX_COMPUTE_GROUPS_PER_DISPATCH)
		{
			const u32 dispatch_count = MIN(MAX_COMPUTE_GROUPS_PER_DISPATCH, patch_count - base_patch);
			ComputeParams params = {
				.count = (i32) patch_count,
				.base_index = (i32) base_patch,
				.phong_strength = phong_strength,
			};

			{
				CPU_TIMING_BACKEND_SCOPE("sg_begin_pass", debug_label);
				sg_begin_pass((sg_pass) { .compute = true, .label = debug_label });
			}
			{
				GpuDebugScope debug_scope(debug_label);
				sg_apply_pipeline(pipeline);
				sg_apply_uniforms(0, SG_RANGE(params));
				gpu_apply_bindings(&bindings);
				sg_dispatch((i32) dispatch_count, 1, 1);
			}
			{
				CPU_TIMING_BACKEND_SCOPE("sg_end_pass", debug_label);
				sg_end_pass();
			}
		}
	}

	void dispatch_edge_weld(Mesh& mesh)
	{
		TessellatedGeometry& tessellated = mesh.tessellated_geometry;
		if (tessellated.edge_weld_pair_count == 0)
		{
			return;
		}

		const u32 workgroup_size = 64;
		for (u32 base_pair = 0; base_pair < tessellated.edge_weld_pair_count; base_pair += MAX_COMPUTE_GROUPS_PER_DISPATCH * workgroup_size)
		{
			const u32 pair_count = tessellated.edge_weld_pair_count - base_pair;
			const u32 dispatch_pair_count = MIN(pair_count, MAX_COMPUTE_GROUPS_PER_DISPATCH * workgroup_size);
			const u32 group_count = (dispatch_pair_count + workgroup_size - 1u) / workgroup_size;
			ComputeParams params = {
				.count = (i32) tessellated.edge_weld_pair_count,
				.base_index = (i32) base_pair,
			};

			sg_bindings bindings = {
				.views = {
					[0] = tessellated.vertex_buffer.get_storage_view(),
					[1] = tessellated.edge_weld_pair_buffer.get_storage_view(),
				},
			};

			const char* debug_label = "Tessellation Weld Edges";
			{
				CPU_TIMING_BACKEND_SCOPE("sg_begin_pass", debug_label);
				sg_begin_pass((sg_pass) { .compute = true, .label = debug_label });
			}
			{
				GpuDebugScope debug_scope(debug_label);
				sg_apply_pipeline(weld_edges_pipeline);
				sg_apply_uniforms(0, SG_RANGE(params));
				gpu_apply_bindings(&bindings);
				sg_dispatch((i32) group_count, 1, 1);
			}
			{
				CPU_TIMING_BACKEND_SCOPE("sg_end_pass", debug_label);
				sg_end_pass();
			}
		}
	}

	void emit_mesh(State& state, Mesh& mesh)
	{
		TessellatedGeometry& tessellated = mesh.tessellated_geometry;
		if (tessellated.patch_count == 0 || tessellated.vertex_count == 0 || tessellated.index_count == 0)
		{
			return;
		}

		{
			sg_bindings bindings = {
				.views = {
					[0] = mesh.vertex_buffer.get_storage_view(),
					[1] = mesh.index_buffer.get_storage_view(),
					[2] = tessellated.patch_buffer.get_storage_view(),
					[3] = tessellated.vertex_buffer.get_storage_view(),
				},
			};
			dispatch_patch_pipeline(emit_vertices_pipeline, "Tessellation Emit Vertices", tessellated.patch_count, state.tessellation.phong_strength, bindings);
		}

		{
			sg_bindings bindings = {
				.views = {
					[0] = tessellated.patch_buffer.get_storage_view(),
					[1] = tessellated.index_buffer.get_storage_view(),
					[2] = tessellated.wire_index_buffer.get_storage_view(),
				},
			};
			dispatch_patch_pipeline(emit_indices_pipeline, "Tessellation Emit Indices", tessellated.patch_count, state.tessellation.phong_strength, bindings);
		}

		if (state.tessellation.edge_welding)
		{
			dispatch_edge_weld(mesh);
		}

		tessellated.active = true;
	}

	void update(State& state, const Camera& camera, const f32 fov_radians)
	{
		reset_stats(state);
		state.tessellation.max_factor = CLAMP(state.tessellation.max_factor, 1, (i32) MAX_FACTOR);
		state.tessellation.fixed_factor = CLAMP(state.tessellation.fixed_factor, 1, state.tessellation.max_factor);
		state.tessellation.target_pixels_per_segment = fmaxf(state.tessellation.target_pixels_per_segment, 1.0f);
		state.tessellation.phong_strength = CLAMP(state.tessellation.phong_strength, 0.0f, 1.0f);
		state.tessellation.virtual_patch_max_depth = CLAMP(state.tessellation.virtual_patch_max_depth, 0, 4);
		state.tessellation.max_generated_vertices = MAX(state.tessellation.max_generated_vertices, 3);
		state.tessellation.max_generated_indices = MAX(state.tessellation.max_generated_indices, 3);
		state.tessellation.bounds_padding = fmaxf(state.tessellation.bounds_padding, 0.0f);

		if (!state.tessellation.enabled)
		{
			disable_all_meshes(state);
			return;
		}

		init();

		for (auto& [unique_id, object] : state.scene.objects)
		{
			if (!object.has_mesh)
			{
				continue;
			}

			if (object.mesh.has_skinned_vertices)
			{
				mesh_cleanup_tessellated_geometry(object.mesh);
				continue;
			}

			const u32 used_vertices = (u32) state.tessellation.generated_vertex_count;
			const u32 used_indices = (u32) state.tessellation.generated_index_count;
			const u32 max_vertices = (u32) state.tessellation.max_generated_vertices;
			const u32 max_indices = (u32) state.tessellation.max_generated_indices;
			const u32 remaining_vertices = used_vertices < max_vertices ? max_vertices - used_vertices : 0;
			const u32 remaining_indices = used_indices < max_indices ? max_indices - used_indices : 0;

			if (prepare_mesh(state, object, camera, fov_radians, remaining_vertices, remaining_indices))
			{
				emit_mesh(state, object.mesh);
			}
		}
	}
}
