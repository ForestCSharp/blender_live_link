#pragma once

#include <cmath>

#include "core/timings.h"
#include "render/core/frame_render_graph.h"
#include "render/core/fullscreen_pipeline.h"
#include "state/state.h"

namespace Tessellation
{
	static constexpr u32 MAX_FACTOR = 31;
	static constexpr u32 MAX_COMPUTE_GROUPS_PER_DISPATCH = 65535;
	static constexpr u32 MAX_SETS_PER_FRAME = 2048;

	struct ComputeParams
	{
		i32 count = 0;
		i32 base_index = 0;
		f32 phong_strength = 0.0f;
		i32 padding0 = 0;
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
	static_assert(sizeof(PlanParams) == 144, "PlanParams shader layout mismatch");

	struct DrawCommandParams
	{
		i32 object_index = 0;
		i32 index_capacity = 0;
		i32 wire_index_capacity = 0;
		i32 padding0 = 0;
	};

	inline ComputeEffect clear_counters;
	inline TypedComputeEffect<PlanParams> measure_mesh_factor;
	inline TypedComputeEffect<PlanParams> plan_patches;
	inline TypedComputeEffect<ComputeParams> emit_vertices;
	inline TypedComputeEffect<ComputeParams> emit_indices;
	inline TypedComputeEffect<DrawCommandParams> write_draw_commands;
	inline bool initialized = false;

	inline u32 vertex_count_for_factor(u32 factor)
	{
		factor = CLAMP(factor, 1u, MAX_FACTOR);
		return ((factor + 1u) * (factor + 2u)) / 2u;
	}

	inline u32 index_count_for_factor(u32 factor)
	{
		factor = CLAMP(factor, 1u, MAX_FACTOR);
		return factor * factor * 3u;
	}

	inline HMM_Mat4 transform_matrix(const Transform& transform)
	{
		return HMM_MulM4(HMM_Translate(transform.location.XYZ),
			HMM_MulM4(HMM_QToM4(transform.rotation), HMM_Scale(transform.scale)));
	}

	template<typename EffectT>
	inline void init_effect(VulkanContext* ctx, EffectT& out, u32 binding_count,
		const char* shader_path)
	{
		DynamicArray<DescriptorBindingSpec> bindings;
		for (u32 binding_idx = 0; binding_idx < binding_count; ++binding_idx)
		{
			bindings.add({
				.binding = binding_idx,
				.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.stages = VK_SHADER_STAGE_COMPUTE_BIT,
			});
		}
		out.init(ctx, {
			.shader_path = shader_path,
			.bindings = bindings.data(),
			.binding_count = binding_count,
		});
	}

	inline void init(VulkanContext* ctx)
	{
		if (initialized) { return; }
		init_effect(ctx, clear_counters, 1,
			"bin/shaders/tessellation_clear_counters.comp.spv");
		init_effect(ctx, measure_mesh_factor, 3,
			"bin/shaders/tessellation_measure_mesh_factor.comp.spv");
		init_effect(ctx, plan_patches, 4,
			"bin/shaders/tessellation_plan_patches.comp.spv");
		init_effect(ctx, emit_vertices, 5,
			"bin/shaders/tessellation_emit_vertices_gpu.comp.spv");
		init_effect(ctx, emit_indices, 4,
			"bin/shaders/tessellation_emit_indices_gpu.comp.spv");
		init_effect(ctx, write_draw_commands, 3,
			"bin/shaders/tessellation_write_draw_commands.comp.spv");
		initialized = true;
	}

	inline void bind_set(VulkanContext* ctx, ComputeEffect& effect,
		const VkBuffer* buffers, u32 buffer_count)
	{
		assert(buffer_count == effect.descriptors.binding_specs.length());
		DescriptorWriter writer = effect.writer(ctx);
		for (u32 idx = 0; idx < buffer_count; ++idx)
		{
			writer.buffer(idx, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffers[idx]);
		}
		writer.commit();
		effect.bind(ctx, writer.set);
	}

	// Only pay for the diagnostics copy when something will actually display it.
	//
	// debug_ui.visible defaults to true and is not compiled out with the UI, so
	// without this guard a WITH_DEBUG_UI=0 build would run the copy every frame
	// with nothing able to read it.
	inline bool stats_wanted(const State& state)
	{
#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
		return state.debug_ui.visible || state.runtime.benchmark_active;
#else
		(void) state;
		return state.runtime.benchmark_active;
#endif
	}

	inline void cleanup_slot(TessellatedGeometry::GpuSlot& slot)
	{
		slot.counters_buffer.destroy_gpu_buffer();
		slot.patch_buffer.destroy_gpu_buffer();
		slot.vertex_buffer.destroy_gpu_buffer();
		slot.index_buffer.destroy_gpu_buffer();
		slot.wire_index_buffer.destroy_gpu_buffer();
		slot.draw_command_buffer.destroy_gpu_buffer();
		slot.wire_draw_command_buffer.destroy_gpu_buffer();
		slot.counters_readback.destroy_gpu_buffer();
		slot = {};
	}

	inline bool ensure_slot(TessellatedGeometry::GpuSlot& slot, u32 patches, u32 vertices, u32 indices)
	{
		patches = MAX(patches, 1u); vertices = MAX(vertices, 3u); indices = MAX(indices, 3u);
		if (slot.patch_capacity >= patches && slot.vertex_capacity >= vertices
			&& slot.index_capacity >= indices && slot.counters_buffer.is_gpu_buffer_valid())
		{
			return true;
		}
		cleanup_slot(slot);
		slot.patch_capacity = patches;
		slot.vertex_capacity = vertices;
		slot.index_capacity = indices;
		slot.wire_index_capacity = indices * 2u;
		slot.counters_buffer = GpuBuffer((GpuBufferDesc<TessellationCounters>) {
			.data = nullptr, .size = sizeof(TessellationCounters),
			.usage = { .storage_buffer = true, .prefer_device_local = true, .transfer_src = true },
			.label = "Tessellation counters",
		});
		slot.patch_buffer = GpuBuffer((GpuBufferDesc<TessellationPatch>) {
			.data = nullptr, .size = sizeof(TessellationPatch) * patches,
			.usage = { .storage_buffer = true, .prefer_device_local = true }, .label = "Tessellation patches",
		});
		slot.vertex_buffer = GpuBuffer((GpuBufferDesc<Vertex>) {
			.data = nullptr, .size = sizeof(Vertex) * vertices,
			.usage = { .vertex_buffer = true, .storage_buffer = true, .prefer_device_local = true }, .label = "Tessellation vertices",
		});
		slot.index_buffer = GpuBuffer((GpuBufferDesc<u32>) {
			.data = nullptr, .size = sizeof(u32) * indices,
			.usage = { .index_buffer = true, .storage_buffer = true, .prefer_device_local = true }, .label = "Tessellation indices",
		});
		slot.wire_index_buffer = GpuBuffer((GpuBufferDesc<u32>) {
			.data = nullptr, .size = sizeof(u32) * indices * 2u,
			.usage = { .index_buffer = true, .storage_buffer = true, .prefer_device_local = true }, .label = "Tessellation wire indices",
		});
		slot.draw_command_buffer = GpuBuffer((GpuBufferDesc<VkDrawIndexedIndirectCommand>) {
			.data = nullptr, .size = sizeof(VkDrawIndexedIndirectCommand),
			.usage = { .storage_buffer = true, .prefer_device_local = true, .indirect_buffer = true },
			.label = "Tessellation draw command",
		});
		slot.wire_draw_command_buffer = GpuBuffer((GpuBufferDesc<VkDrawIndirectCommand>) {
			.data = nullptr, .size = sizeof(VkDrawIndirectCommand),
			.usage = { .storage_buffer = true, .prefer_device_local = true, .indirect_buffer = true },
			.label = "Tessellation wire draw command",
		});
		// Force creation before descriptor/copy recording.
		slot.counters_buffer.get_gpu_buffer(); slot.patch_buffer.get_gpu_buffer();
		slot.vertex_buffer.get_gpu_buffer(); slot.index_buffer.get_gpu_buffer();
		slot.wire_index_buffer.get_gpu_buffer();
		slot.counters_readback = GpuBuffer((GpuBufferDesc<TessellationCounters>) {
			.data = nullptr, .size = sizeof(TessellationCounters),
			.usage = { .stream_update = true, .readback = true },
			.label = "Tessellation stats readback",
		});
		slot.draw_command_buffer.get_gpu_buffer(); slot.wire_draw_command_buffer.get_gpu_buffer();
		slot.counters_readback.get_gpu_buffer();
		return true;
	}

	inline PlanParams make_plan_params(State& state, Object& object, const Camera& camera,
		f32 fov, TessellatedGeometry::GpuSlot& slot, u32 triangle_count, u32 base_triangle)
	{
		return (PlanParams) {
			.model_matrix = transform_matrix(object.current_transform),
			.camera_position = HMM_V4V(camera.location, 1.0f),
			.source_triangle_count = (i32) triangle_count,
			.base_triangle_index = (i32) base_triangle,
			.max_patch_count = (i32) slot.patch_capacity,
			.max_vertex_count = (i32) slot.vertex_capacity,
			.max_index_count = (i32) slot.index_capacity,
			.max_factor = state.tessellation.max_factor,
			.virtual_patches_enabled = state.tessellation.virtual_patches_enabled ? 1 : 0,
			.virtual_patch_max_depth = state.tessellation.virtual_patch_max_depth,
			.tessellation_mode = (i32) state.tessellation.mode,
			.fixed_factor = state.tessellation.fixed_factor,
			.fov_radians = fov,
			.render_height = (f32) state.window.render_height,
			.target_pixels_per_segment = state.tessellation.target_pixels_per_segment,
		};
	}

	inline VkBuffer source_vertices(Mesh& mesh)
	{
		return mesh.has_skinned_vertices && mesh.skinned_vertex_cache_valid
			? mesh.skinned_vertex_cache_buffer.get_gpu_buffer()
			: mesh.vertex_buffer.get_gpu_buffer();
	}

	// A single slot suffices: draw commands are produced on the GPU in the same
	// frame, so nothing has to be kept alive for a later CPU read.
	inline u32 choose_slot(TessellatedGeometry& tessellated, bool)
	{
		tessellated.next_gpu_slot = 0;
		return 0;
	}

	inline bool prepare_mesh(FrameRenderGraph& graph, VulkanContext* ctx,
		State& state, Object& object, const Camera& camera, f32 fov)
	{
		Mesh& mesh = object.mesh;
		TessellatedGeometry& tessellated = mesh.tessellated_geometry;
		if (mesh.index_count < 3 || mesh.vertex_count == 0
			|| (mesh.has_skinned_vertices && !mesh.skinned_vertex_cache_valid))
		{
			tessellated.active = false;
			return false;
		}
		// Fixed mode derives its counts analytically; adaptive modes let the GPU
		// decide and clamp to capacity. Neither needs a readback any more.
		const bool adaptive = state.tessellation.mode != ETessellationMode::Fixed;
		const u32 slot_idx = choose_slot(tessellated, true);
		if (slot_idx >= TessellatedGeometry::GPU_SLOT_COUNT) { return tessellated.active; }

		const u32 triangle_count = mesh.index_count / 3;
		const u32 max_factor = CLAMP((u32) state.tessellation.max_factor, 1u, MAX_FACTOR);
		u32 patch_capacity, vertex_capacity, index_capacity;
		if (!adaptive)
		{
			const u32 factor = CLAMP((u32) state.tessellation.fixed_factor, 1u, max_factor);
			const u64 patch_count = triangle_count;
			const u64 vertex_count = patch_count * vertex_count_for_factor(factor);
			const u64 index_count = patch_count * index_count_for_factor(factor);
			if (patch_count > (u64) state.tessellation.max_generated_patches
				|| vertex_count > (u64) state.tessellation.max_generated_vertices
				|| index_count > (u64) state.tessellation.max_generated_indices)
			{
				tessellated.active = false; tessellated.overflowed = true; return false;
			}
			patch_capacity = (u32) patch_count; vertex_capacity = (u32) vertex_count; index_capacity = (u32) index_count;
		}
		else
		{
			u32 split = state.tessellation.mode == ETessellationMode::AdaptiveAngularPerTriangle
				&& state.tessellation.virtual_patches_enabled
				? 1u << (u32) CLAMP(state.tessellation.virtual_patch_max_depth, 0, 4) : 1u;
			// Capacity is the analytic worst case at max_factor, clamped by the
			// generation budgets. Whether a mesh actually ran out is not
			// predictable from here - adaptive factors are usually far below
			// max_factor, so nearly every mesh trips the clamp while almost none
			// really overflow. The GPU reports the truth; see consume_stats.
			patch_capacity = (u32) MIN((u64) triangle_count * split * split, (u64) state.tessellation.max_generated_patches);
			vertex_capacity = (u32) MIN((u64) patch_capacity * vertex_count_for_factor(max_factor), (u64) state.tessellation.max_generated_vertices);
			index_capacity = (u32) MIN((u64) patch_capacity * index_count_for_factor(max_factor), (u64) state.tessellation.max_generated_indices);
		}
		if (!ensure_slot(tessellated.gpu_slots[slot_idx], patch_capacity, vertex_capacity, index_capacity))
		{
			tessellated.active = false; return false;
		}
		auto& slot = tessellated.gpu_slots[slot_idx];
		slot.counters = {};
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);

		const VkBuffer source_vertex_buffer = source_vertices(mesh);
		const VkBuffer source_index_buffer = mesh.index_buffer.get_gpu_buffer();
		const VkBuffer counters_buffer = slot.counters_buffer.get_gpu_buffer();
		const VkBuffer patch_buffer = slot.patch_buffer.get_gpu_buffer();
		const VkBuffer vertex_buffer = slot.vertex_buffer.get_gpu_buffer();
		const VkBuffer index_buffer = slot.index_buffer.get_gpu_buffer();
		const VkBuffer wire_index_buffer = slot.wire_index_buffer.get_gpu_buffer();

		graph.storage_write(frame_graph_buffer(counters_buffer));
		graph.compute([&]() {
			VkBuffer buffers[] = { counters_buffer };
			bind_set(ctx, clear_counters, buffers, 1);
			clear_counters.dispatch(ctx, 1, 1, 1);
		});

		if (state.tessellation.mode == ETessellationMode::AdaptiveAngularPerMesh)
		{
			graph.storage_read(frame_graph_buffer(source_vertex_buffer));
			graph.storage_read(frame_graph_buffer(source_index_buffer));
			graph.storage_read_write(frame_graph_buffer(counters_buffer));
			graph.compute([&]() {
				for (u32 base = 0; base < triangle_count;
					base += MAX_COMPUTE_GROUPS_PER_DISPATCH)
				{
					PlanParams params = make_plan_params(
						state, object, camera, fov, slot, triangle_count, base);
					VkBuffer buffers[] = {
						source_vertex_buffer, source_index_buffer, counters_buffer };
					bind_set(ctx, measure_mesh_factor.effect, buffers, 3);
					measure_mesh_factor.dispatch(ctx, params,
						MIN(MAX_COMPUTE_GROUPS_PER_DISPATCH, triangle_count - base), 1, 1);
				}
			});
		}

		graph.storage_read(frame_graph_buffer(source_vertex_buffer));
		graph.storage_read(frame_graph_buffer(source_index_buffer));
		graph.storage_write(frame_graph_buffer(patch_buffer));
		graph.storage_read_write(frame_graph_buffer(counters_buffer));
		graph.compute([&]() {
			for (u32 base = 0; base < triangle_count;
				base += MAX_COMPUTE_GROUPS_PER_DISPATCH)
			{
				PlanParams params = make_plan_params(
					state, object, camera, fov, slot, triangle_count, base);
				VkBuffer buffers[] = { source_vertex_buffer, source_index_buffer,
					patch_buffer, counters_buffer };
				bind_set(ctx, plan_patches.effect, buffers, 4);
				plan_patches.dispatch(ctx, params,
					MIN(MAX_COMPUTE_GROUPS_PER_DISPATCH, triangle_count - base), 1, 1);
			}
		});

		graph.storage_read(frame_graph_buffer(source_vertex_buffer));
		graph.storage_read(frame_graph_buffer(source_index_buffer));
		graph.storage_read(frame_graph_buffer(patch_buffer));
		graph.storage_write(frame_graph_buffer(vertex_buffer));
		graph.storage_write(frame_graph_buffer(index_buffer));
		graph.storage_write(frame_graph_buffer(wire_index_buffer));
		graph.storage_read_write(frame_graph_buffer(counters_buffer));
		graph.compute([&]() {
			for (u32 base = 0; base < patch_capacity;
				base += MAX_COMPUTE_GROUPS_PER_DISPATCH)
			{
				ComputeParams params = { .count = (i32) patch_capacity,
					.base_index = (i32) base,
					.phong_strength = state.tessellation.phong_strength };
				VkBuffer vertex_buffers[] = { source_vertex_buffer, source_index_buffer,
					patch_buffer, vertex_buffer, counters_buffer };
				bind_set(ctx, emit_vertices.effect, vertex_buffers, 5);
				emit_vertices.dispatch(ctx, params,
					MIN(MAX_COMPUTE_GROUPS_PER_DISPATCH, patch_capacity - base), 1, 1);
				VkBuffer index_buffers[] = {
					patch_buffer, index_buffer, wire_index_buffer, counters_buffer };
				bind_set(ctx, emit_indices.effect, index_buffers, 4);
				emit_indices.dispatch(ctx, params,
					MIN(MAX_COMPUTE_GROUPS_PER_DISPATCH, patch_capacity - base), 1, 1);
			}
		});
		graph.storage_read(frame_graph_buffer(vertex_buffer),
			VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
		graph.storage_read(frame_graph_buffer(index_buffer),
			VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
		graph.storage_read(frame_graph_buffer(wire_index_buffer),
			VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
		graph.vertex(frame_graph_buffer(vertex_buffer));
		graph.index(frame_graph_buffer(index_buffer));
		graph.index(frame_graph_buffer(wire_index_buffer));

		// Turn the counters into draw commands on the GPU. This is what removes
		// the readback: the draw learns its index count from this buffer instead
		// of the CPU learning it two frames later.
		const VkBuffer draw_command_buffer = slot.draw_command_buffer.get_gpu_buffer();
		const VkBuffer wire_draw_command_buffer = slot.wire_draw_command_buffer.get_gpu_buffer();
		graph.storage_read(frame_graph_buffer(counters_buffer));
		graph.storage_write(frame_graph_buffer(draw_command_buffer));
		graph.storage_write(frame_graph_buffer(wire_draw_command_buffer));
		graph.compute([&]() {
			DrawCommandParams params = {
				.object_index = object.render_object_index,
				.index_capacity = (i32) slot.index_capacity,
				.wire_index_capacity = (i32) slot.wire_index_capacity,
			};
			VkBuffer command_buffers[] = {
				counters_buffer, draw_command_buffer, wire_draw_command_buffer };
			bind_set(ctx, write_draw_commands.effect, command_buffers, 3);
			write_draw_commands.dispatch(ctx, params, 1, 1, 1);
		});
		graph.indirect_read(frame_graph_buffer(draw_command_buffer));
		graph.indirect_read(frame_graph_buffer(wire_draw_command_buffer));

		// Diagnostics copy. Off the draw path entirely, so it adds no latency and
		// is skipped when nothing will read it.
		if (stats_wanted(state) && !slot.stats_readback_pending)
		{
			const VkBuffer readback_buffer = slot.counters_readback.get_gpu_buffer();
			VkBufferCopy copy = { .size = sizeof(TessellationCounters) };
			graph.transfer_source(frame_graph_buffer(counters_buffer));
			graph.transfer_destination(frame_graph_buffer(readback_buffer));
			graph.transfer([&]() {
				vkCmdCopyBuffer(command_buffer, counters_buffer, readback_buffer, 1, &copy);
			});
			slot.stats_readback_pending = true;
			slot.stats_ready_frame_number = ctx->frame_number + MAX_FRAMES_IN_FLIGHT;
		}

		// Published immediately - the same frame that tessellates also draws.
		// The counts here are capacities, i.e. upper bounds; the exact values
		// stay on the GPU and only the indirect commands see them.
		const u32 reported_factor = adaptive
			? max_factor : CLAMP((u32) state.tessellation.fixed_factor, 1u, max_factor);
		slot.counters = { .patch_count = patch_capacity, .vertex_count = vertex_capacity,
			.index_count = index_capacity, .wire_index_count = index_capacity * 2u,
			.source_triangle_count = triangle_count, .max_factor_seen = reported_factor };
		tessellated.active_gpu_slot = slot_idx;
		tessellated.active = true;
		tessellated.patch_count = patch_capacity;
		tessellated.vertex_count = vertex_capacity;
		tessellated.index_count = index_capacity;
		tessellated.wire_index_count = index_capacity * 2u;
		tessellated.gpu_planned = true;
		return tessellated.active;
	}

	// Publishes the GPU's own counters for display. Deliberately touches nothing
	// the draw path reads: active, active_gpu_slot and the capacities are all
	// decided the frame they are dispatched.
	inline void consume_stats(VulkanContext* ctx, State& state)
	{
		if (!stats_wanted(state)) { return; }
		for (i32 object_id : state.scene.indexes.mesh_object_ids)
		{
			auto found = state.scene.objects.find(object_id);
			if (found == state.scene.objects.end()) { continue; }
			TessellatedGeometry& tessellated = found->second.mesh.tessellated_geometry;
			for (auto& slot : tessellated.gpu_slots)
			{
				if (!slot.stats_readback_pending
					|| ctx->frame_number < slot.stats_ready_frame_number)
				{
					continue;
				}
				slot.counters_readback.read_gpu_buffer(&slot.counters, sizeof(slot.counters));
				slot.stats_readback_pending = false;
				tessellated.stats = slot.counters;
				tessellated.stats_valid = true;
				// The GPU either flagged overflow itself or emitted more than fit.
				tessellated.overflowed = slot.counters.overflowed != 0
					|| slot.counters.patch_count > slot.patch_capacity
					|| slot.counters.vertex_count > slot.vertex_capacity
					|| slot.counters.index_count > slot.index_capacity;
			}
		}
	}

	inline void reset_stats(State& state)
	{
		state.tessellation.source_triangle_count = 0; state.tessellation.patch_count = 0;
		state.tessellation.generated_vertex_count = 0; state.tessellation.generated_index_count = 0;
		state.tessellation.mesh_count = 0; state.tessellation.overflowed_mesh_count = 0;
		state.tessellation.max_factor_seen = 1;
	}

	inline void update(FrameRenderGraph& graph, VulkanContext* ctx,
		State& state, const Camera& camera, f32 fov)
	{
		scene_ensure_indexes(state);
		consume_stats(ctx, state);
		reset_stats(state);
		state.data_oriented.frame.tessellation_candidate_count += (i32) state.scene.indexes.mesh_object_ids.length();
		if (!state.tessellation.enabled)
		{
			for (i32 object_id : state.scene.indexes.mesh_object_ids)
			{
				auto found = state.scene.objects.find(object_id);
				if (found != state.scene.objects.end()) { found->second.mesh.tessellated_geometry.active = false; }
			}
			return;
		}
		CPU_TIMING_SCOPE("Tessellation Update");
		for (i32 object_id : state.scene.indexes.mesh_object_ids)
		{
			auto found = state.scene.objects.find(object_id);
			if (found == state.scene.objects.end()) { continue; }
			prepare_mesh(graph, ctx, state, found->second, camera, fov);
			state.data_oriented.frame.tessellation_processed_count += 1;
			auto& tessellated = found->second.mesh.tessellated_geometry;
			if (tessellated.active)
			{
				state.tessellation.mesh_count++;
				state.tessellation.source_triangle_count += (i32) (found->second.mesh.index_count / 3);
				// Report what the GPU actually produced once it has come back;
				// capacities would otherwise read as wildly inflated totals.
				const TessellationCounters& reported = tessellated.stats;
				state.tessellation.patch_count += (i32) (tessellated.stats_valid ? reported.patch_count : 0u);
				state.tessellation.generated_vertex_count += (i32) (tessellated.stats_valid ? reported.vertex_count : 0u);
				state.tessellation.generated_index_count += (i32) (tessellated.stats_valid ? reported.index_count : 0u);
			}
			if (tessellated.overflowed) { state.tessellation.overflowed_mesh_count++; }
		}
	}

	inline void shutdown(VulkanContext* ctx)
	{
		if (!initialized) { return; }
		emit_indices.shutdown(ctx);
		emit_vertices.shutdown(ctx);
		plan_patches.shutdown(ctx);
		measure_mesh_factor.shutdown(ctx);
		clear_counters.shutdown(ctx);
		initialized = false;
	}
}
