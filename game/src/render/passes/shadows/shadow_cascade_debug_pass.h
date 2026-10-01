#pragma once

#include "render/core/frame_data.h"
#include "render/core/fullscreen_pipeline.h"
#include "render/core/render_pass.h"

namespace ShadowCascadeDebugPass
{
	struct PushConstants
	{
		i32 cascade_index;
		i32 view_mode;
	};

	inline VkDescriptorPool pool = VK_NULL_HANDLE;
	inline VkDescriptorSet sets[MAX_FRAMES_IN_FLIGHT] = {};
	inline VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	inline VkPipeline pipeline = VK_NULL_HANDLE;

	inline void init(VulkanContext* ctx)
	{
		VkDescriptorPoolSize pool_size = {
			.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = MAX_FRAMES_IN_FLIGHT,
		};
		VkDescriptorPoolCreateInfo pool_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = MAX_FRAMES_IN_FLIGHT,
			.poolSizeCount = 1,
			.pPoolSizes = &pool_size,
		};
		VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_info, nullptr, &pool));
		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			VkDescriptorSetAllocateInfo alloc_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &frame_data.sampled_input_layout,
			};
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &alloc_info, &sets[frame_idx]));
		}

		VkPushConstantRange push_range = {
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			.size = sizeof(PushConstants),
		};
		VkPipelineLayoutCreateInfo layout_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &frame_data.sampled_input_layout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_range,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_info, nullptr, &pipeline_layout));

		VkFormat output_format = Render::SCENE_COLOR_FORMAT;
		pipeline = vulkan_create_fullscreen_pipeline(ctx, {
			.vertex_shader_path = "bin/shaders/shadow_cascade_debug.vert.spv",
			.fragment_shader_path = "bin/shaders/shadow_cascade_debug.frag.spv",
			.pipeline_layout = pipeline_layout,
			.color_formats = &output_format,
			.color_format_count = 1,
		});
	}

	inline void render(
		VulkanContext* ctx,
		VkImageView in_moments_view,
		VkSampler in_sampler,
		i32 in_cascade_index,
		i32 in_view_mode)
	{
		VkDescriptorImageInfo image_info = {
			.sampler = in_sampler,
			.imageView = in_moments_view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = sets[ctx->frame_index],
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image_info,
		};
		vulkan_update_descriptor_sets(ctx, 1, &write);

		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdBindDescriptorSets(
			command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
			0, 1, &sets[ctx->frame_index], 0, nullptr);
		PushConstants push = { .cascade_index = in_cascade_index, .view_mode = in_view_mode };
		vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		vulkan_cmd_draw(ctx, 3, 1, 0, 0);
	}

	inline void shutdown(VulkanContext* ctx)
	{
		vkDestroyPipeline(ctx->device, pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, pipeline_layout, nullptr);
		vkDestroyDescriptorPool(ctx->device, pool, nullptr);
	}
}
