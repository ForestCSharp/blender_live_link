#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/render_pass.h"
#include "render/fullscreen_pipeline.h"
#include "render/frame_data.h"

// Generic separable gaussian blur over a 2D target: horizontal
// (intermediate pass) then vertical (final pass).
// The 2D counterpart of ShadowBlurPass. Inputs resize with the window, so
// each consumer gets per-frame descriptor sets rewritten in its update.

namespace BlurPass
{
	struct PushConstants
	{
		HMM_Vec2 screen_size;
		HMM_Vec2 direction;
		i32 blur_size;
	};
	static_assert(sizeof(PushConstants) == 20, "Must match blur.frag's push constant block");

	// One pipeline shared by all users of a given color format
	inline VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	inline VkPipeline pipeline = VK_NULL_HANDLE;
	inline VkFormat pipeline_format = VK_FORMAT_UNDEFINED;

	inline void init(VulkanContext* ctx, VkFormat in_color_format)
	{
		assert(pipeline == VK_NULL_HANDLE && "single-format blur pipeline; extend to a per-format cache when needed");
		pipeline_format = in_color_format;

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

		pipeline = vulkan_create_fullscreen_pipeline(ctx, {
			.vertex_shader_path = "bin/shaders/blur.vert.spv",
			.fragment_shader_path = "bin/shaders/blur.frag.spv",
			.pipeline_layout = pipeline_layout,
			.color_formats = &pipeline_format,
			.color_format_count = 1,
		});
	}

	inline void draw_blur(VulkanContext* ctx, VkDescriptorSet in_input_set, HMM_Vec2 in_screen_size, HMM_Vec2 in_direction, i32 in_blur_size)
	{
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);

		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdBindDescriptorSets(
			command_buffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipeline_layout,
			0, 1, &in_input_set,
			0, nullptr
		);
		PushConstants push_constants = {
			.screen_size = in_screen_size,
			.direction = in_direction,
			.blur_size = in_blur_size,
		};
		vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants), &push_constants);
		vulkan_cmd_draw(ctx, 3, 1, 0, 0);
	}

	// Horizontal into the intermediate target, vertical into the final one.
	// in_horizontal_input_set samples the caller's source image (must be
	// SHADER_READ_ONLY); in_vertical_input_set samples the intermediate
	// target. Leaves both targets in SHADER_READ_ONLY.
	inline void execute_separable(
		VulkanContext* ctx,
		RenderPass& in_horizontal_pass,
		RenderPass& in_vertical_pass,
		VkDescriptorSet in_horizontal_input_set,
		VkDescriptorSet in_vertical_input_set,
		i32 in_blur_size
	)
	{
		RenderPass& horizontal_pass = in_horizontal_pass;
		RenderPass& vertical_pass = in_vertical_pass;
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);

		const HMM_Vec2 screen_size = HMM_V2((f32) horizontal_pass.current_width, (f32) horizontal_pass.current_height);

		horizontal_pass.execute(ctx, [&](i32)
		{
			draw_blur(ctx, in_horizontal_input_set, screen_size, HMM_V2(1.0f, 0.0f), in_blur_size);
		});
		gpu_image_transition(command_buffer, horizontal_pass.get_color_output(0), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vertical_pass.execute(ctx, [&](i32)
		{
			draw_blur(ctx, in_vertical_input_set, screen_size, HMM_V2(0.0f, 1.0f), in_blur_size);
		});
		gpu_image_transition(command_buffer, vertical_pass.get_color_output(0), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}

	inline void shutdown(VulkanContext* ctx)
	{
		vkDestroyPipeline(ctx->device, pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, pipeline_layout, nullptr);
	}
}
