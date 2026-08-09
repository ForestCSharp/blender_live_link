#pragma once

#include <cmath>

#include "core/runtime_config.h"
#include "core/timings.h"
#include "render/bloom_profile.inl"
#include "render/frame_data.h"
#include "render/fullscreen_pipeline.h"
#include "render/gpu_image.h"
#include "render/render_types.h"
#include "render/vulkan_context.h"
#include "state/state.h"

// HDR bloom stores scene-linear radiance. A user-controlled influence determines
// how much automatic exposure affects thresholding and the final composite.

namespace BloomPass
{
	static constexpr i32 MAX_MIP_COUNT = BloomProfile::MAX_BAND_COUNT;

	struct DownsamplePushConstants
	{
		HMM_Vec2 source_pixel_size;
		f32 threshold;
		f32 soft_knee;
		f32 exposure_scale;
		i32 apply_threshold;
		i32 auto_exposure_enabled;
		i32 auto_white_balance_enabled;
		f32 auto_exposure_influence;
	};
	static_assert(sizeof(DownsamplePushConstants) == 36);

	struct UpsamplePushConstants
	{
		HMM_Vec4 coarse_weight_ratio;
		HMM_Vec2 source_pixel_size;
	};
	static_assert(sizeof(UpsamplePushConstants) == 32);

	struct Pass
	{
		VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline downsample_pipeline = VK_NULL_HANDLE;
		VkPipeline upsample_pipeline = VK_NULL_HANDLE;
		GpuImage pyramid;
		VkSampler linear_sampler = VK_NULL_HANDLE;
		u32 source_width = 0;
		u32 source_height = 0;
		u32 base_width = 0;
		u32 base_height = 0;
		i32 available_mip_count = 0;
		i32 effective_mip_count = 0;
		HMM_Vec4 profile_base_gain = HMM_V4(1.0f, 1.0f, 1.0f, 1.0f);
		VkBuffer auto_adaptation_state_buffer = VK_NULL_HANDLE;
	};

	inline Pass bloom_pass;

	inline u32 mip_extent(u32 in_base_extent, u32 in_mip)
	{
		return MAX(1u, in_base_extent >> in_mip);
	}

	inline i32 mip_count_for_extent(u32 in_width, u32 in_height)
	{
		i32 mip_count = 1;
		while (mip_count < MAX_MIP_COUNT && in_width > 1 && in_height > 1)
		{
			in_width = MAX(1u, in_width >> 1);
			in_height = MAX(1u, in_height >> 1);
			++mip_count;
		}
		return mip_count;
	}

	inline VkImageView mip_view(i32 in_mip)
	{
		assert(in_mip >= 0 && in_mip < bloom_pass.available_mip_count);
		return bloom_pass.pyramid.mip_levels > 1
			? bloom_pass.pyramid.mip_views[in_mip]
			: bloom_pass.pyramid.view;
	}

	inline i32 get_available_mip_count()
	{
		return MAX(1, bloom_pass.available_mip_count);
	}

	inline i32 get_effective_mip_count()
	{
		return MAX(1, bloom_pass.effective_mip_count);
	}

	inline GpuImage& get_pyramid()
	{
		return bloom_pass.pyramid;
	}

	inline HMM_Vec4 get_profile_base_gain()
	{
		return bloom_pass.profile_base_gain;
	}

	inline ImageUsage mip_usage(
		u32 in_mip,
		VkPipelineStageFlags2 in_stage,
		VkAccessFlags2 in_access,
		VkImageLayout in_layout,
		bool in_discard = false)
	{
		return {
			.image = &bloom_pass.pyramid,
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

	inline void begin_mip_render(
		VulkanContext* ctx,
		u32 in_mip,
		VkAttachmentLoadOp in_load_op,
		bool in_discard)
	{
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		ImageUsage output_usage = mip_usage(
			in_mip,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			in_discard);
		gpu_image_apply_usages(command_buffer, &output_usage, 1);

		VkRenderingAttachmentInfo attachment = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = mip_view((i32)in_mip),
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.loadOp = in_load_op,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		};
		const u32 width = mip_extent(bloom_pass.base_width, in_mip);
		const u32 height = mip_extent(bloom_pass.base_height, in_mip);
		VkRenderingInfo rendering_info = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea = {
				.offset = {0, 0},
				.extent = {width, height},
			},
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &attachment,
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

	inline VkDescriptorSet sampled_set(VulkanContext* ctx, VkImageView in_view)
	{
		VkDescriptorSet set =
			vulkan_allocate_transient_descriptor_set(ctx, bloom_pass.set_layout);
		VkDescriptorImageInfo image_info =
			descriptor_sampled(bloom_pass.linear_sampler, in_view);
		VkDescriptorBufferInfo buffer_info = descriptor_buffer(
			bloom_pass.auto_adaptation_state_buffer);
		VkWriteDescriptorSet writes[] = {
			descriptor_write_image(
				set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &image_info),
			descriptor_write_buffer(
				set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &buffer_info),
		};
		vulkan_update_descriptor_sets(ctx, 2, writes, 0, nullptr, false);
		return set;
	}

	inline void draw(
		VulkanContext* ctx,
		VkPipeline in_pipeline,
		VkDescriptorSet in_set,
		const void* in_push_constants,
		u32 in_push_constant_size)
	{
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, in_pipeline);
		vkCmdBindDescriptorSets(
			command_buffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			bloom_pass.pipeline_layout,
			0, 1, &in_set,
			0, nullptr);
		vkCmdPushConstants(
			command_buffer,
			bloom_pass.pipeline_layout,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			0, in_push_constant_size, in_push_constants);
		vulkan_cmd_draw(ctx, 3, 1, 0, 0);
	}

	inline void init(VulkanContext* ctx, VkSampler in_linear_sampler)
	{
		bloom_pass.linear_sampler = in_linear_sampler;
		VkDescriptorSetLayoutBinding bindings[] = {
			{
				.binding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			},
			{
				.binding = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			},
		};
		VkDescriptorSetLayoutCreateInfo set_layout_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = 2,
			.pBindings = bindings,
		};
		VK_CHECK(vkCreateDescriptorSetLayout(
			ctx->device, &set_layout_info, nullptr, &bloom_pass.set_layout));

		VkPushConstantRange push_constant_range = {
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			.offset = 0,
			.size = (u32)MAX(
				sizeof(DownsamplePushConstants), sizeof(UpsamplePushConstants)),
		};
		VkPipelineLayoutCreateInfo pipeline_layout_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &bloom_pass.set_layout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_constant_range,
		};
		VK_CHECK(vkCreatePipelineLayout(
			ctx->device, &pipeline_layout_info, nullptr, &bloom_pass.pipeline_layout));

		bloom_pass.downsample_pipeline = vulkan_create_fullscreen_pipeline(ctx, {
			.vertex_shader_path = "bin/shaders/bloom.vert.spv",
			.fragment_shader_path = "bin/shaders/bloom_downsample.frag.spv",
			.pipeline_layout = bloom_pass.pipeline_layout,
			.color_formats = &Render::SCENE_COLOR_FORMAT,
			.color_format_count = 1,
		});
		bloom_pass.upsample_pipeline = vulkan_create_fullscreen_pipeline(ctx, {
			.vertex_shader_path = "bin/shaders/bloom.vert.spv",
			.fragment_shader_path = "bin/shaders/bloom_upsample.frag.spv",
			.pipeline_layout = bloom_pass.pipeline_layout,
			.color_formats = &Render::SCENE_COLOR_FORMAT,
			.color_format_count = 1,
			.additive_blending = true,
		});
	}

	inline void release_image(VulkanContext* ctx)
	{
		vulkan_context_retire_image(ctx, bloom_pass.pyramid);
		bloom_pass.base_width = 0;
		bloom_pass.base_height = 0;
		bloom_pass.source_width = 0;
		bloom_pass.source_height = 0;
		bloom_pass.available_mip_count = 0;
		bloom_pass.effective_mip_count = 0;
		bloom_pass.profile_base_gain = HMM_V4(1.0f, 1.0f, 1.0f, 1.0f);
	}

	inline void handle_resize(VulkanContext* ctx, i32 in_width, i32 in_height)
	{
		const u32 source_width = (u32)MAX(in_width, 1);
		const u32 source_height = (u32)MAX(in_height, 1);
		const u32 base_width = MAX(1u, (source_width + 1u) / 2u);
		const u32 base_height = MAX(1u, (source_height + 1u) / 2u);
		if (bloom_pass.pyramid.image != VK_NULL_HANDLE &&
			bloom_pass.base_width == base_width &&
			bloom_pass.base_height == base_height)
		{
			bloom_pass.source_width = source_width;
			bloom_pass.source_height = source_height;
			return;
		}

		release_image(ctx);
		bloom_pass.base_width = base_width;
		bloom_pass.base_height = base_height;
		bloom_pass.source_width = source_width;
		bloom_pass.source_height = source_height;
		bloom_pass.available_mip_count = mip_count_for_extent(base_width, base_height);
		bloom_pass.pyramid = gpu_image_create(ctx->allocator, ctx->device, {
			.width = base_width,
			.height = base_height,
			.format = Render::SCENE_COLOR_FORMAT,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
			.mip_levels = (u32)bloom_pass.available_mip_count,
			.label = "Bloom Pyramid",
		});
	}

	inline void execute(
		VulkanContext* ctx,
		VkImageView in_source_view,
		const State::BloomState& in_state,
		const State::TonemappingState& in_tonemapping_state,
		VkBuffer in_auto_adaptation_state_buffer)
	{
		assert(bloom_pass.pyramid.image != VK_NULL_HANDLE);
		bloom_pass.effective_mip_count = CLAMP(
			in_state.requested_mip_count, 1, bloom_pass.available_mip_count);
		bloom_pass.auto_adaptation_state_buffer = in_auto_adaptation_state_buffer;
		const BloomProfile::ResolvedProfile profile =
			BloomProfile::resolve(bloom_pass.effective_mip_count);
		bloom_pass.profile_base_gain = HMM_V4(
			profile.bands[0].r,
			profile.bands[0].g,
			profile.bands[0].b,
			1.0f);
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);

		{
			CPU_TIMING_SCOPE("Bloom Downsample");
			const i32 timing_slot = gpu_timestamps_begin_scope(ctx, "Bloom Downsample");
			vulkan_begin_debug_label(ctx, "Bloom Downsample");

			VkImageView source_view = in_source_view;
			u32 source_width = bloom_pass.source_width;
			u32 source_height = bloom_pass.source_height;
			for (i32 mip = 0; mip < bloom_pass.effective_mip_count; ++mip)
			{
				if (mip > 0)
				{
					ImageUsage input_usage = mip_usage(
						(u32)mip - 1,
						VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
						VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
						VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
					gpu_image_apply_usages(command_buffer, &input_usage, 1);
					source_view = mip_view(mip - 1);
					source_width = mip_extent(bloom_pass.base_width, (u32)mip - 1);
					source_height = mip_extent(bloom_pass.base_height, (u32)mip - 1);
				}

				VkDescriptorSet set = sampled_set(ctx, source_view);
				begin_mip_render(
					ctx, (u32)mip, VK_ATTACHMENT_LOAD_OP_DONT_CARE, true);
				DownsamplePushConstants constants = {
					.source_pixel_size = HMM_V2(
						1.0f / (f32)source_width,
						1.0f / (f32)source_height),
					.threshold = CLAMP(in_state.threshold, 0.0f, 10.0f),
					.soft_knee = CLAMP(in_state.soft_knee, 0.0f, 1.0f),
					.exposure_scale = std::exp2(CLAMP(
						in_tonemapping_state.exposure_bias, -5.0f, 5.0f)),
					.apply_threshold = mip == 0 ? 1 : 0,
					.auto_exposure_enabled = RuntimeConfig::get().tonemap_validation_chart == 0
						&& in_tonemapping_state.auto_exposure_enabled ? 1 : 0,
					.auto_white_balance_enabled = RuntimeConfig::get().tonemap_validation_chart == 0
						&& in_tonemapping_state.auto_white_balance_enabled ? 1 : 0,
					.auto_exposure_influence = CLAMP(
						in_state.auto_exposure_influence, 0.0f, 1.0f),
				};
				draw(
					ctx, bloom_pass.downsample_pipeline, set,
					&constants, sizeof(constants));
				vkCmdEndRendering(command_buffer);
			}

			ImageUsage last_mip_usage = mip_usage(
				(u32)bloom_pass.effective_mip_count - 1,
				VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
				VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			gpu_image_apply_usages(command_buffer, &last_mip_usage, 1);

			vulkan_end_debug_label(ctx);
			gpu_timestamps_end_scope(ctx, timing_slot);
		}

		if (bloom_pass.effective_mip_count > 1)
		{
			CPU_TIMING_SCOPE("Bloom Upsample");
			const i32 timing_slot = gpu_timestamps_begin_scope(ctx, "Bloom Upsample");
			vulkan_begin_debug_label(ctx, "Bloom Upsample");

			for (i32 coarse_mip = bloom_pass.effective_mip_count - 1;
				coarse_mip > 0;
				--coarse_mip)
			{
				const i32 fine_mip = coarse_mip - 1;
				const BloomProfile::RgbWeight coarse_weight_ratio =
					BloomProfile::reconstruction_ratio(profile, fine_mip);
				VkDescriptorSet set = sampled_set(ctx, mip_view(coarse_mip));
				begin_mip_render(
					ctx, (u32)fine_mip, VK_ATTACHMENT_LOAD_OP_LOAD, false);
				UpsamplePushConstants constants = {
					.coarse_weight_ratio = HMM_V4(
						coarse_weight_ratio.r,
						coarse_weight_ratio.g,
						coarse_weight_ratio.b,
						1.0f),
					.source_pixel_size = HMM_V2(
						1.0f / (f32)mip_extent(bloom_pass.base_width, (u32)coarse_mip),
						1.0f / (f32)mip_extent(bloom_pass.base_height, (u32)coarse_mip)),
				};
				draw(
					ctx, bloom_pass.upsample_pipeline, set,
					&constants, sizeof(constants));
				vkCmdEndRendering(command_buffer);

				ImageUsage fine_read_usage = mip_usage(
					(u32)fine_mip,
					VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
					VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				gpu_image_apply_usages(command_buffer, &fine_read_usage, 1);
			}

			vulkan_end_debug_label(ctx);
			gpu_timestamps_end_scope(ctx, timing_slot);
		}
	}

	inline void shutdown(VulkanContext* ctx)
	{
		gpu_image_destroy(ctx->allocator, ctx->device, bloom_pass.pyramid);
		vkDestroyPipeline(ctx->device, bloom_pass.upsample_pipeline, nullptr);
		vkDestroyPipeline(ctx->device, bloom_pass.downsample_pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, bloom_pass.pipeline_layout, nullptr);
		vkDestroyDescriptorSetLayout(ctx->device, bloom_pass.set_layout, nullptr);
	}
}
