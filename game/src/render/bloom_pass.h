#pragma once

#include <cmath>

#include "core/runtime_config.h"
#include "core/timings.h"
#include "render/bloom_profile.inl"
#include "render/frame_data.h"
#include "render/frame_render_graph.h"
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
		DescriptorSetSchema descriptors;
		EffectPipelineLayout pipeline_layout;
		FullscreenPipeline downsample_pipeline;
		FullscreenPipeline upsample_pipeline;
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

	inline VkDescriptorSet sampled_set(VulkanContext* ctx, VkImageView in_view)
	{
		DescriptorWriter writer = bloom_pass.descriptors.writer(ctx);
		writer.sampled(0, bloom_pass.linear_sampler, in_view)
			.buffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				bloom_pass.auto_adaptation_state_buffer);
		writer.commit();
		return writer.set;
	}

	inline void draw(
		VulkanContext* ctx,
		const FullscreenPipeline& in_pipeline,
		VkDescriptorSet in_set,
		const void* in_push_constants,
		u32 in_push_constant_size)
	{
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		in_pipeline.bind(ctx);
		vkCmdBindDescriptorSets(
			command_buffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			bloom_pass.pipeline_layout.layout,
			0, 1, &in_set,
			0, nullptr);
		bloom_pass.pipeline_layout.push(
			ctx, in_push_constants, in_push_constant_size);
		vulkan_cmd_draw(ctx, 3, 1, 0, 0);
	}

	inline void init(VulkanContext* ctx, VkSampler in_linear_sampler)
	{
		bloom_pass.linear_sampler = in_linear_sampler;
		const DescriptorBindingSpec bindings[] = {
			{
				.binding = 0,
				.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			},
			{
				.binding = 1,
				.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			},
		};
		bloom_pass.descriptors.init(ctx, bindings, 2,
			EDescriptorSetAllocation::Transient);
		bloom_pass.pipeline_layout.init(ctx, &bloom_pass.descriptors.layout, 1,
			(u32)MAX(sizeof(DownsamplePushConstants), sizeof(UpsamplePushConstants)),
			VK_SHADER_STAGE_FRAGMENT_BIT);

		bloom_pass.downsample_pipeline.init(ctx, {
			.vertex_shader_path = "bin/shaders/bloom.vert.spv",
			.fragment_shader_path = "bin/shaders/bloom_downsample.frag.spv",
			.pipeline_layout = bloom_pass.pipeline_layout.layout,
			.color_formats = &Render::SCENE_COLOR_FORMAT,
			.color_format_count = 1,
		});
		bloom_pass.upsample_pipeline.init(ctx, {
			.vertex_shader_path = "bin/shaders/bloom.vert.spv",
			.fragment_shader_path = "bin/shaders/bloom_upsample.frag.spv",
			.pipeline_layout = bloom_pass.pipeline_layout.layout,
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
		FrameRenderGraph& graph,
		VulkanContext* ctx,
		FrameGraphImage in_source,
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
		{
			CPU_TIMING_SCOPE("Bloom Downsample");
			const i32 timing_slot = gpu_timestamps_begin_scope(ctx, "Bloom Downsample");
			vulkan_begin_debug_label(ctx, "Bloom Downsample");

			FrameGraphImage source = in_source;
			for (i32 mip = 0; mip < bloom_pass.effective_mip_count; ++mip)
			{
				if (mip > 0)
					source = frame_graph_mip(bloom_pass.pyramid, (u32)mip - 1);

				graph.sampled(source);
				graph.storage_read(frame_graph_buffer(in_auto_adaptation_state_buffer),
					VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
				VkDescriptorSet set = sampled_set(ctx, source.view());
				DownsamplePushConstants constants = {
					.source_pixel_size = HMM_V2(
						1.0f / (f32)source.extent.width,
						1.0f / (f32)source.extent.height),
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
				FrameGraphColorAttachment attachment = {
					.image = frame_graph_mip(bloom_pass.pyramid, (u32)mip),
				};
				graph.render(&attachment, 1, [&]() {
					draw(ctx, bloom_pass.downsample_pipeline, set,
						&constants, sizeof(constants));
				});
			}

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
				FrameGraphImage coarse = frame_graph_mip(
					bloom_pass.pyramid, (u32)coarse_mip);
				graph.sampled(coarse);
				graph.storage_read(frame_graph_buffer(in_auto_adaptation_state_buffer),
					VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
				VkDescriptorSet set = sampled_set(ctx, coarse.view());
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
				FrameGraphColorAttachment attachment = {
					.image = frame_graph_mip(bloom_pass.pyramid, (u32)fine_mip),
					.load_op = VK_ATTACHMENT_LOAD_OP_LOAD,
				};
				graph.render(&attachment, 1, [&]() {
					draw(ctx, bloom_pass.upsample_pipeline, set,
						&constants, sizeof(constants));
				});
			}

			vulkan_end_debug_label(ctx);
			gpu_timestamps_end_scope(ctx, timing_slot);
		}

		graph.sampled(frame_graph_mip(bloom_pass.pyramid, 0));
	}

	inline void shutdown(VulkanContext* ctx)
	{
		gpu_image_destroy(ctx->allocator, ctx->device, bloom_pass.pyramid);
		bloom_pass.upsample_pipeline.shutdown(ctx);
		bloom_pass.downsample_pipeline.shutdown(ctx);
		bloom_pass.pipeline_layout.shutdown(ctx);
		bloom_pass.descriptors.shutdown(ctx);
	}
}
