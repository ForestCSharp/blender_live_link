#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/fullscreen_pipeline.h"
#include "render/gpu_buffer.h"

// Single-pass gather depth-of-field (port of game/'s DOF combine pass desc,
// main.cpp:1573-1594 + dof_combine.glsl). Reads the post-fog scene color +
// G-buffer world position; writes the DOF'd scene color.

// Mirrors dof_combine.frag's fs_params / game/'s dof_combine_fs_params_t
struct DofCombineFsParams
{
	HMM_Vec4 cam_pos;
	HMM_Vec4 cam_forward;
	HMM_Vec2 screen_size;
	f32 focus_distance;
	f32 focus_range;
	f32 max_coc_radius;
	f32 foreground_blur_scale;
	f32 background_blur_scale;
	i32 debug_mode;
};
static_assert(sizeof(DofCombineFsParams) == 64, "Must match game/'s dof_combine_fs_params_t std140 layout");

struct DofCombinePass
{
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	PerFrameDescriptorSets sets;

	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;

	PerFrameUniform<DofCombineFsParams> fs_params;

	VkSampler linear_sampler = VK_NULL_HANDLE;	// borrowed from frame_data
};

static DofCombinePass dof_combine_pass;

void dof_combine_pass_init(VulkanContext* ctx, VkSampler in_linear_sampler)
{
	dof_combine_pass.linear_sampler = in_linear_sampler;

	// Set layout: b0 fs_params UBO, b1 scene color CIS, b2 world position CIS
	{
		VkDescriptorSetLayoutBinding bindings[3] = {};
		bindings[0] = (VkDescriptorSetLayoutBinding) {
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		};
		for (u32 binding_idx = 1; binding_idx <= 2; ++binding_idx)
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
			.bindingCount = 3,
			.pBindings = bindings,
		};
		VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &layout_create_info, nullptr, &dof_combine_pass.set_layout));
	}

	dof_combine_pass.sets.init_persistent(ctx, dof_combine_pass.set_layout);
	dof_combine_pass.fs_params.init("DofCombinePass::fs_params");

	// Pipeline
	{
		VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &dof_combine_pass.set_layout,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &pipeline_layout_create_info, nullptr, &dof_combine_pass.pipeline_layout));

			VkFormat color_format = Render::SCENE_COLOR_FORMAT;
			dof_combine_pass.pipeline = vulkan_create_fullscreen_pipeline(ctx, {
				.vertex_shader_path = "bin/shaders/dof_combine.vert.spv",
				.fragment_shader_path = "bin/shaders/dof_combine.frag.spv",
				.pipeline_layout = dof_combine_pass.pipeline_layout,
				.color_formats = &color_format,
				.color_format_count = 1,
			});
		}
}

// Uploads fs_params + rewrites this frame's set (after the fence wait, before
// any binds record)
void dof_combine_pass_update(
	VulkanContext* ctx,
	const DofCombineFsParams& in_fs_params,
	VkImageView in_scene_color_view,
	VkImageView in_gbuffer_position_view
)
{
	VkDescriptorSet& set = dof_combine_pass.sets.current(ctx);
	VkDescriptorBufferInfo ubo_info = descriptor_buffer(
		dof_combine_pass.fs_params.update(ctx, in_fs_params),
		sizeof(DofCombineFsParams)
	);
	VkDescriptorImageInfo image_infos[] = {
		descriptor_sampled(dof_combine_pass.linear_sampler, in_scene_color_view),
		descriptor_sampled(dof_combine_pass.linear_sampler, in_gbuffer_position_view),
	};

	VkWriteDescriptorSet writes[3] = {};
	writes[0] =
		descriptor_write_buffer(set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &ubo_info);
	for (u32 image_idx = 0; image_idx < 2; ++image_idx)
	{
		writes[image_idx + 1] = descriptor_write_image(
			set,
			image_idx + 1,
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			&image_infos[image_idx]
		);
	}
	vulkan_update_descriptor_sets(ctx, 3, writes);
}

void dof_combine_pass_draw(VulkanContext* ctx)
{
	VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, dof_combine_pass.pipeline);
	vkCmdBindDescriptorSets(
		command_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		dof_combine_pass.pipeline_layout,
		0, 1, &dof_combine_pass.sets.current(ctx),
		0, nullptr
	);
	vulkan_cmd_draw(ctx, 3, 1, 0, 0);
}

void dof_combine_pass_shutdown(VulkanContext* ctx)
{
	vkDestroyPipeline(ctx->device, dof_combine_pass.pipeline, nullptr);
	vkDestroyPipelineLayout(ctx->device, dof_combine_pass.pipeline_layout, nullptr);
	dof_combine_pass.fs_params.shutdown();
	vkDestroyDescriptorSetLayout(ctx->device, dof_combine_pass.set_layout, nullptr);
}
