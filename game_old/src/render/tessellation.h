#pragma once

#include <cmath>
#include <cstddef>
#include <optional>

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

	struct PlanParams
	{
		HMM_Mat4 model_matrix = HMM_M4D(1.0f);
		HMM_Vec4 camera_position = HMM_V4(0, 0, 0, 1);
		i32 source_triangle_count = 0;
		i32 base_triangle_index = 0;
		i32 max_patch_count = 0;
		i32 max_vertex_count = 0;
		i32 max_index_count = 0;
		i32 max_factor = 1;
		i32 virtual_patches_enabled = 0;
		i32 virtual_patch_max_depth = 0;
		i32 tessellation_mode = 0;
		i32 fixed_factor = 1;
		i32 plan_padding0 = 0;
		i32 plan_padding1 = 0;
		f32 fov_radians = 0.0f;
		f32 render_height = 0.0f;
		f32 target_pixels_per_segment = 1.0f;
		f32 padding0 = 0.0f;
	};

	static_assert(sizeof(PlanParams) == 144, "PlanParams must match tessellation plan_params shader layout.");

	std::optional<sg_shader> clear_counters_shader;
	std::optional<sg_shader> measure_mesh_factor_shader;
	std::optional<sg_shader> plan_patches_shader;
	std::optional<sg_shader> emit_vertices_gpu_shader;
	std::optional<sg_shader> emit_indices_gpu_shader;

	sg_pipeline clear_counters_pipeline = {};
	sg_pipeline measure_mesh_factor_pipeline = {};
	sg_pipeline plan_patches_pipeline = {};
	sg_pipeline emit_vertices_gpu_pipeline = {};
	sg_pipeline emit_indices_gpu_pipeline = {};

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

	HMM_Mat4 transform_matrix(const Transform& transform)
	{
		HMM_Mat4 scale_matrix = HMM_Scale(transform.scale);
		HMM_Mat4 rotation_matrix = HMM_QToM4(transform.rotation);
		HMM_Mat4 translation_matrix = HMM_Translate(transform.location.XYZ);
		return HMM_MulM4(translation_matrix, HMM_MulM4(rotation_matrix, scale_matrix));
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

		clear_counters_shader = sg_make_shader(tessellation_clear_counters_shader_desc(sg_query_backend()));
		measure_mesh_factor_shader = sg_make_shader(tessellation_measure_mesh_factor_shader_desc(sg_query_backend()));
		plan_patches_shader = sg_make_shader(tessellation_plan_patches_shader_desc(sg_query_backend()));
		emit_vertices_gpu_shader = sg_make_shader(tessellation_emit_vertices_gpu_shader_desc(sg_query_backend()));
		emit_indices_gpu_shader = sg_make_shader(tessellation_emit_indices_gpu_shader_desc(sg_query_backend()));

		clear_counters_pipeline = sg_make_pipeline((sg_pipeline_desc) {
			.compute = true,
			.shader = clear_counters_shader.value(),
			.label = "tessellation-clear-counters-pipeline",
		});
		measure_mesh_factor_pipeline = sg_make_pipeline((sg_pipeline_desc) {
			.compute = true,
			.shader = measure_mesh_factor_shader.value(),
			.label = "tessellation-measure-mesh-factor-pipeline",
		});
		plan_patches_pipeline = sg_make_pipeline((sg_pipeline_desc) {
			.compute = true,
			.shader = plan_patches_shader.value(),
			.label = "tessellation-plan-patches-pipeline",
		});
		emit_vertices_gpu_pipeline = sg_make_pipeline((sg_pipeline_desc) {
			.compute = true,
			.shader = emit_vertices_gpu_shader.value(),
			.label = "tessellation-emit-vertices-gpu-pipeline",
		});
		emit_indices_gpu_pipeline = sg_make_pipeline((sg_pipeline_desc) {
			.compute = true,
			.shader = emit_indices_gpu_shader.value(),
			.label = "tessellation-emit-indices-gpu-pipeline",
		});

		initialized = true;
	}

	bool buffer_readback_supported();

	void reset_stats(State& state)
	{
		state.tessellation.source_triangle_count = 0;
		state.tessellation.patch_count = 0;
		state.tessellation.generated_vertex_count = 0;
		state.tessellation.generated_index_count = 0;
		state.tessellation.mesh_count = 0;
		state.tessellation.overflowed_mesh_count = 0;
		state.tessellation.max_factor_seen = 1;
		state.tessellation.readback_supported = buffer_readback_supported();
		state.tessellation.readback_age = 0;
	}

	void disable_all_meshes(State& state)
	{
		scene_ensure_indexes(state);
		for (const i32 unique_id : state.scene.indexes.mesh_object_ids)
		{
			if (!state.scene.objects.contains(unique_id))
			{
				continue;
			}

			Object& object = state.scene.objects[unique_id];
			assert(object.has_mesh);
			object.mesh.tessellated_geometry.active = false;
			object.mesh.tessellated_geometry.overflowed = false;
		}
	}

	bool buffer_readback_supported()
	{
		const sg_backend backend = sg_query_backend();
		return backend == SG_BACKEND_D3D11 ||
			backend == SG_BACKEND_METAL_IOS ||
			backend == SG_BACKEND_METAL_MACOS ||
			backend == SG_BACKEND_METAL_SIMULATOR;
	}

	void cleanup_gpu_slot(TessellatedGeometry::GpuSlot& slot)
	{
		slot.counters_buffer.destroy_gpu_buffer();
		slot.patch_buffer.destroy_gpu_buffer();
		slot.vertex_buffer.destroy_gpu_buffer();
		slot.index_buffer.destroy_gpu_buffer();
		slot.wire_index_buffer.destroy_gpu_buffer();
		if (slot.counters_readback.impl)
		{
			sg_destroy_buffer_readback(slot.counters_readback);
		}
		slot = {};
	}

	bool ensure_gpu_slot(
		TessellatedGeometry::GpuSlot& slot,
		const u32 patch_capacity,
		const u32 vertex_capacity,
		const u32 index_capacity,
		const bool needs_readback)
	{
		const u32 safe_patch_capacity = MAX(patch_capacity, 1u);
		const u32 safe_vertex_capacity = MAX(vertex_capacity, 3u);
		const u32 safe_index_capacity = MAX(index_capacity, 3u);
		const u32 safe_wire_index_capacity = safe_index_capacity * 2u;
		if (slot.patch_capacity >= safe_patch_capacity &&
			slot.vertex_capacity >= safe_vertex_capacity &&
			slot.index_capacity >= safe_index_capacity &&
			slot.wire_index_capacity >= safe_wire_index_capacity &&
			slot.counters_buffer.is_gpu_buffer_valid() &&
			(!needs_readback || slot.counters_readback.impl))
		{
			return true;
		}

		cleanup_gpu_slot(slot);
		slot.patch_capacity = safe_patch_capacity;
		slot.vertex_capacity = safe_vertex_capacity;
		slot.index_capacity = safe_index_capacity;
		slot.wire_index_capacity = safe_wire_index_capacity;
		slot.counters_buffer = GpuBuffer((GpuBufferDesc<TessellationCounters>) {
			.data = nullptr,
			.size = sizeof(TessellationCounters),
			.usage = {
				.storage_buffer = true,
			},
			.label = "TessellatedGeometry::gpu_slot::counters_buffer",
		});
		slot.patch_buffer = GpuBuffer((GpuBufferDesc<TessellationPatch>) {
			.data = nullptr,
			.size = sizeof(TessellationPatch) * slot.patch_capacity,
			.usage = {
				.storage_buffer = true,
			},
			.label = "TessellatedGeometry::gpu_slot::patch_buffer",
		});
		slot.vertex_buffer = GpuBuffer((GpuBufferDesc<Vertex>) {
			.data = nullptr,
			.size = sizeof(Vertex) * slot.vertex_capacity,
			.usage = {
				.vertex_buffer = true,
				.storage_buffer = true,
			},
			.label = "TessellatedGeometry::gpu_slot::vertex_buffer",
		});
		slot.index_buffer = GpuBuffer((GpuBufferDesc<u32>) {
			.data = nullptr,
			.size = sizeof(u32) * slot.index_capacity,
			.usage = {
				.index_buffer = true,
				.storage_buffer = true,
			},
			.label = "TessellatedGeometry::gpu_slot::index_buffer",
		});
		slot.wire_index_buffer = GpuBuffer((GpuBufferDesc<u32>) {
			.data = nullptr,
			.size = sizeof(u32) * slot.wire_index_capacity,
			.usage = {
				.index_buffer = true,
				.storage_buffer = true,
			},
			.label = "TessellatedGeometry::gpu_slot::wire_index_buffer",
		});
		slot.counters_buffer.get_storage_view();
		slot.patch_buffer.get_storage_view();
		slot.vertex_buffer.get_storage_view();
		slot.index_buffer.get_storage_view();
		slot.wire_index_buffer.get_storage_view();
		if (needs_readback)
		{
			sg_buffer_readback_desc readback_desc = {
				.size = sizeof(TessellationCounters),
				.label = "TessellatedGeometry::gpu_slot::counters_readback",
			};
			slot.counters_readback = sg_make_buffer_readback(&readback_desc);
			return slot.counters_readback.impl != 0;
		}
		return true;
	}

	void accumulate_gpu_slot_stats(State& state, TessellatedGeometry& tessellated)
	{
		if (!tessellated.active ||
			!tessellated.gpu_planned ||
			tessellated.active_gpu_slot >= TessellatedGeometry::GPU_SLOT_COUNT)
		{
			return;
		}

		const TessellatedGeometry::GpuSlot& slot = tessellated.gpu_slots[tessellated.active_gpu_slot];
		state.tessellation.source_triangle_count += (i32) slot.counters.source_triangle_count;
		state.tessellation.patch_count += (i32) tessellated.patch_count;
		state.tessellation.generated_vertex_count += (i32) tessellated.vertex_count;
		state.tessellation.generated_index_count += (i32) tessellated.index_count;
		state.tessellation.max_factor_seen = MAX(state.tessellation.max_factor_seen, (i32) slot.counters.max_factor_seen);
		state.tessellation.mesh_count += 1;
		state.tessellation.readback_age = MAX(state.tessellation.readback_age, (i32) tessellated.readback_age);
	}

	void poll_gpu_readbacks(State& state, Mesh& mesh)
	{
		TessellatedGeometry& tessellated = mesh.tessellated_geometry;
		if (!tessellated.gpu_planned)
		{
			return;
		}

		bool consumed_new_counts = false;
		for (u32 slot_index = 0; slot_index < TessellatedGeometry::GPU_SLOT_COUNT; ++slot_index)
		{
			TessellatedGeometry::GpuSlot& slot = tessellated.gpu_slots[slot_index];
			if (!slot.readback_requested || !slot.counters_readback.impl)
			{
				continue;
			}

			const sg_buffer_readback_state readback_state = sg_query_buffer_readback_state(slot.counters_readback);
			if (readback_state == SG_BUFFER_READBACKSTATE_PENDING)
			{
				continue;
			}

			slot.readback_requested = false;
			if (readback_state != SG_BUFFER_READBACKSTATE_READY ||
				!sg_consume_buffer_readback(slot.counters_readback, &slot.counters, sizeof(TessellationCounters)))
			{
				tessellated.readback_supported = false;
				tessellated.active = false;
				continue;
			}

			slot.has_counts = true;
			consumed_new_counts = true;
			tessellated.readback_supported = true;
			if (slot.counters.overflowed ||
				slot.counters.vertex_count == 0 ||
				slot.counters.index_count == 0 ||
				slot.counters.vertex_count > slot.vertex_capacity ||
				slot.counters.index_count > slot.index_capacity ||
				slot.counters.wire_index_count > slot.wire_index_capacity)
			{
				tessellated.active = false;
				tessellated.overflowed = true;
				state.tessellation.overflowed_mesh_count += 1;
				continue;
			}

			tessellated.active_gpu_slot = slot_index;
			tessellated.active = true;
			tessellated.overflowed = false;
			tessellated.patch_count = MIN(slot.counters.patch_count, slot.patch_capacity);
			tessellated.vertex_count = slot.counters.vertex_count;
			tessellated.index_count = slot.counters.index_count;
			tessellated.wire_index_count = slot.counters.wire_index_count;
			tessellated.readback_age = 0;
		}

		if (!consumed_new_counts && tessellated.active)
		{
			tessellated.readback_age += 1;
		}
		accumulate_gpu_slot_stats(state, tessellated);
	}

	u32 choose_gpu_plan_slot(TessellatedGeometry& tessellated, const bool allow_active_slot)
	{
		for (u32 attempt = 0; attempt < TessellatedGeometry::GPU_SLOT_COUNT; ++attempt)
		{
			const u32 slot_index = (tessellated.next_gpu_slot + attempt) % TessellatedGeometry::GPU_SLOT_COUNT;
			TessellatedGeometry::GpuSlot& slot = tessellated.gpu_slots[slot_index];
			if ((allow_active_slot || slot_index != tessellated.active_gpu_slot) && !slot.readback_requested)
			{
				tessellated.next_gpu_slot = (slot_index + 1u) % TessellatedGeometry::GPU_SLOT_COUNT;
				return slot_index;
			}
		}
		return TessellatedGeometry::GPU_SLOT_COUNT;
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

			gpu_execute_compute_pass(debug_label, pipeline, [&]()
			{
				sg_apply_uniforms(0, SG_RANGE(params));
				gpu_apply_bindings(&bindings);
				sg_dispatch((i32) dispatch_count, 1, 1);
			});
		}
	}

	void dispatch_clear_counters(TessellatedGeometry::GpuSlot& slot)
	{
		sg_bindings bindings = {
			.views = {
				[0] = slot.counters_buffer.get_storage_view(),
			},
		};
		const char* debug_label = "Tessellation Clear Counters";
		gpu_execute_compute_pass(debug_label, clear_counters_pipeline, [&]()
		{
			gpu_apply_bindings(&bindings);
			sg_dispatch(1, 1, 1);
		});
	}

	PlanParams make_plan_params(
		State& state,
		Object& object,
		const Camera& camera,
		const f32 fov_radians,
		TessellatedGeometry::GpuSlot& slot,
		const u32 source_triangle_count,
		const u32 base_triangle)
	{
		return (PlanParams) {
			.model_matrix = transform_matrix(object.current_transform),
			.camera_position = HMM_V4V(camera.location, 1.0f),
			.source_triangle_count = (i32) source_triangle_count,
			.base_triangle_index = (i32) base_triangle,
			.max_patch_count = (i32) slot.patch_capacity,
			.max_vertex_count = (i32) slot.vertex_capacity,
			.max_index_count = (i32) slot.index_capacity,
			.max_factor = state.tessellation.max_factor,
			.virtual_patches_enabled = state.tessellation.virtual_patches_enabled ? 1 : 0,
			.virtual_patch_max_depth = state.tessellation.virtual_patch_max_depth,
			.tessellation_mode = (i32) state.tessellation.mode,
			.fixed_factor = state.tessellation.fixed_factor,
			.fov_radians = fov_radians,
			.render_height = (f32) state.window.render_height,
			.target_pixels_per_segment = state.tessellation.target_pixels_per_segment,
		};
	}

	void dispatch_measure_mesh_factor(
		State& state,
		Object& object,
		const Camera& camera,
		const f32 fov_radians,
		TessellatedGeometry::GpuSlot& slot,
		const u32 source_triangle_count)
	{
		const char* debug_label = "Tessellation Measure Mesh Factor";
		for (u32 base_triangle = 0; base_triangle < source_triangle_count; base_triangle += MAX_COMPUTE_GROUPS_PER_DISPATCH)
		{
			const u32 dispatch_count = MIN(MAX_COMPUTE_GROUPS_PER_DISPATCH, source_triangle_count - base_triangle);
			PlanParams params = make_plan_params(state, object, camera, fov_radians, slot, source_triangle_count, base_triangle);
			sg_bindings bindings = {
				.views = {
					[0] = mesh_get_deformed_vertex_storage_view(object.mesh),
					[1] = object.mesh.index_buffer.get_storage_view(),
					[2] = slot.counters_buffer.get_storage_view(),
				},
			};

			gpu_execute_compute_pass(debug_label, measure_mesh_factor_pipeline, [&]()
			{
				sg_apply_uniforms(0, SG_RANGE(params));
				gpu_apply_bindings(&bindings);
				sg_dispatch((i32) dispatch_count, 1, 1);
			});
		}
	}

	void dispatch_plan_patches(
		State& state,
		Object& object,
		const Camera& camera,
		const f32 fov_radians,
		TessellatedGeometry::GpuSlot& slot,
		const u32 source_triangle_count)
	{
		const char* debug_label = "Tessellation Plan Patches";
		for (u32 base_triangle = 0; base_triangle < source_triangle_count; base_triangle += MAX_COMPUTE_GROUPS_PER_DISPATCH)
		{
			const u32 dispatch_count = MIN(MAX_COMPUTE_GROUPS_PER_DISPATCH, source_triangle_count - base_triangle);
			PlanParams params = make_plan_params(state, object, camera, fov_radians, slot, source_triangle_count, base_triangle);

			sg_bindings bindings = {
				.views = {
					[0] = mesh_get_deformed_vertex_storage_view(object.mesh),
					[1] = object.mesh.index_buffer.get_storage_view(),
					[2] = slot.patch_buffer.get_storage_view(),
					[3] = slot.counters_buffer.get_storage_view(),
				},
			};

			gpu_execute_compute_pass(debug_label, plan_patches_pipeline, [&]()
			{
				sg_apply_uniforms(0, SG_RANGE(params));
				gpu_apply_bindings(&bindings);
				sg_dispatch((i32) dispatch_count, 1, 1);
			});
		}
	}

	void emit_mesh_gpu(State& state, Mesh& mesh, TessellatedGeometry::GpuSlot& slot)
	{
		if (slot.patch_capacity == 0 || slot.vertex_capacity == 0 || slot.index_capacity == 0)
		{
			return;
		}

		{
			sg_bindings bindings = {
				.views = {
					[0] = mesh_get_deformed_vertex_storage_view(mesh),
					[1] = mesh.index_buffer.get_storage_view(),
					[2] = slot.patch_buffer.get_storage_view(),
					[3] = slot.vertex_buffer.get_storage_view(),
					[4] = slot.counters_buffer.get_storage_view(),
				},
			};
			dispatch_patch_pipeline(emit_vertices_gpu_pipeline, "Tessellation Emit Vertices GPU", slot.patch_capacity, state.tessellation.phong_strength, bindings);
		}

		{
			sg_bindings bindings = {
				.views = {
					[0] = slot.patch_buffer.get_storage_view(),
					[1] = slot.index_buffer.get_storage_view(),
					[2] = slot.wire_index_buffer.get_storage_view(),
					[3] = slot.counters_buffer.get_storage_view(),
				},
			};
			dispatch_patch_pipeline(emit_indices_gpu_pipeline, "Tessellation Emit Indices GPU", slot.patch_capacity, state.tessellation.phong_strength, bindings);
		}
	}

	bool prepare_mesh_gpu(
		State& state,
		Object& object,
		const Camera& camera,
		const f32 fov_radians)
	{
		static bool unsupported_readback_warning_emitted = false;
		Mesh& mesh = object.mesh;
		TessellatedGeometry& tessellated = mesh.tessellated_geometry;
		tessellated.gpu_planned = true;
		const bool needs_readback = state.tessellation.mode != ETessellationMode::Fixed;
		tessellated.readback_supported = !needs_readback || buffer_readback_supported();
		if (needs_readback && !tessellated.readback_supported)
		{
			if (!unsupported_readback_warning_emitted)
			{
				sg_buffer_readback_desc unsupported_readback_desc = {
					.size = sizeof(TessellationCounters),
					.label = "Tessellation unsupported readback probe",
				};
				sg_buffer_readback unsupported_readback = sg_make_buffer_readback(&unsupported_readback_desc);
				if (unsupported_readback.impl)
				{
					sg_destroy_buffer_readback(unsupported_readback);
				}
				unsupported_readback_warning_emitted = true;
			}
			tessellated.active = false;
			return false;
		}

		if (mesh.index_count < 3 || mesh.vertex_count == 0)
		{
			tessellated.active = false;
			return false;
		}

		if (!mesh_has_deformed_vertex_source(mesh))
		{
			tessellated.active = false;
			return false;
		}

		const u32 plan_slot_index = choose_gpu_plan_slot(tessellated, !needs_readback);
		if (plan_slot_index >= TessellatedGeometry::GPU_SLOT_COUNT)
		{
			return tessellated.active;
		}

		const u32 source_triangle_count = mesh.index_count / 3;
		const u32 max_factor = CLAMP((u32) state.tessellation.max_factor, 1u, MAX_FACTOR);
		u32 patch_capacity = 0;
		u32 vertex_capacity = 0;
		u32 index_capacity = 0;
		u32 fixed_vertex_count = 0;
		u32 fixed_index_count = 0;

		if (state.tessellation.mode == ETessellationMode::Fixed)
		{
			const u32 fixed_factor = CLAMP((u32) state.tessellation.fixed_factor, 1u, max_factor);
			const u64 exact_patch_count = (u64) source_triangle_count;
			const u64 exact_vertex_count = exact_patch_count * (u64) vertex_count_for_factor(fixed_factor);
			const u64 exact_index_count = exact_patch_count * (u64) index_count_for_factor(fixed_factor);
			if (exact_patch_count > (u64) state.tessellation.max_generated_patches ||
				exact_vertex_count > (u64) state.tessellation.max_generated_vertices ||
				exact_index_count > (u64) state.tessellation.max_generated_indices)
			{
				tessellated.active = false;
				tessellated.overflowed = true;
				state.tessellation.overflowed_mesh_count += 1;
				return false;
			}

			patch_capacity = (u32) exact_patch_count;
			vertex_capacity = (u32) exact_vertex_count;
			index_capacity = (u32) exact_index_count;
			fixed_vertex_count = vertex_capacity;
			fixed_index_count = index_capacity;
		}
		else
		{
			u32 max_split_segments = 1u;
			if (state.tessellation.mode == ETessellationMode::AdaptiveAngularPerTriangle && state.tessellation.virtual_patches_enabled)
			{
				const u32 max_depth = (u32) CLAMP(state.tessellation.virtual_patch_max_depth, 0, 4);
				max_split_segments = 1u << max_depth;
			}

			const u64 worst_case_patch_count = (u64) source_triangle_count * (u64) max_split_segments * (u64) max_split_segments;
			patch_capacity = (u32) MIN(worst_case_patch_count, (u64) state.tessellation.max_generated_patches);
			const u64 worst_case_vertex_count = (u64) patch_capacity * (u64) vertex_count_for_factor(max_factor);
			const u64 worst_case_index_count = (u64) patch_capacity * (u64) index_count_for_factor(max_factor);
			vertex_capacity = (u32) MIN(worst_case_vertex_count, (u64) state.tessellation.max_generated_vertices);
			index_capacity = (u32) MIN(worst_case_index_count, (u64) state.tessellation.max_generated_indices);
		}

		if (patch_capacity == 0 || vertex_capacity < 3 || index_capacity < 3)
		{
			tessellated.active = false;
			return false;
		}

		TessellatedGeometry::GpuSlot& slot = tessellated.gpu_slots[plan_slot_index];
		if (!ensure_gpu_slot(slot, patch_capacity, vertex_capacity, index_capacity, needs_readback))
		{
			tessellated.readback_supported = false;
			tessellated.active = false;
			return false;
		}

		slot.readback_requested = false;
		slot.has_counts = false;
		slot.counters = {};
		dispatch_clear_counters(slot);
		if (state.tessellation.mode == ETessellationMode::AdaptiveAngularPerMesh)
		{
			dispatch_measure_mesh_factor(state, object, camera, fov_radians, slot, source_triangle_count);
		}
		dispatch_plan_patches(state, object, camera, fov_radians, slot, source_triangle_count);
		emit_mesh_gpu(state, mesh, slot);

		if (!needs_readback)
		{
			const u32 fixed_factor = CLAMP((u32) state.tessellation.fixed_factor, 1u, max_factor);
			slot.counters = (TessellationCounters) {
				.patch_count = source_triangle_count,
				.vertex_count = fixed_vertex_count,
				.index_count = fixed_index_count,
				.wire_index_count = fixed_index_count * 2u,
				.source_triangle_count = source_triangle_count,
				.overflowed = 0,
				.max_factor_seen = fixed_factor,
			};
			slot.has_counts = true;
			tessellated.active_gpu_slot = plan_slot_index;
			tessellated.active = true;
			tessellated.overflowed = false;
			tessellated.patch_count = source_triangle_count;
			tessellated.vertex_count = fixed_vertex_count;
			tessellated.index_count = fixed_index_count;
			tessellated.wire_index_count = fixed_index_count * 2u;
			tessellated.readback_age = 0;
			accumulate_gpu_slot_stats(state, tessellated);
			return true;
		}

		if (sg_request_buffer_readback(slot.counters_readback, slot.counters_buffer.get_gpu_buffer(), 0, sizeof(TessellationCounters)))
		{
			slot.readback_requested = true;
		}
		else
		{
			tessellated.readback_supported = false;
			tessellated.active = false;
		}

		return tessellated.active;
	}

	void refresh_active_skinned_mesh_gpu(State& state, Mesh& mesh)
	{
		TessellatedGeometry& tessellated = mesh.tessellated_geometry;
		if (!mesh.has_skinned_vertices ||
			!mesh_has_deformed_vertex_source(mesh) ||
			!tessellated.active ||
			!tessellated.gpu_planned ||
			tessellated.active_gpu_slot >= TessellatedGeometry::GPU_SLOT_COUNT)
		{
			return;
		}

		emit_mesh_gpu(state, mesh, tessellated.gpu_slots[tessellated.active_gpu_slot]);
	}

	void update(State& state, const Camera& camera, const f32 fov_radians)
	{
		reset_stats(state);
		state.tessellation.max_factor = CLAMP(state.tessellation.max_factor, 1, (i32) MAX_FACTOR);
		state.tessellation.fixed_factor = CLAMP(state.tessellation.fixed_factor, 1, state.tessellation.max_factor);
		state.tessellation.target_pixels_per_segment = fmaxf(state.tessellation.target_pixels_per_segment, 1.0f);
		state.tessellation.phong_strength = CLAMP(state.tessellation.phong_strength, 0.0f, 1.0f);
		state.tessellation.virtual_patch_max_depth = CLAMP(state.tessellation.virtual_patch_max_depth, 0, 4);
		state.tessellation.max_generated_patches = MAX(state.tessellation.max_generated_patches, 1);
		state.tessellation.max_generated_vertices = MAX(state.tessellation.max_generated_vertices, 3);
		state.tessellation.max_generated_indices = MAX(state.tessellation.max_generated_indices, 3);
		state.tessellation.bounds_padding = fmaxf(state.tessellation.bounds_padding, 0.0f);

		if (!state.tessellation.enabled)
		{
			disable_all_meshes(state);
			return;
		}

		init();
		scene_ensure_indexes(state);
		state.data_oriented.frame.tessellation_candidate_count += (i32)state.scene.indexes.mesh_object_ids.length();

		for (const i32 unique_id : state.scene.indexes.mesh_object_ids)
		{
			if (!state.scene.objects.contains(unique_id))
			{
				continue;
			}

			Object& object = state.scene.objects[unique_id];
			assert(object.has_mesh);
			if (state.tessellation.mode != ETessellationMode::Fixed)
			{
				poll_gpu_readbacks(state, object.mesh);
				refresh_active_skinned_mesh_gpu(state, object.mesh);
			}
			prepare_mesh_gpu(state, object, camera, fov_radians);
			state.data_oriented.frame.tessellation_processed_count += 1;
		}
	}
}
