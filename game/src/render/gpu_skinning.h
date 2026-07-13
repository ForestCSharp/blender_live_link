#pragma once

#include "core/types.h"
#include "core/timings.h"
#include "render/vulkan_context.h"
#include "render/shader_module.h"
#include "state/state.h"

// GPU skinning cache (port of game/'s gpu_skinning.h): a compute pass bakes
// each skinned mesh's posed vertices into a per-mesh cache buffer so
// tessellation and the wire overlay can consume skinned meshes as static
// geometry. Only runs when a consumer needs it (game/ parity: tessellation
// or shaded wireframe); the normal draw path keeps in-shader skinning.
// Deviation from game/: skin matrices come from the shared per-frame arena
// ring, so the push constants carry each mesh's arena offset.

namespace GpuSkinning
{
	constexpr u32 WORKGROUP_SIZE = 64;
	constexpr u32 MAX_COMPUTE_GROUPS_PER_DISPATCH = 65535;
	constexpr u32 MAX_SKINNED_DISPATCH_SETS_PER_FRAME = 256;

	struct SkinningParams
	{
		i32 vertex_count = 0;
		i32 base_vertex = 0;
		i32 skin_matrix_offset = 0;
		i32 _padding0 = 0;
	};
	static_assert(sizeof(SkinningParams) == 16, "Must match gpu_skinning.comp's push constant block");

	inline VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	inline VkDescriptorPool pools[MAX_FRAMES_IN_FLIGHT] = {};	// reset each frame
	inline VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	inline VkPipeline pipeline = VK_NULL_HANDLE;

	inline void init(VulkanContext* ctx)
	{
		// Set layout: b0 source vertices, b1 skinning weights, b2 skin matrix
		// arena, b3 cache output — all compute SSBOs
		{
			VkDescriptorSetLayoutBinding bindings[4] = {};
			for (u32 binding_idx = 0; binding_idx < 4; ++binding_idx)
			{
				bindings[binding_idx] = (VkDescriptorSetLayoutBinding) {
					.binding = binding_idx,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
				};
			}
			VkDescriptorSetLayoutCreateInfo layout_create_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.bindingCount = 4,
				.pBindings = bindings,
			};
			VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &layout_create_info, nullptr, &set_layout));
		}

		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			VkDescriptorPoolSize pool_size = {
				.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 4 * MAX_SKINNED_DISPATCH_SETS_PER_FRAME,
			};
			VkDescriptorPoolCreateInfo pool_create_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.maxSets = MAX_SKINNED_DISPATCH_SETS_PER_FRAME,
				.poolSizeCount = 1,
				.pPoolSizes = &pool_size,
			};
			VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_create_info, nullptr, &pools[frame_idx]));
		}

		VkPushConstantRange push_constant_range = {
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.offset = 0,
			.size = sizeof(SkinningParams),
		};
		VkPipelineLayoutCreateInfo layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &set_layout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_constant_range,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &pipeline_layout));

		VkShaderModule compute_module = create_shader_module_from_file(ctx->device, "bin/shaders/gpu_skinning.comp.spv");
		VkComputePipelineCreateInfo pipeline_create_info = {
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT,
				.module = compute_module,
				.pName = "main",
			},
			.layout = pipeline_layout,
		};
		VK_CHECK(vulkan_create_compute_pipelines(ctx, 1, &pipeline_create_info, &pipeline));
		vkDestroyShaderModule(ctx->device, compute_module, nullptr);
	}

	inline void ensure_cache(Mesh& in_mesh)
	{
		if (in_mesh.skinned_vertex_cache_capacity >= in_mesh.vertex_count
			&& in_mesh.skinned_vertex_cache_buffer.is_gpu_buffer_valid())
		{
			return;
		}

		in_mesh.skinned_vertex_cache_buffer.destroy_gpu_buffer();
		in_mesh.skinned_vertex_cache_capacity = MAX(in_mesh.vertex_count, 1u);
		in_mesh.skinned_vertex_cache_buffer = GpuBuffer((GpuBufferDesc<Vertex>) {
			.data = nullptr,
			.size = sizeof(Vertex) * in_mesh.skinned_vertex_cache_capacity,
			.usage = {
				.vertex_buffer = true,
				.storage_buffer = true,
			},
			.label = "Mesh::skinned_vertex_cache_buffer",
		});
	}

	// Records the cache dispatch for one mesh into the frame's command buffer
	inline void update_mesh(VulkanContext* ctx, State& in_state, Mesh& in_mesh)
	{
		in_mesh.skinned_vertex_cache_valid = false;
		if (!in_mesh.has_skinned_vertices
			|| in_mesh.vertex_count == 0
			|| in_mesh.skinned_vertices == nullptr
			|| in_mesh.skin_matrices == nullptr
			|| in_mesh.skin_matrix_count == 0
			|| in_mesh.skin_matrix_arena_offset < 0)
		{
			return;
		}

		ensure_cache(in_mesh);

		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];

		VkDescriptorSet set = VK_NULL_HANDLE;
		VkDescriptorSetAllocateInfo allocate_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = pools[ctx->frame_index],
			.descriptorSetCount = 1,
			.pSetLayouts = &set_layout,
		};
		VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &set));

		VkDescriptorBufferInfo buffer_infos[] = {
			{ .buffer = in_mesh.vertex_buffer.get_gpu_buffer(), .offset = 0, .range = VK_WHOLE_SIZE },
			{ .buffer = in_mesh.skinned_vertex_buffer.get_gpu_buffer(), .offset = 0, .range = VK_WHOLE_SIZE },
			{ .buffer = get_skin_matrix_arena_buffer(in_state).get_gpu_buffer(), .offset = 0, .range = VK_WHOLE_SIZE },
			{ .buffer = in_mesh.skinned_vertex_cache_buffer.get_gpu_buffer(), .offset = 0, .range = VK_WHOLE_SIZE },
		};
		VkWriteDescriptorSet writes[4] = {};
		for (u32 binding_idx = 0; binding_idx < 4; ++binding_idx)
		{
			writes[binding_idx] = (VkWriteDescriptorSet) {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = set,
				.dstBinding = binding_idx,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &buffer_infos[binding_idx],
			};
		}
		vulkan_update_descriptor_sets(ctx, 4, writes);

		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, nullptr);

		for (u32 base_vertex = 0; base_vertex < in_mesh.vertex_count; base_vertex += MAX_COMPUTE_GROUPS_PER_DISPATCH * WORKGROUP_SIZE)
		{
			const u32 remaining_vertices = in_mesh.vertex_count - base_vertex;
			const u32 dispatch_vertex_count = MIN(remaining_vertices, MAX_COMPUTE_GROUPS_PER_DISPATCH * WORKGROUP_SIZE);
			const u32 group_count = (dispatch_vertex_count + WORKGROUP_SIZE - 1u) / WORKGROUP_SIZE;

			SkinningParams params = {
				.vertex_count = (i32) in_mesh.vertex_count,
				.base_vertex = (i32) base_vertex,
				.skin_matrix_offset = in_mesh.skin_matrix_arena_offset,
			};
			vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
			vulkan_cmd_dispatch(ctx, group_count, 1, 1);
		}

		in_mesh.skinned_vertex_cache_valid = true;
	}

	// Records all cache dispatches + the barrier making the caches visible to
	// vertex input/shaders. Call after begin_frame, before any pass executes.
	inline void update(VulkanContext* ctx, State& in_state, const bool in_required)
	{
		scene_ensure_indexes(in_state);
		in_state.data_oriented.frame.gpu_skinning_candidate_count += (i32) in_state.scene.indexes.skinned_mesh_object_ids.length();

		if (!in_required)
		{
			for (const i32 unique_id : in_state.scene.indexes.skinned_mesh_object_ids)
			{
				auto found = in_state.scene.objects.find(unique_id);
				if (found == in_state.scene.objects.end())
				{
					continue;
				}
				found->second.mesh.skinned_vertex_cache_valid = false;
			}
			return;
		}

		CPU_TIMING_SCOPE("GPU Skinning Cache");

		VK_CHECK(vkResetDescriptorPool(ctx->device, pools[ctx->frame_index], 0));

		bool dispatched_any = false;
		u32 dispatch_count = 0;
		for (const i32 unique_id : in_state.scene.indexes.skinned_mesh_object_ids)
		{
			auto found = in_state.scene.objects.find(unique_id);
			if (found == in_state.scene.objects.end())
			{
				continue;
			}
			if (dispatch_count >= MAX_SKINNED_DISPATCH_SETS_PER_FRAME)
			{
				break;
			}

			Mesh& mesh = found->second.mesh;
			update_mesh(ctx, in_state, mesh);
			if (mesh.skinned_vertex_cache_valid)
			{
				in_state.data_oriented.frame.gpu_skinning_updated_count += 1;
			}
			dispatched_any = dispatched_any || mesh.skinned_vertex_cache_valid;
			dispatch_count += 1;
		}

		if (dispatched_any)
		{
			VkMemoryBarrier2 memory_barrier = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
				.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
				.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
			};
			VkDependencyInfo dependency_info = {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.memoryBarrierCount = 1,
				.pMemoryBarriers = &memory_barrier,
			};
			vkCmdPipelineBarrier2(ctx->command_buffers[ctx->frame_index], &dependency_info);
		}
	}

	inline void shutdown(VulkanContext* ctx)
	{
		vkDestroyPipeline(ctx->device, pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, pipeline_layout, nullptr);
		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			vkDestroyDescriptorPool(ctx->device, pools[frame_idx], nullptr);
		}
		vkDestroyDescriptorSetLayout(ctx->device, set_layout, nullptr);
	}
}
