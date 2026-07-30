#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/fullscreen_pipeline.h"
#include "render/frame_data.h"

// Luma FXAA over the tonemapped LDR target (port of game/'s fxaa_pass.h).
// Uses layout B (single sampled input) + a small push constant block; the
// copy pass samples this output when enabled.

namespace FXAAPass
{
	struct PushConstants
	{
		HMM_Vec2 screen_size;
		f32 contrast_threshold;
		f32 relative_threshold;
	};
	static_assert(sizeof(PushConstants) == 16, "Must match fxaa.frag's push constant block");

	inline PerFrameDescriptorSets input_sets;
	inline VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	inline VkPipeline pipeline = VK_NULL_HANDLE;
	inline VkSampler linear_sampler = VK_NULL_HANDLE;	// borrowed from frame_data

	inline void init(VulkanContext* ctx, VkSampler in_linear_sampler)
	{
		linear_sampler = in_linear_sampler;

		input_sets.init_persistent(ctx, frame_data.sampled_input_layout);

		VkPushConstantRange push_constant_range = {
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			.offset = 0,
			.size = sizeof(PushConstants),
		};
		VkPipelineLayoutCreateInfo layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &frame_data.sampled_input_layout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_constant_range,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &pipeline_layout));

		// Same LDR format as the tonemapping target it filters
		pipeline = vulkan_create_fullscreen_pipeline(ctx, {
			.vertex_shader_path = "bin/shaders/fxaa.vert.spv",
			.fragment_shader_path = "bin/shaders/fxaa.frag.spv",
			.pipeline_layout = pipeline_layout,
			.color_formats = &ctx->surface_format.format,
			.color_format_count = 1,
		});
	}

	// Points this frame's input set at the tonemapped LDR target
	inline void update(VulkanContext* ctx, VkImageView in_tonemapped_view)
	{
		VkDescriptorSet& set = input_sets.current(ctx);
		VkDescriptorImageInfo image_info =
			descriptor_sampled(linear_sampler, in_tonemapped_view);
		VkWriteDescriptorSet write = descriptor_write_image(
			set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &image_info);
		vulkan_update_descriptor_sets(ctx, 1, &write);
	}

	inline void draw(VulkanContext* ctx, HMM_Vec2 in_screen_size)
	{
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);

		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdBindDescriptorSets(
			command_buffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipeline_layout,
			0, 1, &input_sets.current(ctx),
			0, nullptr
		);
		// game/ parity constants (main.cpp:3567-3571)
		PushConstants push_constants = {
			.screen_size = in_screen_size,
			.contrast_threshold = 0.0312f,
			.relative_threshold = 0.125f,
		};
		vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants), &push_constants);
		vulkan_cmd_draw(ctx, 3, 1, 0, 0);
	}

	inline void shutdown(VulkanContext* ctx)
	{
		vkDestroyPipeline(ctx->device, pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, pipeline_layout, nullptr);
	}
}
