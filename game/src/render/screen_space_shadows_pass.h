#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/render_pass.h"
#include "render/fullscreen_pipeline.h"
#include "render/gpu_buffer.h"

// Screen-space contact shadows:
// a short G-buffer ray march toward the sun (trace, intermediate pass) then
// an edge-aware smooth (filter, final pass), both at half render resolution.
// Lighting multiplies the mask into the sun's shadow visibility.

// Mirrors screen_space_shadows_trace.frag's trace_fs_params (std140)
struct SssTraceFsParams
{
	HMM_Vec2 screen_size;
	f32 _pad0[2];
	HMM_Mat4 view;
	HMM_Mat4 projection;
	HMM_Vec3 light_direction;
	f32 ray_length;
	f32 thickness;
	f32 jitter_strength;
	i32 max_steps;
	i32 enable;
};
static_assert(sizeof(SssTraceFsParams) == 176, "Must match trace_fs_params std140 layout");

// Mirrors screen_space_shadows_filter.frag's filter_fs_params (std140)
struct SssFilterFsParams
{
	HMM_Vec2 screen_size;
	i32 filter_radius;
	f32 _pad0;
};
static_assert(sizeof(SssFilterFsParams) == 16, "Must match filter_fs_params std140 layout");

namespace ScreenSpaceShadowsPass
{
	inline VkDescriptorSetLayout trace_set_layout = VK_NULL_HANDLE;
	inline VkDescriptorSetLayout filter_set_layout = VK_NULL_HANDLE;
	inline VkDescriptorPool pool = VK_NULL_HANDLE;
	inline VkDescriptorSet trace_sets[MAX_FRAMES_IN_FLIGHT] = {};
	inline VkDescriptorSet filter_sets[MAX_FRAMES_IN_FLIGHT] = {};

	inline VkPipelineLayout trace_pipeline_layout = VK_NULL_HANDLE;
	inline VkPipelineLayout filter_pipeline_layout = VK_NULL_HANDLE;
	inline VkPipeline trace_pipeline = VK_NULL_HANDLE;
	inline VkPipeline filter_pipeline = VK_NULL_HANDLE;

	inline GpuBuffer<SssTraceFsParams> trace_ubos[MAX_FRAMES_IN_FLIGHT];
	inline GpuBuffer<SssFilterFsParams> filter_ubos[MAX_FRAMES_IN_FLIGHT];

	inline VkSampler linear_sampler = VK_NULL_HANDLE;	// borrowed from frame_data

	inline VkDescriptorSetLayout create_set_layout(VulkanContext* ctx, u32 in_sampled_image_count)
	{
		VkDescriptorSetLayoutBinding bindings[4] = {};
		bindings[0] = (VkDescriptorSetLayoutBinding) {
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		};
		for (u32 binding_idx = 1; binding_idx <= in_sampled_image_count; ++binding_idx)
		{
			bindings[binding_idx] = (VkDescriptorSetLayoutBinding) {
				.binding = binding_idx,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			};
		}

		VkDescriptorSetLayoutCreateInfo layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = 1 + in_sampled_image_count,
			.pBindings = bindings,
		};
		VkDescriptorSetLayout out_layout = VK_NULL_HANDLE;
		VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &layout_create_info, nullptr, &out_layout));
		return out_layout;
	}

	inline VkPipeline create_pipeline(VulkanContext* ctx, VkPipelineLayout in_layout, const char* in_vert_path, const char* in_frag_path)
	{
		VkFormat mask_format = Render::SSAO_FORMAT;
		return vulkan_create_fullscreen_pipeline(ctx, {
			.vertex_shader_path = in_vert_path,
			.fragment_shader_path = in_frag_path,
			.pipeline_layout = in_layout,
			.color_formats = &mask_format,
			.color_format_count = 1,
		});
	}

	inline void init(VulkanContext* ctx, VkSampler in_linear_sampler)
	{
		linear_sampler = in_linear_sampler;

		trace_set_layout = create_set_layout(ctx, 2);
		filter_set_layout = create_set_layout(ctx, 3);

		VkDescriptorPoolSize pool_sizes[] = {
			{ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 2 * MAX_FRAMES_IN_FLIGHT },
			{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 5 * MAX_FRAMES_IN_FLIGHT },
		};
		VkDescriptorPoolCreateInfo pool_create_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = 2 * MAX_FRAMES_IN_FLIGHT,
			.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]),
			.pPoolSizes = pool_sizes,
		};
		VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_create_info, nullptr, &pool));

		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			VkDescriptorSetAllocateInfo allocate_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &trace_set_layout,
			};
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &trace_sets[frame_idx]));

			allocate_info.pSetLayouts = &filter_set_layout;
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &filter_sets[frame_idx]));

			trace_ubos[frame_idx] = GpuBuffer((GpuBufferDesc<SssTraceFsParams>){
				.data = nullptr,
				.size = sizeof(SssTraceFsParams),
				.usage = { .uniform_buffer = true, .stream_update = true },
				.label = "ScreenSpaceShadowsPass::trace_fs_params",
			});
			filter_ubos[frame_idx] = GpuBuffer((GpuBufferDesc<SssFilterFsParams>){
				.data = nullptr,
				.size = sizeof(SssFilterFsParams),
				.usage = { .uniform_buffer = true, .stream_update = true },
				.label = "ScreenSpaceShadowsPass::filter_fs_params",
			});
		}

		{
			VkPipelineLayoutCreateInfo layout_create_info = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 1,
				.pSetLayouts = &trace_set_layout,
			};
			VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &trace_pipeline_layout));

			layout_create_info.pSetLayouts = &filter_set_layout;
			VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &filter_pipeline_layout));
		}

		trace_pipeline = create_pipeline(
			ctx, trace_pipeline_layout,
			"bin/shaders/screen_space_shadows_trace.vert.spv",
			"bin/shaders/screen_space_shadows_trace.frag.spv"
		);
		filter_pipeline = create_pipeline(
			ctx, filter_pipeline_layout,
			"bin/shaders/screen_space_shadows_filter.vert.spv",
			"bin/shaders/screen_space_shadows_filter.frag.spv"
		);
	}

	// Uploads both UBOs + rewrites this frame's sets (after the fence wait,
	// before any binds record)
	inline void update(
		VulkanContext* ctx,
		const State& in_state,
		HMM_Vec2 in_target_size,
		const HMM_Mat4& in_view,
		const HMM_Mat4& in_projection,
		HMM_Vec3 in_light_direction,
		bool in_enable,
		VkImageView in_gbuffer_position_view,
		VkImageView in_gbuffer_normal_view,
		VkImageView in_trace_output_view
	)
	{
		const u32 frame_index = ctx->frame_index;

		SssTraceFsParams trace_fs_params = {
			.screen_size = in_target_size,
			.view = in_view,
			.projection = in_projection,
			.light_direction = in_light_direction,
			.ray_length = in_state.shadow.screen_space.ray_length,
			.thickness = in_state.shadow.screen_space.thickness,
			.jitter_strength = in_state.shadow.screen_space.jitter_strength,
			.max_steps = in_state.shadow.screen_space.max_steps,
			.enable = in_enable ? 1 : 0,
		};
		trace_ubos[frame_index].update_gpu_buffer(&trace_fs_params, sizeof(trace_fs_params));

		SssFilterFsParams filter_fs_params = {
			.screen_size = in_target_size,
			.filter_radius = in_state.shadow.screen_space.filter_radius,
		};
		filter_ubos[frame_index].update_gpu_buffer(&filter_fs_params, sizeof(filter_fs_params));

		VkDescriptorBufferInfo trace_ubo_info = {
			.buffer = trace_ubos[frame_index].get_gpu_buffer(),
			.offset = 0,
			.range = sizeof(SssTraceFsParams),
		};
		VkDescriptorBufferInfo filter_ubo_info = {
			.buffer = filter_ubos[frame_index].get_gpu_buffer(),
			.offset = 0,
			.range = sizeof(SssFilterFsParams),
		};
		VkDescriptorImageInfo image_infos[] = {
			// trace b1, b2
			{ .sampler = linear_sampler, .imageView = in_gbuffer_position_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ .sampler = linear_sampler, .imageView = in_gbuffer_normal_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			// filter b1 (raw trace), b2 (position), b3 (normal)
			{ .sampler = linear_sampler, .imageView = in_trace_output_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ .sampler = linear_sampler, .imageView = in_gbuffer_position_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ .sampler = linear_sampler, .imageView = in_gbuffer_normal_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		};

		VkWriteDescriptorSet writes[7] = {};
		writes[0] = (VkWriteDescriptorSet) {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = trace_sets[frame_index],
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &trace_ubo_info,
		};
		for (u32 image_idx = 0; image_idx < 2; ++image_idx)
		{
			writes[1 + image_idx] = (VkWriteDescriptorSet) {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = trace_sets[frame_index],
				.dstBinding = 1 + image_idx,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image_infos[image_idx],
			};
		}
		writes[3] = (VkWriteDescriptorSet) {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = filter_sets[frame_index],
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &filter_ubo_info,
		};
		for (u32 image_idx = 0; image_idx < 3; ++image_idx)
		{
			writes[4 + image_idx] = (VkWriteDescriptorSet) {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = filter_sets[frame_index],
				.dstBinding = 1 + image_idx,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image_infos[2 + image_idx],
			};
		}
		vulkan_update_descriptor_sets(ctx, 7, writes);
	}

	inline void draw_fullscreen(VulkanContext* ctx, VkPipeline in_pipeline, VkPipelineLayout in_layout, VkDescriptorSet in_set)
	{
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, in_pipeline);
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, in_layout, 0, 1, &in_set, 0, nullptr);
		vulkan_cmd_draw(ctx, 3, 1, 0, 0);
	}

	// Trace into the intermediate target, filter into the final one. Leaves
	// both in SHADER_READ_ONLY.
	inline void execute(VulkanContext* ctx, RenderPass& trace_pass, RenderPass& filter_pass)
	{
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);

		trace_pass.execute(ctx, [&](i32)
		{
			draw_fullscreen(ctx, trace_pipeline, trace_pipeline_layout, trace_sets[ctx->frame_index]);
		});
		gpu_image_transition(command_buffer, trace_pass.get_color_output(0), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		filter_pass.execute(ctx, [&](i32)
		{
			draw_fullscreen(ctx, filter_pipeline, filter_pipeline_layout, filter_sets[ctx->frame_index]);
		});
		gpu_image_transition(command_buffer, filter_pass.get_color_output(0), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}

	inline void shutdown(VulkanContext* ctx)
	{
		vkDestroyPipeline(ctx->device, filter_pipeline, nullptr);
		vkDestroyPipeline(ctx->device, trace_pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, filter_pipeline_layout, nullptr);
		vkDestroyPipelineLayout(ctx->device, trace_pipeline_layout, nullptr);
		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			trace_ubos[frame_idx].destroy_gpu_buffer();
			filter_ubos[frame_idx].destroy_gpu_buffer();
		}
		vkDestroyDescriptorPool(ctx->device, pool, nullptr);
		vkDestroyDescriptorSetLayout(ctx->device, filter_set_layout, nullptr);
		vkDestroyDescriptorSetLayout(ctx->device, trace_set_layout, nullptr);
	}
}
