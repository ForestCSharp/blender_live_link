#pragma once

#include <cmath>
#include <cstddef>
#include <optional>

// Ankerl's Segmented Vector and Fast Unordered Hash Math
#include "ankerl/unordered_dense.h"
using ankerl::unordered_dense::map;

#include "tessellation.compiled.h"

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

	u32 angular_factor_for_edge(
		const HMM_Vec3 a,
		const HMM_Vec3 b,
		const Camera& camera,
		const f32 fov_radians,
		const f32 render_height,
		const f32 target_pixels_per_segment,
		const u32 max_factor)
	{
		HMM_Vec3 midpoint = (a + b) * 0.5f;
		const f32 distance_to_camera = fmaxf(HMM_LenV3(midpoint - camera.location), 0.001f);
		const f32 edge_length = HMM_LenV3(b - a);
		const f32 sine_half_angle = CLAMP(edge_length / (2.0f * distance_to_camera), 0.0f, 1.0f);
		const f32 angular_length = 2.0f * asinf(sine_half_angle);
		const f32 pixel_length = (angular_length / fmaxf(fov_radians, 0.001f)) * render_height;
		const f32 target_pixels = fmaxf(target_pixels_per_segment, 1.0f);
		const u32 factor = (u32) ceilf(pixel_length / target_pixels);
		return CLAMP(factor, 1u, max_factor);
	}

    // FCS TODO: this looks expensive. Should either use some other heuristic or do in a compute shader
	u32 object_tessellation_factor(
		const Object& object,
		const State& state,
		const Camera& camera,
		const f32 fov_radians)
	{
		const u32 max_factor = CLAMP((u32) state.tessellation.max_factor, 1u, MAX_FACTOR);
		if (state.tessellation.mode == ETessellationMode::Fixed)
		{
			return CLAMP((u32) state.tessellation.fixed_factor, 1u, max_factor);
		}

		// V1 keeps one factor per object so every generated triangle edge uses
		// matching subdivisions. The factor is still camera-adaptive: it is the
		// maximum angular edge factor observed in the source mesh this frame.
		const Mesh& mesh = object.mesh;
		const f32 render_height = (f32) state.window.render_height;
		u32 factor = 1;
		for (u32 index_idx = 0; index_idx + 2 < mesh.index_count; index_idx += 3)
		{
			const u32 i0 = mesh.indices[index_idx + 0];
			const u32 i1 = mesh.indices[index_idx + 1];
			const u32 i2 = mesh.indices[index_idx + 2];

			const HMM_Vec3 p0 = transform_position(object.current_transform, mesh.vertices[i0].position.XYZ);
			const HMM_Vec3 p1 = transform_position(object.current_transform, mesh.vertices[i1].position.XYZ);
			const HMM_Vec3 p2 = transform_position(object.current_transform, mesh.vertices[i2].position.XYZ);

			factor = MAX(factor, angular_factor_for_edge(p0, p1, camera, fov_radians, render_height, state.tessellation.target_pixels_per_segment, max_factor));
			factor = MAX(factor, angular_factor_for_edge(p1, p2, camera, fov_radians, render_height, state.tessellation.target_pixels_per_segment, max_factor));
			factor = MAX(factor, angular_factor_for_edge(p2, p0, camera, fov_radians, render_height, state.tessellation.target_pixels_per_segment, max_factor));
		}
		return CLAMP(factor, 1u, max_factor);
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

	void build_edge_weld_pairs(Mesh& mesh, const u32 factor)
	{
		TessellatedGeometry& tessellated = mesh.tessellated_geometry;
		tessellated.edge_weld_pairs.reset();

		if (factor <= 1)
		{
			tessellated.edge_weld_pair_count = 0;
			return;
		}

		map<EdgeKey, EdgeRef, EdgeKeyHash> edge_map;
		edge_map.reserve(mesh.index_count);

		for (u32 patch_index = 0; patch_index < tessellated.patch_count; ++patch_index)
		{
			const TessellationPatch& patch = tessellated.patches[patch_index];
			const u32 source_index_offset = patch.source_index_offset;
			const u32 source_indices[3] = {
				mesh.indices[source_index_offset + 0],
				mesh.indices[source_index_offset + 1],
				mesh.indices[source_index_offset + 2],
			};

			const u32 edge_vertices[3][2] = {
				{ source_indices[0], source_indices[1] },
				{ source_indices[1], source_indices[2] },
				{ source_indices[2], source_indices[0] },
			};

			for (u32 local_edge = 0; local_edge < 3; ++local_edge)
			{
				const HMM_Vec3 edge_start = mesh.vertices[edge_vertices[local_edge][0]].position.XYZ;
				const HMM_Vec3 edge_end = mesh.vertices[edge_vertices[local_edge][1]].position.XYZ;
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
						.tess_factor = factor,
					};
					continue;
				}

				const EdgeRef& other_edge = found_edge->second;
				if (other_edge.tess_factor != factor)
				{
					continue;
				}

				const TessellationPatch& other_patch = tessellated.patches[other_edge.patch_index];
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
		const u32 factor = object_tessellation_factor(object, state, camera, fov_radians);
		const u32 vertices_per_patch = vertex_count_for_factor(factor);
		const u32 indices_per_patch = index_count_for_factor(factor);
		const u64 generated_vertex_count = (u64) source_triangle_count * (u64) vertices_per_patch;
		const u64 generated_index_count = (u64) source_triangle_count * (u64) indices_per_patch;
		const u64 generated_wire_index_count = generated_index_count * 2u;

		state.tessellation.source_triangle_count += source_triangle_count;
		state.tessellation.max_factor_seen = MAX(state.tessellation.max_factor_seen, (i32) factor);

		if (generated_vertex_count == 0 || generated_index_count == 0 ||
			generated_vertex_count > remaining_vertex_budget ||
			generated_index_count > remaining_index_budget ||
			generated_wire_index_count > (u64) state.tessellation.max_generated_indices * 2u)
		{
			tessellated.overflowed = true;
			state.tessellation.overflowed_mesh_count += 1;
			return false;
		}

		tessellated.patch_count = source_triangle_count;
		tessellated.vertex_count = (u32) generated_vertex_count;
		tessellated.index_count = (u32) generated_index_count;
		tessellated.wire_index_count = (u32) generated_wire_index_count;

		tessellated.patches.add_uninitialized(source_triangle_count);
		u32 vertex_offset = 0;
		u32 index_offset = 0;
		for (u32 tri_idx = 0; tri_idx < source_triangle_count; ++tri_idx)
		{
			tessellated.patches[tri_idx] = (TessellationPatch) {
				.source_index_offset = tri_idx * 3u,
				.source_triangle_index = tri_idx,
				.generated_vertex_offset = vertex_offset,
				.generated_index_offset = index_offset,
				.tess_factor = factor,
				.vertex_count = vertices_per_patch,
				.index_count = indices_per_patch,
			};
			vertex_offset += vertices_per_patch;
			index_offset += indices_per_patch;
		}

		build_edge_weld_pairs(mesh, factor);

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

			sg_begin_pass((sg_pass) { .compute = true, .label = debug_label });
			{
				GpuDebugScope debug_scope(debug_label);
				sg_apply_pipeline(pipeline);
				sg_apply_uniforms(0, SG_RANGE(params));
				sg_apply_bindings(&bindings);
				sg_dispatch((i32) dispatch_count, 1, 1);
			}
			sg_end_pass();
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
			sg_begin_pass((sg_pass) { .compute = true, .label = debug_label });
			{
				GpuDebugScope debug_scope(debug_label);
				sg_apply_pipeline(weld_edges_pipeline);
				sg_apply_uniforms(0, SG_RANGE(params));
				sg_apply_bindings(&bindings);
				sg_dispatch((i32) group_count, 1, 1);
			}
			sg_end_pass();
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
