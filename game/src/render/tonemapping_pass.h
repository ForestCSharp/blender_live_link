#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/fullscreen_pipeline.h"
#include "render/frame_data.h"

// Exposure + Reinhard tonemapping: HDR scene color -> LDR target (swapchain
// format, render resolution). Uses layout B (single sampled input, written
// per frame like the copy input) + a 4-byte exposure push constant.

struct TonemappingPass
{
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;

	PerFrameDescriptorSets input_sets;
};

static TonemappingPass tonemapping_pass;

void tonemapping_pass_init(VulkanContext* ctx)
{
	// Input sets share frame_data's layout B and the persistent arena.
	tonemapping_pass.input_sets.init_persistent(ctx, frame_data.sampled_input_layout);

	VkPushConstantRange push_constant_range = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(f32),
	};

	VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &frame_data.sampled_input_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range,
	};
	VK_CHECK(vkCreatePipelineLayout(ctx->device, &pipeline_layout_create_info, nullptr, &tonemapping_pass.pipeline_layout));

	// LDR target in swapchain format so the copy pass is a plain passthrough
	tonemapping_pass.pipeline = vulkan_create_fullscreen_pipeline(ctx, {
		.vertex_shader_path = "bin/shaders/tonemapping.vert.spv",
		.fragment_shader_path = "bin/shaders/tonemapping.frag.spv",
		.pipeline_layout = tonemapping_pass.pipeline_layout,
		.color_formats = &ctx->surface_format.format,
		.color_format_count = 1,
	});
}

// Point this frame's input set at the HDR scene color (after fence wait)
void tonemapping_pass_update(VulkanContext* ctx, VkImageView in_scene_color_view, VkSampler in_sampler)
{
	VkDescriptorSet& set = tonemapping_pass.input_sets.current(ctx);
	VkDescriptorImageInfo image_info = descriptor_sampled(in_sampler, in_scene_color_view);
	VkWriteDescriptorSet write = descriptor_write_image(
		set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &image_info);
	vulkan_update_descriptor_sets(ctx, 1, &write);
}

void tonemapping_pass_draw(VulkanContext* ctx, f32 in_exposure_bias)
{
	VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapping_pass.pipeline);
	vkCmdBindDescriptorSets(
		command_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		tonemapping_pass.pipeline_layout,
		0, 1, &tonemapping_pass.input_sets.current(ctx),
		0, nullptr
	);
	vkCmdPushConstants(
		command_buffer,
		tonemapping_pass.pipeline_layout,
		VK_SHADER_STAGE_FRAGMENT_BIT,
		0, sizeof(f32), &in_exposure_bias
	);
	vulkan_cmd_draw(ctx, 3, 1, 0, 0);
}

void tonemapping_pass_shutdown(VulkanContext* ctx)
{
	vkDestroyPipeline(ctx->device, tonemapping_pass.pipeline, nullptr);
	vkDestroyPipelineLayout(ctx->device, tonemapping_pass.pipeline_layout, nullptr);
}
