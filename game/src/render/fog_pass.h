#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/fullscreen_pipeline.h"
#include "render/gpu_buffer.h"

// Exponential height fog over the lit scene.
// Runs after lighting when an enabled fog controller exists;
// downstream passes read this output instead of the lighting target.

// Mirrors fog.frag's fs_params block (std140).
struct FogFsParams
{
	HMM_Vec3 camera_position;
	f32 fog_base_height;
	HMM_Vec3 fog_color;
	f32 density;
	f32 scale_height;
	f32 max_distance;
	i32 ceiling_enabled;
	f32 ceiling_height;
	f32 ceiling_fade;
	f32 ambient_intensity;
	f32 sun_intensity;
	f32 anisotropy;
	HMM_Vec3 sun_direction;
	f32 _pad0;
	HMM_Vec3 sun_color;
	f32 _pad1;
};
static_assert(sizeof(FogFsParams) == 96, "FogFsParams must match fog.frag's fs_params std140 layout");

struct FogPass
{
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	PerFrameDescriptorSets sets;

	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;

	PerFrameUniform<FogFsParams> fs_params;

	VkSampler linear_sampler = VK_NULL_HANDLE;	// borrowed from frame_data
};

static FogPass fog_pass;

void fog_pass_init(VulkanContext* ctx, VkSampler in_linear_sampler)
{
	fog_pass.linear_sampler = in_linear_sampler;

	// Set layout: b0 fs_params UBO, b1 lit color CIS, b2 world position CIS
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
		VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &layout_create_info, nullptr, &fog_pass.set_layout));
	}

	fog_pass.sets.init_persistent(ctx, fog_pass.set_layout);
	fog_pass.fs_params.init("FogPass::fs_params");

	// Pipeline
	{
		VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &fog_pass.set_layout,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &pipeline_layout_create_info, nullptr, &fog_pass.pipeline_layout));

			VkFormat color_format = Render::SCENE_COLOR_FORMAT;
			fog_pass.pipeline = vulkan_create_fullscreen_pipeline(ctx, {
				.vertex_shader_path = "bin/shaders/fog.vert.spv",
				.fragment_shader_path = "bin/shaders/fog.frag.spv",
				.pipeline_layout = fog_pass.pipeline_layout,
				.color_formats = &color_format,
				.color_format_count = 1,
			});
		}
}

// Uploads fs_params + rewrites this frame's set (after the fence wait, before
// any binds record)
void fog_pass_update(
	VulkanContext* ctx,
	const FogFsParams& in_fs_params,
	VkImageView in_lit_color_view,
	VkImageView in_gbuffer_position_view
)
{
	VkDescriptorSet& set = fog_pass.sets.current(ctx);
	VkDescriptorBufferInfo ubo_info =
		descriptor_buffer(fog_pass.fs_params.update(ctx, in_fs_params), sizeof(FogFsParams));
	VkDescriptorImageInfo image_infos[] = {
		descriptor_sampled(fog_pass.linear_sampler, in_lit_color_view),
		descriptor_sampled(fog_pass.linear_sampler, in_gbuffer_position_view),
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

void fog_pass_draw(VulkanContext* ctx)
{
	VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, fog_pass.pipeline);
	vkCmdBindDescriptorSets(
		command_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		fog_pass.pipeline_layout,
		0, 1, &fog_pass.sets.current(ctx),
		0, nullptr
	);
	vulkan_cmd_draw(ctx, 3, 1, 0, 0);
}

void fog_pass_shutdown(VulkanContext* ctx)
{
	vkDestroyPipeline(ctx->device, fog_pass.pipeline, nullptr);
	vkDestroyPipelineLayout(ctx->device, fog_pass.pipeline_layout, nullptr);
	fog_pass.fs_params.shutdown();
	vkDestroyDescriptorSetLayout(ctx->device, fog_pass.set_layout, nullptr);
}
