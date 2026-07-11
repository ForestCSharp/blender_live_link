#pragma once

#include <cmath>

#include "core/timings.h"
#include "render/shader_module.h"
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

	struct PipelineState
	{
		VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		u32 binding_count = 0;
	};

	inline PipelineState clear_counters;
	inline PipelineState measure_mesh_factor;
	inline PipelineState plan_patches;
	inline PipelineState emit_vertices;
	inline PipelineState emit_indices;
	inline VkDescriptorPool pools[MAX_FRAMES_IN_FLIGHT] = {};
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

	inline void create_pipeline(VulkanContext* ctx, PipelineState& out, u32 binding_count,
		const char* shader_path, u32 push_constant_size)
	{
		out.binding_count = binding_count;
		StretchyBuffer<VkDescriptorSetLayoutBinding> bindings;
		for (u32 binding_idx = 0; binding_idx < binding_count; ++binding_idx)
		{
			bindings.add((VkDescriptorSetLayoutBinding) {
				.binding = binding_idx,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			});
		}
		VkDescriptorSetLayoutCreateInfo set_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = binding_count,
			.pBindings = bindings.data(),
		};
		VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &set_info, nullptr, &out.set_layout));

		VkPushConstantRange push_range = {
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.offset = 0,
			.size = push_constant_size,
		};
		VkPipelineLayoutCreateInfo layout_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &out.set_layout,
			.pushConstantRangeCount = push_constant_size > 0 ? 1u : 0u,
			.pPushConstantRanges = push_constant_size > 0 ? &push_range : nullptr,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_info, nullptr, &out.pipeline_layout));

		VkShaderModule module = create_shader_module_from_file(ctx->device, shader_path);
		VkComputePipelineCreateInfo pipeline_info = {
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT,
				.module = module,
				.pName = "main",
			},
			.layout = out.pipeline_layout,
		};
		VK_CHECK(vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &out.pipeline));
		vkDestroyShaderModule(ctx->device, module, nullptr);
	}

	inline void init(VulkanContext* ctx)
	{
		if (initialized) { return; }
		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			VkDescriptorPoolSize pool_size = {
				.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = MAX_SETS_PER_FRAME * 5,
			};
			VkDescriptorPoolCreateInfo pool_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.maxSets = MAX_SETS_PER_FRAME,
				.poolSizeCount = 1,
				.pPoolSizes = &pool_size,
			};
			VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_info, nullptr, &pools[frame_idx]));
		}
		create_pipeline(ctx, clear_counters, 1, "bin/shaders/tessellation_clear_counters.comp.spv", 0);
		create_pipeline(ctx, measure_mesh_factor, 3, "bin/shaders/tessellation_measure_mesh_factor.comp.spv", sizeof(PlanParams));
		create_pipeline(ctx, plan_patches, 4, "bin/shaders/tessellation_plan_patches.comp.spv", sizeof(PlanParams));
		create_pipeline(ctx, emit_vertices, 5, "bin/shaders/tessellation_emit_vertices_gpu.comp.spv", sizeof(ComputeParams));
		create_pipeline(ctx, emit_indices, 4, "bin/shaders/tessellation_emit_indices_gpu.comp.spv", sizeof(ComputeParams));
		initialized = true;
	}

	inline VkDescriptorSet bind_set(VulkanContext* ctx, PipelineState& pipeline_state,
		const VkBuffer* buffers, u32 buffer_count)
	{
		assert(buffer_count == pipeline_state.binding_count);
		VkDescriptorSet set = VK_NULL_HANDLE;
		VkDescriptorSetAllocateInfo allocate_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = pools[ctx->frame_index],
			.descriptorSetCount = 1,
			.pSetLayouts = &pipeline_state.set_layout,
		};
		VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &set));
		VkDescriptorBufferInfo infos[5] = {};
		VkWriteDescriptorSet writes[5] = {};
		for (u32 idx = 0; idx < buffer_count; ++idx)
		{
			infos[idx] = { .buffer = buffers[idx], .offset = 0, .range = VK_WHOLE_SIZE };
			writes[idx] = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = set,
				.dstBinding = idx,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &infos[idx],
			};
		}
		vkUpdateDescriptorSets(ctx->device, buffer_count, writes, 0, nullptr);
		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_state.pipeline);
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
			pipeline_state.pipeline_layout, 0, 1, &set, 0, nullptr);
		return set;
	}

	inline void compute_barrier(VulkanContext* ctx, VkPipelineStageFlags2 dst_stages,
		VkAccessFlags2 dst_access)
	{
		VkMemoryBarrier2 barrier = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			.dstStageMask = dst_stages,
			.dstAccessMask = dst_access,
		};
		VkDependencyInfo info = {
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &barrier,
		};
		vkCmdPipelineBarrier2(ctx->command_buffers[ctx->frame_index], &info);
	}

	inline void cleanup_slot(TessellatedGeometry::GpuSlot& slot)
	{
		slot.counters_buffer.destroy_gpu_buffer();
		slot.patch_buffer.destroy_gpu_buffer();
		slot.vertex_buffer.destroy_gpu_buffer();
		slot.index_buffer.destroy_gpu_buffer();
		slot.wire_index_buffer.destroy_gpu_buffer();
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
		slot.counters_readback = GpuBuffer((GpuBufferDesc<TessellationCounters>) {
			.data = nullptr, .size = sizeof(TessellationCounters),
			.usage = { .stream_update = true, .readback = true }, .label = "Tessellation counter readback",
		});
		// Force creation before descriptor/copy recording.
		slot.counters_buffer.get_gpu_buffer(); slot.patch_buffer.get_gpu_buffer();
		slot.vertex_buffer.get_gpu_buffer(); slot.index_buffer.get_gpu_buffer();
		slot.wire_index_buffer.get_gpu_buffer(); slot.counters_readback.get_gpu_buffer();
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

	inline void consume_readbacks(VulkanContext* ctx, State& state)
	{
		for (i32 object_id : state.scene.indexes.mesh_object_ids)
		{
			auto found = state.scene.objects.find(object_id);
			if (found == state.scene.objects.end()) { continue; }
			TessellatedGeometry& tessellated = found->second.mesh.tessellated_geometry;
			for (u32 slot_idx = 0; slot_idx < TessellatedGeometry::GPU_SLOT_COUNT; ++slot_idx)
			{
				auto& slot = tessellated.gpu_slots[slot_idx];
				if (!slot.readback_requested || ctx->frame_number < slot.ready_frame_number) { continue; }
				slot.counters_readback.read_gpu_buffer(&slot.counters, sizeof(slot.counters));
				slot.readback_requested = false;
				slot.has_counts = true;
				if (slot.counters.overflowed || slot.counters.patch_count > slot.patch_capacity
					|| slot.counters.vertex_count > slot.vertex_capacity || slot.counters.index_count > slot.index_capacity)
				{
					tessellated.overflowed = true;
					continue;
				}
				tessellated.active_gpu_slot = slot_idx;
				tessellated.active = slot.counters.index_count > 0;
				tessellated.overflowed = false;
				tessellated.patch_count = slot.counters.patch_count;
				tessellated.vertex_count = slot.counters.vertex_count;
				tessellated.index_count = slot.counters.index_count;
				tessellated.wire_index_count = slot.counters.wire_index_count;
				tessellated.readback_age = 0;
			}
		}
	}

	inline u32 choose_slot(TessellatedGeometry& tessellated, bool allow_active)
	{
		for (u32 attempt = 0; attempt < TessellatedGeometry::GPU_SLOT_COUNT; ++attempt)
		{
			u32 idx = (tessellated.next_gpu_slot + attempt) % TessellatedGeometry::GPU_SLOT_COUNT;
			auto& slot = tessellated.gpu_slots[idx];
			if (!slot.readback_requested && (allow_active || idx != tessellated.active_gpu_slot))
			{
				tessellated.next_gpu_slot = (idx + 1) % TessellatedGeometry::GPU_SLOT_COUNT;
				return idx;
			}
		}
		return TessellatedGeometry::GPU_SLOT_COUNT;
	}

	inline bool prepare_mesh(VulkanContext* ctx, State& state, Object& object, const Camera& camera, f32 fov)
	{
		Mesh& mesh = object.mesh;
		TessellatedGeometry& tessellated = mesh.tessellated_geometry;
		if (mesh.index_count < 3 || mesh.vertex_count == 0
			|| (mesh.has_skinned_vertices && !mesh.skinned_vertex_cache_valid))
		{
			tessellated.active = false;
			return false;
		}
		const bool needs_readback = state.tessellation.mode != ETessellationMode::Fixed;
		const u32 slot_idx = choose_slot(tessellated, !needs_readback);
		if (slot_idx >= TessellatedGeometry::GPU_SLOT_COUNT) { return tessellated.active; }

		const u32 triangle_count = mesh.index_count / 3;
		const u32 max_factor = CLAMP((u32) state.tessellation.max_factor, 1u, MAX_FACTOR);
		u32 patch_capacity, vertex_capacity, index_capacity;
		if (!needs_readback)
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
			patch_capacity = (u32) MIN((u64) triangle_count * split * split, (u64) state.tessellation.max_generated_patches);
			vertex_capacity = (u32) MIN((u64) patch_capacity * vertex_count_for_factor(max_factor), (u64) state.tessellation.max_generated_vertices);
			index_capacity = (u32) MIN((u64) patch_capacity * index_count_for_factor(max_factor), (u64) state.tessellation.max_generated_indices);
		}
		if (!ensure_slot(tessellated.gpu_slots[slot_idx], patch_capacity, vertex_capacity, index_capacity))
		{
			tessellated.active = false; return false;
		}
		auto& slot = tessellated.gpu_slots[slot_idx];
		slot.readback_requested = false; slot.has_counts = false; slot.counters = {};
		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];

		{ VkBuffer buffers[] = { slot.counters_buffer.get_gpu_buffer() };
			bind_set(ctx, clear_counters, buffers, 1); vkCmdDispatch(command_buffer, 1, 1, 1); }
		compute_barrier(ctx, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

		if (state.tessellation.mode == ETessellationMode::AdaptiveAngularPerMesh)
		{
			for (u32 base = 0; base < triangle_count; base += MAX_COMPUTE_GROUPS_PER_DISPATCH)
			{
				PlanParams params = make_plan_params(state, object, camera, fov, slot, triangle_count, base);
				VkBuffer buffers[] = { source_vertices(mesh), mesh.index_buffer.get_gpu_buffer(), slot.counters_buffer.get_gpu_buffer() };
				bind_set(ctx, measure_mesh_factor, buffers, 3);
				vkCmdPushConstants(command_buffer, measure_mesh_factor.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
				vkCmdDispatch(command_buffer, MIN(MAX_COMPUTE_GROUPS_PER_DISPATCH, triangle_count - base), 1, 1);
			}
			compute_barrier(ctx, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		}

		for (u32 base = 0; base < triangle_count; base += MAX_COMPUTE_GROUPS_PER_DISPATCH)
		{
			PlanParams params = make_plan_params(state, object, camera, fov, slot, triangle_count, base);
			VkBuffer buffers[] = { source_vertices(mesh), mesh.index_buffer.get_gpu_buffer(), slot.patch_buffer.get_gpu_buffer(), slot.counters_buffer.get_gpu_buffer() };
			bind_set(ctx, plan_patches, buffers, 4);
			vkCmdPushConstants(command_buffer, plan_patches.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
			vkCmdDispatch(command_buffer, MIN(MAX_COMPUTE_GROUPS_PER_DISPATCH, triangle_count - base), 1, 1);
		}
		compute_barrier(ctx, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

		for (u32 base = 0; base < patch_capacity; base += MAX_COMPUTE_GROUPS_PER_DISPATCH)
		{
			ComputeParams params = { .count = (i32) patch_capacity, .base_index = (i32) base, .phong_strength = state.tessellation.phong_strength };
			VkBuffer vertex_buffers[] = { source_vertices(mesh), mesh.index_buffer.get_gpu_buffer(), slot.patch_buffer.get_gpu_buffer(), slot.vertex_buffer.get_gpu_buffer(), slot.counters_buffer.get_gpu_buffer() };
			bind_set(ctx, emit_vertices, vertex_buffers, 5);
			vkCmdPushConstants(command_buffer, emit_vertices.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
			vkCmdDispatch(command_buffer, MIN(MAX_COMPUTE_GROUPS_PER_DISPATCH, patch_capacity - base), 1, 1);
			VkBuffer index_buffers[] = { slot.patch_buffer.get_gpu_buffer(), slot.index_buffer.get_gpu_buffer(), slot.wire_index_buffer.get_gpu_buffer(), slot.counters_buffer.get_gpu_buffer() };
			bind_set(ctx, emit_indices, index_buffers, 4);
			vkCmdPushConstants(command_buffer, emit_indices.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
			vkCmdDispatch(command_buffer, MIN(MAX_COMPUTE_GROUPS_PER_DISPATCH, patch_capacity - base), 1, 1);
		}
		compute_barrier(ctx,
			VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT
				| VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT
				| VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);

		if (!needs_readback)
		{
			u32 factor = CLAMP((u32) state.tessellation.fixed_factor, 1u, max_factor);
			slot.counters = { .patch_count = triangle_count, .vertex_count = vertex_capacity,
				.index_count = index_capacity, .wire_index_count = index_capacity * 2u,
				.source_triangle_count = triangle_count, .max_factor_seen = factor };
			tessellated.active_gpu_slot = slot_idx; tessellated.active = true; tessellated.overflowed = false;
			tessellated.patch_count = triangle_count; tessellated.vertex_count = vertex_capacity;
			tessellated.index_count = index_capacity; tessellated.wire_index_count = index_capacity * 2u;
		}
		else
		{
			VkBufferCopy copy = { .size = sizeof(TessellationCounters) };
			vkCmdCopyBuffer(command_buffer, slot.counters_buffer.get_gpu_buffer(), slot.counters_readback.get_gpu_buffer(), 1, &copy);
			slot.readback_requested = true;
			slot.ready_frame_number = ctx->frame_number + MAX_FRAMES_IN_FLIGHT;
		}
		tessellated.gpu_planned = true;
		return tessellated.active;
	}

	inline void reset_stats(State& state)
	{
		state.tessellation.source_triangle_count = 0; state.tessellation.patch_count = 0;
		state.tessellation.generated_vertex_count = 0; state.tessellation.generated_index_count = 0;
		state.tessellation.mesh_count = 0; state.tessellation.overflowed_mesh_count = 0;
		state.tessellation.max_factor_seen = 1; state.tessellation.readback_age = 0;
	}

	inline void update(VulkanContext* ctx, State& state, const Camera& camera, f32 fov)
	{
		scene_ensure_indexes(state);
		consume_readbacks(ctx, state);
		reset_stats(state);
		VK_CHECK(vkResetDescriptorPool(ctx->device, pools[ctx->frame_index], 0));
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
			prepare_mesh(ctx, state, found->second, camera, fov);
			auto& tessellated = found->second.mesh.tessellated_geometry;
			if (tessellated.active)
			{
				state.tessellation.mesh_count++;
				state.tessellation.source_triangle_count += (i32) (found->second.mesh.index_count / 3);
				state.tessellation.patch_count += (i32) tessellated.patch_count;
				state.tessellation.generated_vertex_count += (i32) tessellated.vertex_count;
				state.tessellation.generated_index_count += (i32) tessellated.index_count;
			}
			if (tessellated.overflowed) { state.tessellation.overflowed_mesh_count++; }
		}
	}

	inline void shutdown(VulkanContext* ctx)
	{
		if (!initialized) { return; }
		PipelineState* states[] = { &clear_counters, &measure_mesh_factor, &plan_patches, &emit_vertices, &emit_indices };
		for (PipelineState* pipeline_state : states)
		{
			vkDestroyPipeline(ctx->device, pipeline_state->pipeline, nullptr);
			vkDestroyPipelineLayout(ctx->device, pipeline_state->pipeline_layout, nullptr);
			vkDestroyDescriptorSetLayout(ctx->device, pipeline_state->set_layout, nullptr);
		}
		for (VkDescriptorPool pool : pools) { vkDestroyDescriptorPool(ctx->device, pool, nullptr); }
		initialized = false;
	}
}
