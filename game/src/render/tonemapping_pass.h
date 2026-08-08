#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/fullscreen_pipeline.h"
#include "render/frame_data.h"
#include "render/gt7_tonemapping.h"

// Global operators remain one fullscreen draw. Exposure Fusion Local adds a
// half-resolution proxy pyramid, reconstructs down to quarter resolution, and
// guided-upsamples the result during the same final fullscreen draw.

struct TonemappingFinalPushConstants
{
	i32 method;
	i32 local_enabled;
	f32 exposure_bias;
	f32 bloom_intensity;
	HMM_Vec2 guide_pixel_size;
};
static_assert(sizeof(TonemappingFinalPushConstants) == 24);

struct TonemappingLocalProxyPushConstants
{
	HMM_Vec2 source_pixel_size;
	f32 exposure_bias;
	f32 shadow_recovery;
	f32 highlight_recovery;
	f32 preference_sigma;
	i32 method;
};
static_assert(sizeof(TonemappingLocalProxyPushConstants) == 28);

struct TonemappingLocalDownsamplePushConstants
{
	HMM_Vec2 source_pixel_size;
};
static_assert(sizeof(TonemappingLocalDownsamplePushConstants) == 8);

struct TonemappingLocalReconstructPushConstants
{
	i32 boost_local_contrast;
};
static_assert(sizeof(TonemappingLocalReconstructPushConstants) == 4);

struct TonemappingPass
{
	VkDescriptorSetLayout final_set_layout = VK_NULL_HANDLE;
	VkDescriptorSetLayout local_set_layout = VK_NULL_HANDLE;
	PerFrameDescriptorSets final_sets;

	VkPipelineLayout final_pipeline_layout = VK_NULL_HANDLE;
	VkPipelineLayout local_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline final_pipeline = VK_NULL_HANDLE;
	VkPipeline local_proxy_pipeline = VK_NULL_HANDLE;
	VkPipeline local_downsample_pipeline = VK_NULL_HANDLE;
	VkPipeline local_blend_pipeline = VK_NULL_HANDLE;
	VkPipeline local_reconstruct_pipeline = VK_NULL_HANDLE;

	GpuImage local_exposure_pyramid;
	GpuImage local_weight_pyramid;
	GpuImage local_reconstruction_pyramid;
	GpuImage gt7_lut;
	u32 local_base_width = 0;
	u32 local_base_height = 0;
	u32 local_mip_count = 0;

	VkImageView scene_color_view = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	i32 effective_coarsest_mip = 1;
	i32 effective_reconstruction_mip = 1;
};

static TonemappingPass tonemapping_pass;

inline u32 tonemapping_local_mip_extent(u32 in_base_extent, u32 in_mip)
{
	return MAX(1u, in_base_extent >> in_mip);
}

inline u32 tonemapping_local_mip_count(u32 in_width, u32 in_height)
{
	u32 mip_count = 1;
	while (in_width > 1 && in_height > 1)
	{
		in_width = MAX(1u, in_width >> 1);
		in_height = MAX(1u, in_height >> 1);
		++mip_count;
	}
	return mip_count;
}

inline VkImageView tonemapping_mip_view(GpuImage& in_image, u32 in_mip)
{
	return in_image.mip_levels > 1 ? in_image.mip_views[in_mip] : in_image.view;
}

inline i32 tonemapping_pass_get_max_full_resolution_mip()
{
	return tonemapping_pass.local_mip_count > 0
		? (i32)tonemapping_pass.local_mip_count
		: 1;
}

inline i32 tonemapping_pass_get_effective_coarsest_mip()
{
	return tonemapping_pass.effective_coarsest_mip;
}

inline i32 tonemapping_pass_get_effective_reconstruction_mip()
{
	return tonemapping_pass.effective_reconstruction_mip;
}

inline void tonemapping_create_sampled_layout(
	VulkanContext* ctx,
	u32 in_binding_count,
	VkDescriptorSetLayout* out_layout)
{
	VkDescriptorSetLayoutBinding bindings[5] = {};
	assert(in_binding_count <= 5);
	for (u32 binding_idx = 0; binding_idx < in_binding_count; ++binding_idx)
	{
		bindings[binding_idx] = {
			.binding = binding_idx,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		};
	}
	VkDescriptorSetLayoutCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = in_binding_count,
		.pBindings = bindings,
	};
	VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &create_info, nullptr, out_layout));
}

inline VkPipelineLayout tonemapping_create_pipeline_layout(
	VulkanContext* ctx,
	VkDescriptorSetLayout in_set_layout,
	u32 in_push_constant_size)
{
	VkPushConstantRange push_constant_range = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = in_push_constant_size,
	};
	VkPipelineLayoutCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &in_set_layout,
		.pushConstantRangeCount = in_push_constant_size > 0 ? 1u : 0u,
		.pPushConstantRanges = in_push_constant_size > 0 ? &push_constant_range : nullptr,
	};
	VkPipelineLayout result = VK_NULL_HANDLE;
	VK_CHECK(vkCreatePipelineLayout(ctx->device, &create_info, nullptr, &result));
	return result;
}

void tonemapping_pass_init(VulkanContext* ctx)
{
	tonemapping_create_sampled_layout(ctx, 5, &tonemapping_pass.final_set_layout);
	tonemapping_create_sampled_layout(ctx, 4, &tonemapping_pass.local_set_layout);
	tonemapping_pass.final_sets.init_persistent(ctx, tonemapping_pass.final_set_layout);

	DynamicArray<u16> gt7_lut_pixels = GT7Tonemapping::generate_sdr_lut();
	tonemapping_pass.gt7_lut = gpu_image_create_from_data(
		ctx,
		GT7Tonemapping::LUT_RESOLUTION,
		GT7Tonemapping::LUT_RESOLUTION,
		VK_FORMAT_R16G16B16A16_SFLOAT,
		gt7_lut_pixels.data(),
		gt7_lut_pixels.length() * sizeof(u16),
		GT7Tonemapping::LUT_RESOLUTION,
		"GT7 SDR Tone Mapping LUT");

	tonemapping_pass.final_pipeline_layout = tonemapping_create_pipeline_layout(
		ctx, tonemapping_pass.final_set_layout, sizeof(TonemappingFinalPushConstants));
	tonemapping_pass.local_pipeline_layout = tonemapping_create_pipeline_layout(
		ctx, tonemapping_pass.local_set_layout, sizeof(TonemappingLocalProxyPushConstants));

	tonemapping_pass.final_pipeline = vulkan_create_fullscreen_pipeline(ctx, {
		.vertex_shader_path = "bin/shaders/tonemapping.vert.spv",
		.fragment_shader_path = "bin/shaders/tonemapping.frag.spv",
		.pipeline_layout = tonemapping_pass.final_pipeline_layout,
		.color_formats = &Render::SCENE_COLOR_FORMAT,
		.color_format_count = 1,
	});

	VkFormat two_color_formats[2] = {
		Render::SCENE_COLOR_FORMAT,
		Render::SCENE_COLOR_FORMAT,
	};
	tonemapping_pass.local_proxy_pipeline = vulkan_create_fullscreen_pipeline(ctx, {
		.vertex_shader_path = "bin/shaders/tonemapping.vert.spv",
		.fragment_shader_path = "bin/shaders/tonemapping_local_proxy.frag.spv",
		.pipeline_layout = tonemapping_pass.local_pipeline_layout,
		.color_formats = two_color_formats,
		.color_format_count = 2,
	});
	tonemapping_pass.local_downsample_pipeline = vulkan_create_fullscreen_pipeline(ctx, {
		.vertex_shader_path = "bin/shaders/tonemapping.vert.spv",
		.fragment_shader_path = "bin/shaders/tonemapping_local_downsample.frag.spv",
		.pipeline_layout = tonemapping_pass.local_pipeline_layout,
		.color_formats = two_color_formats,
		.color_format_count = 2,
	});
	tonemapping_pass.local_blend_pipeline = vulkan_create_fullscreen_pipeline(ctx, {
		.vertex_shader_path = "bin/shaders/tonemapping.vert.spv",
		.fragment_shader_path = "bin/shaders/tonemapping_local_blend.frag.spv",
		.pipeline_layout = tonemapping_pass.local_pipeline_layout,
		.color_formats = &Render::SCENE_COLOR_FORMAT,
		.color_format_count = 1,
	});
	tonemapping_pass.local_reconstruct_pipeline = vulkan_create_fullscreen_pipeline(ctx, {
		.vertex_shader_path = "bin/shaders/tonemapping.vert.spv",
		.fragment_shader_path = "bin/shaders/tonemapping_local_reconstruct.frag.spv",
		.pipeline_layout = tonemapping_pass.local_pipeline_layout,
		.color_formats = &Render::SCENE_COLOR_FORMAT,
		.color_format_count = 1,
	});
}

void tonemapping_pass_release_local_images(VulkanContext* ctx)
{
	vulkan_context_retire_image(ctx, tonemapping_pass.local_exposure_pyramid);
	vulkan_context_retire_image(ctx, tonemapping_pass.local_weight_pyramid);
	vulkan_context_retire_image(ctx, tonemapping_pass.local_reconstruction_pyramid);
	tonemapping_pass.local_base_width = 0;
	tonemapping_pass.local_base_height = 0;
	tonemapping_pass.local_mip_count = 0;
}

void tonemapping_pass_handle_resize(VulkanContext* ctx, i32 in_width, i32 in_height)
{
	const u32 base_width = MAX(1u, ((u32)MAX(in_width, 1) + 1u) / 2u);
	const u32 base_height = MAX(1u, ((u32)MAX(in_height, 1) + 1u) / 2u);
	if (
		tonemapping_pass.local_exposure_pyramid.image != VK_NULL_HANDLE &&
		tonemapping_pass.local_base_width == base_width &&
		tonemapping_pass.local_base_height == base_height
	)
	{
		return;
	}

	tonemapping_pass_release_local_images(ctx);
	tonemapping_pass.local_base_width = base_width;
	tonemapping_pass.local_base_height = base_height;
	tonemapping_pass.local_mip_count = tonemapping_local_mip_count(base_width, base_height);

	const VkImageUsageFlags usage =
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	tonemapping_pass.local_exposure_pyramid = gpu_image_create(
		ctx->allocator, ctx->device, {
			.width = base_width,
			.height = base_height,
			.format = Render::SCENE_COLOR_FORMAT,
			.usage = usage,
			.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
			.mip_levels = tonemapping_pass.local_mip_count,
			.label = "Tonemapping Local Exposure Pyramid",
		});
	tonemapping_pass.local_weight_pyramid = gpu_image_create(
		ctx->allocator, ctx->device, {
			.width = base_width,
			.height = base_height,
			.format = Render::SCENE_COLOR_FORMAT,
			.usage = usage,
			.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
			.mip_levels = tonemapping_pass.local_mip_count,
			.label = "Tonemapping Local Weight Pyramid",
		});
	tonemapping_pass.local_reconstruction_pyramid = gpu_image_create(
		ctx->allocator, ctx->device, {
			.width = base_width,
			.height = base_height,
			.format = Render::SCENE_COLOR_FORMAT,
			.usage = usage,
			.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
			.mip_levels = tonemapping_pass.local_mip_count,
			.label = "Tonemapping Local Reconstruction Pyramid",
		});
}

inline VkDescriptorSet tonemapping_local_descriptor_set(
	VulkanContext* ctx,
	const VkImageView* in_views,
	u32 in_view_count)
{
	assert(in_view_count <= 4);
	VkDescriptorSet set =
		vulkan_allocate_transient_descriptor_set(ctx, tonemapping_pass.local_set_layout);
	VkDescriptorImageInfo image_infos[4] = {};
	VkWriteDescriptorSet writes[4] = {};
	for (u32 view_idx = 0; view_idx < in_view_count; ++view_idx)
	{
		image_infos[view_idx] = descriptor_sampled(tonemapping_pass.sampler, in_views[view_idx]);
		writes[view_idx] = descriptor_write_image(
			set,
			view_idx,
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			&image_infos[view_idx]);
	}
	vulkan_update_descriptor_sets(ctx, in_view_count, writes, 0, nullptr, false);
	return set;
}

inline ImageUsage tonemapping_mip_usage(
	GpuImage* in_image,
	u32 in_mip,
	VkPipelineStageFlags2 in_stage,
	VkAccessFlags2 in_access,
	VkImageLayout in_layout,
	bool in_discard = false)
{
	return {
		.image = in_image,
		.range = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = in_mip,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
		.stage = in_stage,
		.access = in_access,
		.layout = in_layout,
		.discard = in_discard,
	};
}

inline void tonemapping_begin_mip_render(
	VulkanContext* ctx,
	GpuImage** in_outputs,
	u32 in_output_count,
	u32 in_mip)
{
	assert(in_output_count >= 1 && in_output_count <= 2);
	VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
	ImageUsage output_usages[2] = {};
	VkRenderingAttachmentInfo attachments[2] = {};
	for (u32 output_idx = 0; output_idx < in_output_count; ++output_idx)
	{
		output_usages[output_idx] = tonemapping_mip_usage(
			in_outputs[output_idx],
			in_mip,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			true);
		attachments[output_idx] = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = tonemapping_mip_view(*in_outputs[output_idx], in_mip),
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		};
	}
	gpu_image_apply_usages(command_buffer, output_usages, in_output_count);

	const u32 width = tonemapping_local_mip_extent(tonemapping_pass.local_base_width, in_mip);
	const u32 height = tonemapping_local_mip_extent(tonemapping_pass.local_base_height, in_mip);
	VkRenderingInfo rendering_info = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = {
			.offset = {0, 0},
			.extent = {width, height},
		},
		.layerCount = 1,
		.colorAttachmentCount = in_output_count,
		.pColorAttachments = attachments,
	};
	vkCmdBeginRendering(command_buffer, &rendering_info);

	VkViewport viewport = {
		.x = 0.0f,
		.y = (f32)height,
		.width = (f32)width,
		.height = -(f32)height,
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};
	VkRect2D scissor = {
		.offset = {0, 0},
		.extent = {width, height},
	};
	vkCmdSetViewport(command_buffer, 0, 1, &viewport);
	vkCmdSetScissor(command_buffer, 0, 1, &scissor);
}

inline void tonemapping_end_mip_render(VulkanContext* ctx)
{
	vkCmdEndRendering(vulkan_current_command_buffer(ctx));
}

inline void tonemapping_draw_local_stage(
	VulkanContext* ctx,
	VkPipeline in_pipeline,
	VkDescriptorSet in_set,
	const void* in_push_constants = nullptr,
	u32 in_push_constant_size = 0)
{
	VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, in_pipeline);
	vkCmdBindDescriptorSets(
		command_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		tonemapping_pass.local_pipeline_layout,
		0, 1, &in_set,
		0, nullptr);
	if (in_push_constants && in_push_constant_size > 0)
	{
		vkCmdPushConstants(
			command_buffer,
			tonemapping_pass.local_pipeline_layout,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			0, in_push_constant_size, in_push_constants);
	}
	vulkan_cmd_draw(ctx, 3, 1, 0, 0);
}

void tonemapping_pass_update(
	VulkanContext* ctx,
	VkImageView in_scene_color_view,
	VkImageView in_bloom_view,
	f32 in_bloom_intensity,
	VkSampler in_sampler)
{
	tonemapping_pass.scene_color_view = in_scene_color_view;
	tonemapping_pass.sampler = in_sampler;

	// Bind the source as a valid placeholder for local-only bindings. Local
	// preparation replaces them before the final draw.
	VkDescriptorSet& set = tonemapping_pass.final_sets.current(ctx);
	VkDescriptorImageInfo infos[5] = {
		descriptor_sampled(in_sampler, in_scene_color_view),
		descriptor_sampled(in_sampler, in_scene_color_view),
		descriptor_sampled(in_sampler, in_scene_color_view),
		descriptor_sampled(
			in_sampler,
			in_bloom_intensity > 0.0f ? in_bloom_view : in_scene_color_view),
		descriptor_sampled(in_sampler, tonemapping_pass.gt7_lut.view),
	};
	VkWriteDescriptorSet writes[5] = {};
	for (u32 binding = 0; binding < 5; ++binding)
	{
		writes[binding] = descriptor_write_image(
			set, binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &infos[binding]);
	}
	vulkan_update_descriptor_sets(ctx, 5, writes);
}

void tonemapping_pass_prepare_local(
	VulkanContext* ctx,
	const State::TonemappingState& in_state)
{
	assert(tonemapping_pass.local_exposure_pyramid.image != VK_NULL_HANDLE);
	assert(tonemapping_pass.local_mip_count > 0);

	const i32 max_full_mip = tonemapping_pass_get_max_full_resolution_mip();
	const i32 minimum_reconstruction_mip = MIN(2, max_full_mip);
	tonemapping_pass.effective_reconstruction_mip = CLAMP(
		in_state.local_reconstruction_mip,
		minimum_reconstruction_mip,
		max_full_mip);
	tonemapping_pass.effective_coarsest_mip = CLAMP(
		in_state.local_coarsest_mip,
		tonemapping_pass.effective_reconstruction_mip,
		max_full_mip);

	const u32 reconstruction_mip =
		(u32)MAX(tonemapping_pass.effective_reconstruction_mip - 1, 0);
	const u32 coarsest_mip =
		(u32)MAX(tonemapping_pass.effective_coarsest_mip - 1, 0);
	VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);

	// Half-resolution synthetic exposures and their well-exposedness weights.
	{
		CPU_TIMING_SCOPE("Tonemapping Local Proxy");
		const i32 timing_slot =
			gpu_timestamps_begin_scope(ctx, "Tonemapping Local Proxy");
		vulkan_begin_debug_label(ctx, "Tonemapping Local Proxy");

		VkImageView views[] = {
			tonemapping_pass.scene_color_view,
			tonemapping_pass.gt7_lut.view,
		};
		VkDescriptorSet set = tonemapping_local_descriptor_set(ctx, views, 2);
		GpuImage* outputs[] = {
			&tonemapping_pass.local_exposure_pyramid,
			&tonemapping_pass.local_weight_pyramid,
		};
		tonemapping_begin_mip_render(ctx, outputs, 2, 0);
		TonemappingLocalProxyPushConstants constants = {
			.source_pixel_size = HMM_V2(
				0.5f / (f32)tonemapping_pass.local_base_width,
				0.5f / (f32)tonemapping_pass.local_base_height),
			.exposure_bias = in_state.exposure_bias,
			.shadow_recovery = in_state.local_shadow_recovery,
			.highlight_recovery = in_state.local_highlight_recovery,
			.preference_sigma = in_state.local_exposure_preference_sigma,
			.method = (i32)in_state.method,
		};
		tonemapping_draw_local_stage(
			ctx,
			tonemapping_pass.local_proxy_pipeline,
			set,
			&constants,
			sizeof(constants));
		tonemapping_end_mip_render(ctx);

		vulkan_end_debug_label(ctx);
		gpu_timestamps_end_scope(ctx, timing_slot);
	}

	// Build only as far as the selected coarsest level.
	if (coarsest_mip > 0)
	{
		CPU_TIMING_SCOPE("Tonemapping Local Pyramid");
		const i32 timing_slot =
			gpu_timestamps_begin_scope(ctx, "Tonemapping Local Pyramid");
		vulkan_begin_debug_label(ctx, "Tonemapping Local Pyramid");
		for (u32 mip = 1; mip <= coarsest_mip; ++mip)
		{
			ImageUsage input_usages[] = {
				tonemapping_mip_usage(
					&tonemapping_pass.local_exposure_pyramid, mip - 1,
					VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
					VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
				tonemapping_mip_usage(
					&tonemapping_pass.local_weight_pyramid, mip - 1,
					VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
					VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
			};
			gpu_image_apply_usages(command_buffer, input_usages, 2);

			VkImageView views[] = {
				tonemapping_mip_view(tonemapping_pass.local_exposure_pyramid, mip - 1),
				tonemapping_mip_view(tonemapping_pass.local_weight_pyramid, mip - 1),
			};
			VkDescriptorSet set = tonemapping_local_descriptor_set(ctx, views, 2);
			GpuImage* outputs[] = {
				&tonemapping_pass.local_exposure_pyramid,
				&tonemapping_pass.local_weight_pyramid,
			};
			tonemapping_begin_mip_render(ctx, outputs, 2, mip);
			TonemappingLocalDownsamplePushConstants constants = {
				.source_pixel_size = HMM_V2(
					1.0f / (f32)tonemapping_local_mip_extent(
						tonemapping_pass.local_base_width, mip - 1),
					1.0f / (f32)tonemapping_local_mip_extent(
						tonemapping_pass.local_base_height, mip - 1)),
			};
			tonemapping_draw_local_stage(
				ctx,
				tonemapping_pass.local_downsample_pipeline,
				set,
				&constants,
				sizeof(constants));
			tonemapping_end_mip_render(ctx);
		}
		vulkan_end_debug_label(ctx);
		gpu_timestamps_end_scope(ctx, timing_slot);
	}

	// Gaussian blend at the coarsest level.
	{
		CPU_TIMING_SCOPE("Tonemapping Local Reconstruct");
		const i32 timing_slot =
			gpu_timestamps_begin_scope(ctx, "Tonemapping Local Reconstruct");
		vulkan_begin_debug_label(ctx, "Tonemapping Local Reconstruct");
		ImageUsage coarse_inputs[] = {
			tonemapping_mip_usage(
				&tonemapping_pass.local_exposure_pyramid, coarsest_mip,
				VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
				VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
			tonemapping_mip_usage(
				&tonemapping_pass.local_weight_pyramid, coarsest_mip,
				VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
				VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
		};
		gpu_image_apply_usages(command_buffer, coarse_inputs, 2);

		VkImageView coarse_views[] = {
			tonemapping_mip_view(tonemapping_pass.local_exposure_pyramid, coarsest_mip),
			tonemapping_mip_view(tonemapping_pass.local_weight_pyramid, coarsest_mip),
		};
		VkDescriptorSet coarse_set =
			tonemapping_local_descriptor_set(ctx, coarse_views, 2);
		GpuImage* reconstruction_output[] = {
			&tonemapping_pass.local_reconstruction_pyramid,
		};
		tonemapping_begin_mip_render(
			ctx, reconstruction_output, 1, coarsest_mip);
		tonemapping_draw_local_stage(
			ctx, tonemapping_pass.local_blend_pipeline, coarse_set);
		tonemapping_end_mip_render(ctx);

		for (u32 coarse_mip = coarsest_mip;
			coarse_mip > reconstruction_mip;
			--coarse_mip)
		{
			const u32 fine_mip = coarse_mip - 1;
			ImageUsage reconstruct_inputs[] = {
				tonemapping_mip_usage(
					&tonemapping_pass.local_exposure_pyramid, fine_mip,
					VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
					VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
				tonemapping_mip_usage(
					&tonemapping_pass.local_weight_pyramid, fine_mip,
					VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
					VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
				tonemapping_mip_usage(
					&tonemapping_pass.local_exposure_pyramid, coarse_mip,
					VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
					VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
				tonemapping_mip_usage(
					&tonemapping_pass.local_reconstruction_pyramid, coarse_mip,
					VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
					VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
			};
			gpu_image_apply_usages(command_buffer, reconstruct_inputs, 4);

			VkImageView reconstruct_views[] = {
				tonemapping_mip_view(tonemapping_pass.local_exposure_pyramid, fine_mip),
				tonemapping_mip_view(tonemapping_pass.local_weight_pyramid, fine_mip),
				tonemapping_mip_view(tonemapping_pass.local_exposure_pyramid, coarse_mip),
				tonemapping_mip_view(tonemapping_pass.local_reconstruction_pyramid, coarse_mip),
			};
			VkDescriptorSet reconstruct_set =
				tonemapping_local_descriptor_set(ctx, reconstruct_views, 4);
			tonemapping_begin_mip_render(
				ctx, reconstruction_output, 1, fine_mip);
			TonemappingLocalReconstructPushConstants constants = {
				.boost_local_contrast =
					in_state.local_contrast_boost ? 1 : 0,
			};
			tonemapping_draw_local_stage(
				ctx,
				tonemapping_pass.local_reconstruct_pipeline,
				reconstruct_set,
				&constants,
				sizeof(constants));
			tonemapping_end_mip_render(ctx);
		}

		vulkan_end_debug_label(ctx);
		gpu_timestamps_end_scope(ctx, timing_slot);
	}

	ImageUsage final_inputs[] = {
		tonemapping_mip_usage(
			&tonemapping_pass.local_exposure_pyramid, reconstruction_mip,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
		tonemapping_mip_usage(
			&tonemapping_pass.local_reconstruction_pyramid, reconstruction_mip,
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
	};
	gpu_image_apply_usages(command_buffer, final_inputs, 2);

	VkDescriptorSet& final_set = tonemapping_pass.final_sets.current(ctx);
	VkDescriptorImageInfo infos[] = {
		descriptor_sampled(
			tonemapping_pass.sampler,
			tonemapping_mip_view(tonemapping_pass.local_exposure_pyramid, reconstruction_mip)),
		descriptor_sampled(
			tonemapping_pass.sampler,
			tonemapping_mip_view(tonemapping_pass.local_reconstruction_pyramid, reconstruction_mip)),
	};
	VkWriteDescriptorSet writes[] = {
		descriptor_write_image(
			final_set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &infos[0]),
		descriptor_write_image(
			final_set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &infos[1]),
	};
	vulkan_update_descriptor_sets(ctx, 2, writes);
}

void tonemapping_pass_draw(
	VulkanContext* ctx,
	const State::TonemappingState& in_state,
	f32 in_bloom_intensity)
{
	VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
	const i32 reconstruction_mip = MAX(
		tonemapping_pass.effective_reconstruction_mip - 1, 0);
	const u32 guide_width = tonemapping_local_mip_extent(
		tonemapping_pass.local_base_width, (u32)reconstruction_mip);
	const u32 guide_height = tonemapping_local_mip_extent(
		tonemapping_pass.local_base_height, (u32)reconstruction_mip);
	TonemappingFinalPushConstants constants = {
		.method = (i32)in_state.method,
		.local_enabled = in_state.local_enabled ? 1 : 0,
		.exposure_bias = in_state.exposure_bias,
		.bloom_intensity = CLAMP(in_bloom_intensity, 0.0f, 1.0f),
		.guide_pixel_size = HMM_V2(
			1.0f / (f32)MAX(guide_width, 1u),
			1.0f / (f32)MAX(guide_height, 1u)),
	};

	vkCmdBindPipeline(
		command_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		tonemapping_pass.final_pipeline);
	vkCmdBindDescriptorSets(
		command_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		tonemapping_pass.final_pipeline_layout,
		0, 1, &tonemapping_pass.final_sets.current(ctx),
		0, nullptr);
	vkCmdPushConstants(
		command_buffer,
		tonemapping_pass.final_pipeline_layout,
		VK_SHADER_STAGE_FRAGMENT_BIT,
		0, sizeof(constants), &constants);
	vulkan_cmd_draw(ctx, 3, 1, 0, 0);
}

void tonemapping_pass_shutdown(VulkanContext* ctx)
{
	gpu_image_destroy(ctx->allocator, ctx->device, tonemapping_pass.gt7_lut);
	gpu_image_destroy(
		ctx->allocator, ctx->device, tonemapping_pass.local_exposure_pyramid);
	gpu_image_destroy(
		ctx->allocator, ctx->device, tonemapping_pass.local_weight_pyramid);
	gpu_image_destroy(
		ctx->allocator, ctx->device, tonemapping_pass.local_reconstruction_pyramid);

	vkDestroyPipeline(ctx->device, tonemapping_pass.local_reconstruct_pipeline, nullptr);
	vkDestroyPipeline(ctx->device, tonemapping_pass.local_blend_pipeline, nullptr);
	vkDestroyPipeline(ctx->device, tonemapping_pass.local_downsample_pipeline, nullptr);
	vkDestroyPipeline(ctx->device, tonemapping_pass.local_proxy_pipeline, nullptr);
	vkDestroyPipeline(ctx->device, tonemapping_pass.final_pipeline, nullptr);
	vkDestroyPipelineLayout(ctx->device, tonemapping_pass.local_pipeline_layout, nullptr);
	vkDestroyPipelineLayout(ctx->device, tonemapping_pass.final_pipeline_layout, nullptr);
	vkDestroyDescriptorSetLayout(ctx->device, tonemapping_pass.local_set_layout, nullptr);
	vkDestroyDescriptorSetLayout(ctx->device, tonemapping_pass.final_set_layout, nullptr);
}
