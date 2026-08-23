#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/fullscreen_pipeline.h"
#include "render/frame_data.h"
#include "render/frame_render_graph.h"
#include "render/gt7_tonemapping.h"
#include "render/aces2_tonemapping.h"
#include "render/agx_tonemapping.h"

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
	f32 lut_integration_scale;
	i32 validation_chart;
	HMM_Vec4 bloom_profile_gain;
	i32 auto_exposure_enabled;
	i32 auto_white_balance_enabled;
	f32 bloom_auto_exposure_influence;
	HMM_Vec4 local_recovery;
};
static_assert(sizeof(TonemappingFinalPushConstants) == 80);

struct TonemappingLocalProxyPushConstants
{
	HMM_Vec2 source_pixel_size;
	f32 exposure_bias;
	f32 shadow_recovery;
	f32 highlight_recovery;
	f32 preference_sigma;
	i32 method;
	f32 lut_integration_scale;
	i32 validation_chart;
	i32 auto_exposure_enabled;
	i32 auto_white_balance_enabled;
};
static_assert(sizeof(TonemappingLocalProxyPushConstants) == 44);

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

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
static constexpr u32 TONEMAPPING_DEBUG_PANEL_COUNT = 15;

struct TonemappingLocalDebugChannelPushConstants
{
	i32 channel;
};
static_assert(sizeof(TonemappingLocalDebugChannelPushConstants) == 4);

struct TonemappingLocalDebugGuidedPushConstants
{
	i32 method;
	f32 exposure_bias;
	HMM_Vec2 guide_pixel_size;
	f32 lut_integration_scale;
	i32 validation_chart;
	i32 auto_exposure_enabled;
	i32 auto_white_balance_enabled;
	HMM_Vec2 recovery_limits;
};
static_assert(sizeof(TonemappingLocalDebugGuidedPushConstants) == 40);

struct TonemappingDebugViewData
{
	bool ready = false;
	i32 selected_full_mip = 1;
	i32 reconstruction_full_mip = 1;
	i32 coarsest_full_mip = 1;
	u32 base_width = 1;
	u32 base_height = 1;
	VkImageView exposure[3] = {};
	VkImageView weights[3] = {};
	VkImageView laplacians[3] = {};
	VkImageView selected_reconstruction = VK_NULL_HANDLE;
	VkImageView transfer_guide = VK_NULL_HANDLE;
	VkImageView transfer_fused = VK_NULL_HANDLE;
	VkImageView guided[3] = {};
	VkImageView geometry_coverage = VK_NULL_HANDLE;
	VkImageView boundary_suppression = VK_NULL_HANDLE;
	const GpuImage* reconstruction_pyramid = nullptr;
};
#endif

struct TonemappingPass
{
	DescriptorSetSchema final_descriptors;
	DescriptorSetSchema local_descriptors;
	EffectPipelineLayout final_pipeline_layout;
	EffectPipelineLayout local_pipeline_layout;
	FullscreenPipeline final_pipeline;
	FullscreenPipeline local_proxy_pipeline;
	FullscreenPipeline local_downsample_pipeline;
	FullscreenPipeline local_blend_pipeline;
	FullscreenPipeline local_reconstruct_pipeline;
#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	FullscreenPipeline local_debug_laplacian_pipeline;
	FullscreenPipeline local_debug_guided_pipeline;
	FullscreenPipeline local_debug_channel_pipeline;
#endif

	GpuImage local_exposure_pyramid;
	GpuImage local_weight_pyramid;
	GpuImage local_reconstruction_pyramid;
	GpuImage tonemapping_lut;
#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	GpuImage local_debug_laplacian_pyramid;
	GpuImage local_debug_guided;
	GpuImage local_debug_panels;
	u32 local_debug_panel_width = 0;
	u32 local_debug_panel_height = 0;
	TonemappingDebugViewData debug_view_data;
#endif
	u32 local_base_width = 0;
	u32 local_base_height = 0;
	u32 local_mip_count = 0;
	u32 local_source_width = 0;
	u32 local_source_height = 0;

	VkImageView scene_color_view = VK_NULL_HANDLE;
	VkImageView position_view = VK_NULL_HANDLE;
	FrameGraphImage scene_color;
	FrameGraphImage position;
	FrameGraphImage bloom;
	bool bloom_enabled = false;
	VkSampler sampler = VK_NULL_HANDLE;
	VkSampler position_sampler = VK_NULL_HANDLE;
	VkBuffer auto_adaptation_state_buffer = VK_NULL_HANDLE;
	i32 effective_coarsest_mip = 1;
	i32 effective_reconstruction_mip = 1;
};

static TonemappingPass tonemapping_pass;

inline EDisplayOutputMode tonemapping_profile_output_mode(const VulkanContext* ctx)
{
	const RuntimeConfig::Config& config = RuntimeConfig::get();
	if (config.tonemap_validation_chart != 0 && config.tonemap_validation_output_mode)
	{
		if (*config.tonemap_validation_output_mode == "edr") return EDisplayOutputMode::EDR;
		if (*config.tonemap_validation_output_mode == "hdr10") return EDisplayOutputMode::HDR10;
		return EDisplayOutputMode::SDR;
	}
	return ctx->active_output_mode;
}

inline f32 tonemapping_lut_integration_scale(
	const VulkanContext* ctx,
	ETonemappingMethod method)
{
	const bool sdr = tonemapping_profile_output_mode(ctx) == EDisplayOutputMode::SDR;
	if (method == ETonemappingMethod::AgX)
		return sdr ? AgXTonemapping::SDR_INTEGRATION_SCALE : AgXTonemapping::HDR_INTEGRATION_SCALE;
	if (method == ETonemappingMethod::Aces2)
		return sdr ? ACES2Tonemapping::SDR_INTEGRATION_SCALE : ACES2Tonemapping::HDR_INTEGRATION_SCALE;
	return sdr ? GT7Tonemapping::SDR_INTEGRATION_SCALE : GT7Tonemapping::HDR_INTEGRATION_SCALE;
}

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

inline VkImageView tonemapping_mip_view(const GpuImage& in_image, u32 in_mip)
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

void tonemapping_pass_init(VulkanContext* ctx)
{
	DescriptorBindingSpec final_bindings[7] = {};
	for (u32 binding = 0; binding < 6; ++binding)
		final_bindings[binding] = {
			.binding = binding, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
	final_bindings[6] = { .binding = 6, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
	tonemapping_pass.final_descriptors.init(ctx, final_bindings, 7,
		EDescriptorSetAllocation::PersistentPerFrame);
	DescriptorBindingSpec local_bindings[6] = {};
	for (u32 binding = 0; binding < 5; ++binding)
		local_bindings[binding] = {
			.binding = binding, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
	local_bindings[5] = { .binding = 5, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
	tonemapping_pass.local_descriptors.init(ctx, local_bindings, 6,
		EDescriptorSetAllocation::Transient);
	VkSamplerCreateInfo position_sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	VK_CHECK(vkCreateSampler(
		ctx->device, &position_sampler_info, nullptr, &tonemapping_pass.position_sampler));

	const EDisplayOutputMode profile_output_mode = tonemapping_profile_output_mode(ctx);
	const bool hdr_output = profile_output_mode != EDisplayOutputMode::SDR;
	printf("GT7 profile: %s, integration scale %.7f\n",
		hdr_output ? "HDR 1000-nit peak / 203-nit diffuse white" : "SDR",
		hdr_output ? GT7Tonemapping::HDR_INTEGRATION_SCALE : GT7Tonemapping::SDR_INTEGRATION_SCALE);
	DynamicArray<u16> gt7_lut_pixels = hdr_output
		? GT7Tonemapping::generate_hdr_lut()
		: GT7Tonemapping::generate_sdr_lut();
	ACES2Tonemapping::LoadedLUT aces2_lut;
	std::string aces2_error;
	if (!ACES2Tonemapping::load_lut((i32)profile_output_mode, &aces2_lut, &aces2_error))
	{
		printf("ACES 2.0 LUT load failed: %s\n", aces2_error.c_str());
		exit(1);
	}
	AgXTonemapping::LoadedLUT agx_lut;
	std::string agx_error;
	if (!AgXTonemapping::load_lut((i32)profile_output_mode, &agx_lut, &agx_error))
	{
		printf("AgX LUT load failed: %s\n", agx_error.c_str());
		exit(1);
	}
	assert(gt7_lut_pixels.length() == ACES2Tonemapping::LUT_PAYLOAD_SIZE / sizeof(u16));
	assert(aces2_lut.pixels.length() == gt7_lut_pixels.length());
	assert(agx_lut.pixels.length() == gt7_lut_pixels.length());
	assert(ctx->physical_device_properties.limits.maxImageArrayLayers
		>= TONEMAPPING_LUT_REQUIRED_ARRAY_LAYERS);
	DynamicArray<u16> combined_lut_pixels;
	combined_lut_pixels.resize(
		gt7_lut_pixels.length() + aces2_lut.pixels.length() + agx_lut.pixels.length());
	memcpy(combined_lut_pixels.data(), gt7_lut_pixels.data(),
		gt7_lut_pixels.length() * sizeof(u16));
	memcpy(combined_lut_pixels.data() + gt7_lut_pixels.length(), aces2_lut.pixels.data(),
		aces2_lut.pixels.length() * sizeof(u16));
	memcpy(combined_lut_pixels.data() + gt7_lut_pixels.length() + aces2_lut.pixels.length(),
		agx_lut.pixels.data(), agx_lut.pixels.length() * sizeof(u16));
	printf("ACES 2.0 profile: %s, integration scale %.7f, LUT CRC32 %08x\n",
		ACES2Tonemapping::target_name((i32)profile_output_mode),
		hdr_output ? ACES2Tonemapping::HDR_INTEGRATION_SCALE : ACES2Tonemapping::SDR_INTEGRATION_SCALE,
		aces2_lut.crc32);
	printf("AgX profile: %s / Medium High Contrast, integration scale %.7f, LUT CRC32 %08x\n",
		AgXTonemapping::target_name(agx_lut.target),
		hdr_output ? AgXTonemapping::HDR_INTEGRATION_SCALE : AgXTonemapping::SDR_INTEGRATION_SCALE,
		agx_lut.crc32);
	tonemapping_pass.tonemapping_lut = gpu_image_create_from_data(
		ctx,
		GT7Tonemapping::LUT_RESOLUTION,
		GT7Tonemapping::LUT_RESOLUTION,
		VK_FORMAT_R16G16B16A16_SFLOAT,
		combined_lut_pixels.data(),
		combined_lut_pixels.length() * sizeof(u16),
		TONEMAPPING_LUT_REQUIRED_ARRAY_LAYERS,
		hdr_output ? "GT7 + ACES 2 + AgX HDR Tone Mapping LUTs" : "GT7 + ACES 2 + AgX SDR Tone Mapping LUTs");

	tonemapping_pass.final_pipeline_layout.init(ctx,
		&tonemapping_pass.final_descriptors.layout, 1,
		sizeof(TonemappingFinalPushConstants), VK_SHADER_STAGE_FRAGMENT_BIT);
	tonemapping_pass.local_pipeline_layout.init(ctx,
		&tonemapping_pass.local_descriptors.layout, 1,
		sizeof(TonemappingLocalProxyPushConstants), VK_SHADER_STAGE_FRAGMENT_BIT);

	tonemapping_pass.final_pipeline.init(ctx, {
		.vertex_shader_path = "bin/shaders/tonemapping.vert.spv",
		.fragment_shader_path = "bin/shaders/tonemapping.frag.spv",
		.pipeline_layout = tonemapping_pass.final_pipeline_layout.layout,
		.color_formats = &Render::SCENE_COLOR_FORMAT,
		.color_format_count = 1,
	});

	VkFormat two_color_formats[2] = {
		Render::SCENE_COLOR_FORMAT,
		Render::SCENE_COLOR_FORMAT,
	};
	tonemapping_pass.local_proxy_pipeline.init(ctx, {
		.vertex_shader_path = "bin/shaders/tonemapping.vert.spv",
		.fragment_shader_path = "bin/shaders/tonemapping_local_proxy.frag.spv",
		.pipeline_layout = tonemapping_pass.local_pipeline_layout.layout,
		.color_formats = two_color_formats,
		.color_format_count = 2,
	});
	tonemapping_pass.local_downsample_pipeline.init(ctx, {
		.vertex_shader_path = "bin/shaders/tonemapping.vert.spv",
		.fragment_shader_path = "bin/shaders/tonemapping_local_downsample.frag.spv",
		.pipeline_layout = tonemapping_pass.local_pipeline_layout.layout,
		.color_formats = two_color_formats,
		.color_format_count = 2,
	});
	tonemapping_pass.local_blend_pipeline.init(ctx, {
		.vertex_shader_path = "bin/shaders/tonemapping.vert.spv",
		.fragment_shader_path = "bin/shaders/tonemapping_local_blend.frag.spv",
		.pipeline_layout = tonemapping_pass.local_pipeline_layout.layout,
		.color_formats = &Render::SCENE_COLOR_FORMAT,
		.color_format_count = 1,
	});
	tonemapping_pass.local_reconstruct_pipeline.init(ctx, {
		.vertex_shader_path = "bin/shaders/tonemapping.vert.spv",
		.fragment_shader_path = "bin/shaders/tonemapping_local_reconstruct.frag.spv",
		.pipeline_layout = tonemapping_pass.local_pipeline_layout.layout,
		.color_formats = &Render::SCENE_COLOR_FORMAT,
		.color_format_count = 1,
	});
#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	tonemapping_pass.local_debug_laplacian_pipeline.init(ctx, {
		.vertex_shader_path = "bin/shaders/tonemapping.vert.spv",
		.fragment_shader_path = "bin/shaders/tonemapping_local_debug_laplacian.frag.spv",
		.pipeline_layout = tonemapping_pass.local_pipeline_layout.layout,
		.color_formats = &Render::SCENE_COLOR_FORMAT,
		.color_format_count = 1,
	});
	tonemapping_pass.local_debug_guided_pipeline.init(ctx, {
		.vertex_shader_path = "bin/shaders/tonemapping.vert.spv",
		.fragment_shader_path = "bin/shaders/tonemapping_local_debug_guided.frag.spv",
		.pipeline_layout = tonemapping_pass.local_pipeline_layout.layout,
		.color_formats = &Render::SCENE_COLOR_FORMAT,
		.color_format_count = 1,
	});
	tonemapping_pass.local_debug_channel_pipeline.init(ctx, {
		.vertex_shader_path = "bin/shaders/tonemapping.vert.spv",
		.fragment_shader_path = "bin/shaders/tonemapping_local_debug_channel.frag.spv",
		.pipeline_layout = tonemapping_pass.local_pipeline_layout.layout,
		.color_formats = &Render::SCENE_COLOR_FORMAT,
		.color_format_count = 1,
	});
#endif
}

void tonemapping_pass_release_local_images(VulkanContext* ctx)
{
	vulkan_context_retire_image(ctx, tonemapping_pass.local_exposure_pyramid);
	vulkan_context_retire_image(ctx, tonemapping_pass.local_weight_pyramid);
	vulkan_context_retire_image(ctx, tonemapping_pass.local_reconstruction_pyramid);
#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	vulkan_context_retire_image(ctx, tonemapping_pass.local_debug_laplacian_pyramid);
	vulkan_context_retire_image(ctx, tonemapping_pass.local_debug_guided);
	vulkan_context_retire_image(ctx, tonemapping_pass.local_debug_panels);
	tonemapping_pass.local_debug_panel_width = 0;
	tonemapping_pass.local_debug_panel_height = 0;
	tonemapping_pass.debug_view_data = {};
#endif
	tonemapping_pass.local_base_width = 0;
	tonemapping_pass.local_base_height = 0;
	tonemapping_pass.local_mip_count = 0;
	tonemapping_pass.local_source_width = 0;
	tonemapping_pass.local_source_height = 0;
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
	tonemapping_pass.local_source_width = (u32)MAX(in_width, 1);
	tonemapping_pass.local_source_height = (u32)MAX(in_height, 1);

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

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
void tonemapping_pass_ensure_debug_resources(VulkanContext* ctx)
{
	if (tonemapping_pass.local_debug_guided.image != VK_NULL_HANDLE)
	{
		return;
	}

	const VkImageUsageFlags usage =
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	tonemapping_pass.local_debug_laplacian_pyramid = gpu_image_create(
		ctx->allocator, ctx->device, {
			.width = tonemapping_pass.local_base_width,
			.height = tonemapping_pass.local_base_height,
			.format = Render::SCENE_COLOR_FORMAT,
			.usage = usage,
			.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
			.mip_levels = tonemapping_pass.local_mip_count,
			.label = "Tonemapping Local Debug Laplacian Pyramid",
		});
	tonemapping_pass.local_debug_guided = gpu_image_create(
		ctx->allocator, ctx->device, {
			.width = tonemapping_pass.local_source_width,
			.height = tonemapping_pass.local_source_height,
			.format = Render::SCENE_COLOR_FORMAT,
			.usage = usage,
			.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
			.label = "Tonemapping Local Debug Guided Transfer",
		});
	tonemapping_pass.local_debug_panel_width =
		MIN(512u, tonemapping_pass.local_base_width);
	tonemapping_pass.local_debug_panel_height = MAX(
		1u,
		(u32)((u64)tonemapping_pass.local_debug_panel_width
			* (u64)tonemapping_pass.local_base_height
			/ (u64)MAX(tonemapping_pass.local_base_width, 1u)));
	tonemapping_pass.local_debug_panels = gpu_image_create(
		ctx->allocator, ctx->device, {
			.width = tonemapping_pass.local_debug_panel_width,
			.height = tonemapping_pass.local_debug_panel_height,
			.format = Render::SCENE_COLOR_FORMAT,
			.usage = usage,
			.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
			.array_layers = TONEMAPPING_DEBUG_PANEL_COUNT,
			.label = "Tonemapping Local Debug Panels",
		});
}
#endif

inline VkDescriptorSet tonemapping_local_descriptor_set(
	VulkanContext* ctx,
	const VkImageView* in_views,
	u32 in_view_count,
	VkSampler in_last_sampler = VK_NULL_HANDLE)
{
	assert(in_view_count <= 5);
	DescriptorWriter writer = tonemapping_pass.local_descriptors.writer(ctx);
	for (u32 view_idx = 0; view_idx < in_view_count; ++view_idx)
	{
		const VkSampler view_sampler = in_last_sampler != VK_NULL_HANDLE
			&& view_idx + 1 == in_view_count
			? in_last_sampler : tonemapping_pass.sampler;
		writer.sampled(view_idx, view_sampler, in_views[view_idx]);
	}
	writer.buffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		tonemapping_pass.auto_adaptation_state_buffer);
	writer.commit();
	return writer.set;
}

inline void tonemapping_render_mip(
	FrameRenderGraph& graph,
	GpuImage** in_outputs,
	u32 in_output_count,
	u32 in_mip,
	const std::function<void()>& in_callback)
{
	assert(in_output_count >= 1 && in_output_count <= 2);
	FrameGraphColorAttachment attachments[2] = {};
	for (u32 output_idx = 0; output_idx < in_output_count; ++output_idx)
	{
		attachments[output_idx] = {
			.image = frame_graph_mip(*in_outputs[output_idx], in_mip),
		};
	}
	graph.render(attachments, in_output_count, in_callback);
}

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
inline void tonemapping_render_debug_panel(
	FrameRenderGraph& graph,
	u32 in_layer,
	const std::function<void()>& in_callback)
{
	assert(in_layer < tonemapping_pass.local_debug_panels.array_layers);
	FrameGraphColorAttachment attachment = {
		.image = frame_graph_layer(tonemapping_pass.local_debug_panels, in_layer),
	};
	graph.render(&attachment, 1, in_callback);
}
#endif

inline void tonemapping_draw_local_stage(
	VulkanContext* ctx,
	const FullscreenPipeline& in_pipeline,
	VkDescriptorSet in_set,
	const void* in_push_constants = nullptr,
	u32 in_push_constant_size = 0)
{
	VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
	in_pipeline.bind(ctx);
	vkCmdBindDescriptorSets(
		command_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		tonemapping_pass.local_pipeline_layout.layout,
		0, 1, &in_set,
		0, nullptr);
	if (in_push_constants && in_push_constant_size > 0)
	{
		tonemapping_pass.local_pipeline_layout.push(
			ctx, in_push_constants, in_push_constant_size);
	}
	vulkan_cmd_draw(ctx, 3, 1, 0, 0);
}

void tonemapping_pass_update(
	VulkanContext* ctx,
	FrameGraphImage in_scene_color,
	FrameGraphImage in_position,
	FrameGraphImage in_bloom,
	f32 in_bloom_intensity,
	VkSampler in_sampler,
	VkBuffer in_auto_adaptation_state_buffer)
{
	tonemapping_pass.scene_color = in_scene_color;
	tonemapping_pass.position = in_position;
	tonemapping_pass.bloom = in_bloom;
	tonemapping_pass.bloom_enabled = in_bloom_intensity > 0.0f;
	tonemapping_pass.scene_color_view = in_scene_color.view();
	tonemapping_pass.position_view = in_position.view();
	tonemapping_pass.sampler = in_sampler;
	tonemapping_pass.auto_adaptation_state_buffer = in_auto_adaptation_state_buffer;

	// Bind the source as a valid placeholder for local-only bindings. Local
	// preparation replaces them before the final draw.
	DescriptorWriter writer = tonemapping_pass.final_descriptors.writer(ctx);
	writer.sampled(0, in_sampler, in_scene_color.view())
		.sampled(1, in_sampler, in_scene_color.view())
		.sampled(2, in_sampler, in_scene_color.view())
		.sampled(3, in_sampler,
			in_bloom_intensity > 0.0f ? in_bloom.view() : in_scene_color.view())
		.sampled(4, in_sampler, tonemapping_pass.tonemapping_lut.view)
		.sampled(5, tonemapping_pass.position_sampler, in_position.view())
		.buffer(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			in_auto_adaptation_state_buffer);
	writer.commit();
}

void tonemapping_pass_declare_final_resources(
	FrameRenderGraph& graph,
	const State::TonemappingState& in_state)
{
	graph.sampled(tonemapping_pass.scene_color);
	graph.sampled(tonemapping_pass.position);
	graph.sampled(frame_graph_image(tonemapping_pass.tonemapping_lut));
	if (tonemapping_pass.bloom_enabled)
		graph.sampled(tonemapping_pass.bloom);
	if (in_state.local_enabled)
	{
		const u32 mip = (u32)MAX(
			tonemapping_pass.effective_reconstruction_mip - 1, 0);
		graph.sampled(frame_graph_mip(
			tonemapping_pass.local_exposure_pyramid, mip));
		graph.sampled(frame_graph_mip(
			tonemapping_pass.local_reconstruction_pyramid, mip));
	}
	graph.storage_read(frame_graph_buffer(
			tonemapping_pass.auto_adaptation_state_buffer),
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
}

void tonemapping_pass_prepare_local(
	FrameRenderGraph& graph,
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
	// Half-resolution synthetic exposures and their well-exposedness weights.
	{
		CPU_TIMING_SCOPE("Tonemapping Local Proxy");
		const i32 timing_slot =
			gpu_timestamps_begin_scope(ctx, "Tonemapping Local Proxy");
		vulkan_begin_debug_label(ctx, "Tonemapping Local Proxy");

		VkImageView views[] = {
			tonemapping_pass.scene_color_view,
			tonemapping_pass.tonemapping_lut.view,
			tonemapping_pass.position_view,
		};
		VkDescriptorSet set = tonemapping_local_descriptor_set(
			ctx, views, 3, tonemapping_pass.position_sampler);
		GpuImage* outputs[] = {
			&tonemapping_pass.local_exposure_pyramid,
			&tonemapping_pass.local_weight_pyramid,
		};
		TonemappingLocalProxyPushConstants constants = {
			.source_pixel_size = HMM_V2(
				0.5f / (f32)tonemapping_pass.local_base_width,
				0.5f / (f32)tonemapping_pass.local_base_height),
			.exposure_bias = in_state.exposure_bias,
			.shadow_recovery = in_state.local_shadow_recovery,
			.highlight_recovery = in_state.local_highlight_recovery,
			.preference_sigma = in_state.local_exposure_preference_sigma,
			.method = (i32)in_state.method,
			.lut_integration_scale = tonemapping_lut_integration_scale(ctx, in_state.method),
			.validation_chart = RuntimeConfig::get().tonemap_validation_chart,
			.auto_exposure_enabled = RuntimeConfig::get().tonemap_validation_chart == 0
				&& in_state.auto_exposure_enabled ? 1 : 0,
			.auto_white_balance_enabled = RuntimeConfig::get().tonemap_validation_chart == 0
				&& in_state.auto_white_balance_enabled ? 1 : 0,
		};
		graph.sampled(tonemapping_pass.scene_color);
		graph.sampled(frame_graph_image(tonemapping_pass.tonemapping_lut));
		graph.sampled(tonemapping_pass.position);
		graph.storage_read(frame_graph_buffer(
				tonemapping_pass.auto_adaptation_state_buffer),
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
		tonemapping_render_mip(graph, outputs, 2, 0, [&]() {
			tonemapping_draw_local_stage(
				ctx, tonemapping_pass.local_proxy_pipeline, set,
				&constants, sizeof(constants));
		});

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
			graph.sampled(frame_graph_mip(
				tonemapping_pass.local_exposure_pyramid, mip - 1));
			graph.sampled(frame_graph_mip(
				tonemapping_pass.local_weight_pyramid, mip - 1));
			graph.storage_read(frame_graph_buffer(
					tonemapping_pass.auto_adaptation_state_buffer),
				VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

			VkImageView views[] = {
				tonemapping_mip_view(tonemapping_pass.local_exposure_pyramid, mip - 1),
				tonemapping_mip_view(tonemapping_pass.local_weight_pyramid, mip - 1),
			};
			VkDescriptorSet set = tonemapping_local_descriptor_set(ctx, views, 2);
			GpuImage* outputs[] = {
				&tonemapping_pass.local_exposure_pyramid,
				&tonemapping_pass.local_weight_pyramid,
			};
			TonemappingLocalDownsamplePushConstants constants = {
				.source_pixel_size = HMM_V2(
					1.0f / (f32)tonemapping_local_mip_extent(
						tonemapping_pass.local_base_width, mip - 1),
					1.0f / (f32)tonemapping_local_mip_extent(
						tonemapping_pass.local_base_height, mip - 1)),
			};
			tonemapping_render_mip(graph, outputs, 2, mip, [&]() {
				tonemapping_draw_local_stage(
					ctx, tonemapping_pass.local_downsample_pipeline, set,
					&constants, sizeof(constants));
			});
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
		graph.sampled(frame_graph_mip(
			tonemapping_pass.local_exposure_pyramid, coarsest_mip));
		graph.sampled(frame_graph_mip(
			tonemapping_pass.local_weight_pyramid, coarsest_mip));
		graph.storage_read(frame_graph_buffer(
			tonemapping_pass.auto_adaptation_state_buffer),
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

		VkImageView coarse_views[] = {
			tonemapping_mip_view(tonemapping_pass.local_exposure_pyramid, coarsest_mip),
			tonemapping_mip_view(tonemapping_pass.local_weight_pyramid, coarsest_mip),
		};
		VkDescriptorSet coarse_set =
			tonemapping_local_descriptor_set(ctx, coarse_views, 2);
		GpuImage* reconstruction_output[] = {
			&tonemapping_pass.local_reconstruction_pyramid,
		};
		tonemapping_render_mip(
			graph, reconstruction_output, 1, coarsest_mip, [&]() {
				tonemapping_draw_local_stage(
					ctx, tonemapping_pass.local_blend_pipeline, coarse_set);
			});

		for (u32 coarse_mip = coarsest_mip;
			coarse_mip > reconstruction_mip;
			--coarse_mip)
		{
			const u32 fine_mip = coarse_mip - 1;
			graph.sampled(frame_graph_mip(
				tonemapping_pass.local_exposure_pyramid, fine_mip));
			graph.sampled(frame_graph_mip(
				tonemapping_pass.local_weight_pyramid, fine_mip));
			graph.sampled(frame_graph_mip(
				tonemapping_pass.local_exposure_pyramid, coarse_mip));
			graph.sampled(frame_graph_mip(
				tonemapping_pass.local_reconstruction_pyramid, coarse_mip));
			graph.storage_read(frame_graph_buffer(
				tonemapping_pass.auto_adaptation_state_buffer),
				VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);

			VkImageView reconstruct_views[] = {
				tonemapping_mip_view(tonemapping_pass.local_exposure_pyramid, fine_mip),
				tonemapping_mip_view(tonemapping_pass.local_weight_pyramid, fine_mip),
				tonemapping_mip_view(tonemapping_pass.local_exposure_pyramid, coarse_mip),
				tonemapping_mip_view(tonemapping_pass.local_reconstruction_pyramid, coarse_mip),
			};
			VkDescriptorSet reconstruct_set =
				tonemapping_local_descriptor_set(ctx, reconstruct_views, 4);
			TonemappingLocalReconstructPushConstants constants = {
				.boost_local_contrast =
					in_state.local_contrast_boost ? 1 : 0,
			};
			tonemapping_render_mip(
				graph, reconstruction_output, 1, fine_mip, [&]() {
					tonemapping_draw_local_stage(
						ctx, tonemapping_pass.local_reconstruct_pipeline,
						reconstruct_set, &constants, sizeof(constants));
				});
		}

		vulkan_end_debug_label(ctx);
		gpu_timestamps_end_scope(ctx, timing_slot);
	}

	graph.sampled(frame_graph_mip(
		tonemapping_pass.local_exposure_pyramid, reconstruction_mip));
	graph.sampled(frame_graph_mip(
		tonemapping_pass.local_reconstruction_pyramid, reconstruction_mip));

	DescriptorWriter final_writer = tonemapping_pass.final_descriptors.writer(ctx);
	final_writer.sampled(1, tonemapping_pass.sampler,
		tonemapping_mip_view(
			tonemapping_pass.local_exposure_pyramid, reconstruction_mip))
		.sampled(2, tonemapping_pass.sampler,
			tonemapping_mip_view(
				tonemapping_pass.local_reconstruction_pyramid, reconstruction_mip));
	final_writer.commit();
}

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
void tonemapping_pass_prepare_local_debug(
	FrameRenderGraph& graph,
	VulkanContext* ctx,
	const State::TonemappingState& in_state,
	i32 in_selected_full_mip)
{
	tonemapping_pass_ensure_debug_resources(ctx);
	const u32 reconstruction_mip =
		(u32)MAX(tonemapping_pass.effective_reconstruction_mip - 1, 0);
	const u32 coarsest_mip =
		(u32)MAX(tonemapping_pass.effective_coarsest_mip - 1, 0);
	const u32 selected_mip = (u32)CLAMP(
		in_selected_full_mip - 1,
		(i32)reconstruction_mip,
		(i32)coarsest_mip);
	CPU_TIMING_SCOPE("Tonemapping Local Debug");
	const i32 timing_slot =
		gpu_timestamps_begin_scope(ctx, "Tonemapping Local Debug");
	vulkan_begin_debug_label(ctx, "Tonemapping Local Debug");

	if (selected_mip < coarsest_mip)
	{
		graph.sampled(frame_graph_mip(
			tonemapping_pass.local_exposure_pyramid, selected_mip));
		graph.sampled(frame_graph_mip(
			tonemapping_pass.local_exposure_pyramid, selected_mip + 1));
		graph.storage_read(frame_graph_buffer(
			tonemapping_pass.auto_adaptation_state_buffer),
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
		VkImageView laplacian_views[] = {
			tonemapping_mip_view(tonemapping_pass.local_exposure_pyramid, selected_mip),
			tonemapping_mip_view(tonemapping_pass.local_exposure_pyramid, selected_mip + 1),
		};
		VkDescriptorSet laplacian_set =
			tonemapping_local_descriptor_set(ctx, laplacian_views, 2);
		GpuImage* laplacian_output[] = {
			&tonemapping_pass.local_debug_laplacian_pyramid,
		};
		tonemapping_render_mip(
			graph, laplacian_output, 1, selected_mip, [&]() {
				tonemapping_draw_local_stage(
					ctx, tonemapping_pass.local_debug_laplacian_pipeline,
					laplacian_set);
			});
	}

	graph.sampled(tonemapping_pass.scene_color);
	graph.sampled(frame_graph_mip(
		tonemapping_pass.local_exposure_pyramid, reconstruction_mip));
	graph.sampled(frame_graph_mip(
		tonemapping_pass.local_reconstruction_pyramid, reconstruction_mip));
	graph.sampled(frame_graph_image(tonemapping_pass.tonemapping_lut));
	graph.sampled(tonemapping_pass.position);
	graph.storage_read(frame_graph_buffer(
		tonemapping_pass.auto_adaptation_state_buffer),
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
	VkImageView guided_views[] = {
		tonemapping_pass.scene_color_view,
		tonemapping_mip_view(tonemapping_pass.local_exposure_pyramid, reconstruction_mip),
		tonemapping_mip_view(tonemapping_pass.local_reconstruction_pyramid, reconstruction_mip),
		tonemapping_pass.tonemapping_lut.view,
		tonemapping_pass.position_view,
	};
	VkDescriptorSet guided_set =
		tonemapping_local_descriptor_set(
			ctx, guided_views, 5, tonemapping_pass.position_sampler);
	GpuImage* guided_output[] = {
		&tonemapping_pass.local_debug_guided,
	};
	const u32 guide_width = tonemapping_local_mip_extent(
		tonemapping_pass.local_base_width, reconstruction_mip);
	const u32 guide_height = tonemapping_local_mip_extent(
		tonemapping_pass.local_base_height, reconstruction_mip);
	TonemappingLocalDebugGuidedPushConstants constants = {
		.method = (i32)in_state.method,
		.exposure_bias = in_state.exposure_bias,
		.guide_pixel_size = HMM_V2(
			1.0f / (f32)MAX(guide_width, 1u),
			1.0f / (f32)MAX(guide_height, 1u)),
		.lut_integration_scale = tonemapping_lut_integration_scale(ctx, in_state.method),
		.validation_chart = RuntimeConfig::get().tonemap_validation_chart,
		.auto_exposure_enabled = RuntimeConfig::get().tonemap_validation_chart == 0
			&& in_state.auto_exposure_enabled ? 1 : 0,
		.auto_white_balance_enabled = RuntimeConfig::get().tonemap_validation_chart == 0
			&& in_state.auto_white_balance_enabled ? 1 : 0,
		.recovery_limits = HMM_V2(
			in_state.local_shadow_recovery,
			in_state.local_highlight_recovery),
	};
	tonemapping_render_mip(graph, guided_output, 1, 0, [&]() {
		tonemapping_draw_local_stage(
			ctx, tonemapping_pass.local_debug_guided_pipeline, guided_set,
			&constants, sizeof(constants));
	});

	const auto draw_channel = [&](u32 in_panel, FrameGraphImage in_source, i32 in_channel)
	{
		graph.sampled(in_source);
		graph.storage_read(frame_graph_buffer(
				tonemapping_pass.auto_adaptation_state_buffer),
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
		VkImageView views[] = { in_source.view() };
		VkDescriptorSet set = tonemapping_local_descriptor_set(ctx, views, 1);
		TonemappingLocalDebugChannelPushConstants channel_constants = {
			.channel = in_channel,
		};
		tonemapping_render_debug_panel(graph, in_panel, [&]() {
			tonemapping_draw_local_stage(
				ctx, tonemapping_pass.local_debug_channel_pipeline, set,
				&channel_constants, sizeof(channel_constants));
		});
	};
	const FrameGraphImage selected_exposure = frame_graph_mip(
		tonemapping_pass.local_exposure_pyramid, selected_mip);
	const FrameGraphImage selected_weight = frame_graph_mip(
		tonemapping_pass.local_weight_pyramid, selected_mip);
	const FrameGraphImage selected_laplacian = selected_mip < coarsest_mip
		? frame_graph_mip(
			tonemapping_pass.local_debug_laplacian_pyramid, selected_mip)
		: FrameGraphImage {};
	const FrameGraphImage transfer_guide = frame_graph_mip(
		tonemapping_pass.local_exposure_pyramid, reconstruction_mip);
	for (i32 channel = 0; channel < 3; ++channel)
	{
		draw_channel((u32)channel, selected_exposure, channel);
		draw_channel((u32)(3 + channel), selected_weight, channel);
		if (selected_laplacian)
		{
			draw_channel((u32)(6 + channel), selected_laplacian, channel);
		}
		draw_channel(
			(u32)(10 + channel),
			frame_graph_image(tonemapping_pass.local_debug_guided),
			channel);
	}
	draw_channel(9, transfer_guide, 1);
	draw_channel(13, selected_exposure, 3);
	draw_channel(14, frame_graph_image(tonemapping_pass.local_debug_guided), 3);

	for (u32 layer = 0; layer < TONEMAPPING_DEBUG_PANEL_COUNT; ++layer)
	{
		if (selected_mip == coarsest_mip && layer >= 6 && layer <= 8)
		{
			continue;
		}
		graph.sampled(frame_graph_layer(
			tonemapping_pass.local_debug_panels, layer));
	}
	graph.apply();

	TonemappingDebugViewData& debug = tonemapping_pass.debug_view_data;
	debug = {};
	debug.ready = true;
	debug.selected_full_mip = (i32)selected_mip + 1;
	debug.reconstruction_full_mip = (i32)reconstruction_mip + 1;
	debug.coarsest_full_mip = (i32)coarsest_mip + 1;
	debug.base_width = tonemapping_pass.local_base_width;
	debug.base_height = tonemapping_pass.local_base_height;
	for (i32 channel = 0; channel < 3; ++channel)
	{
		debug.exposure[channel] = tonemapping_pass.local_debug_panels.layer_views[channel];
		debug.weights[channel] = tonemapping_pass.local_debug_panels.layer_views[3 + channel];
		debug.laplacians[channel] = selected_mip < coarsest_mip
			? tonemapping_pass.local_debug_panels.layer_views[6 + channel]
			: VK_NULL_HANDLE;
		debug.guided[channel] = tonemapping_pass.local_debug_panels.layer_views[10 + channel];
	}
	debug.selected_reconstruction = tonemapping_mip_view(
		tonemapping_pass.local_reconstruction_pyramid, selected_mip);
	debug.transfer_guide = tonemapping_pass.local_debug_panels.layer_views[9];
	debug.transfer_fused = tonemapping_mip_view(
		tonemapping_pass.local_reconstruction_pyramid, reconstruction_mip);
	debug.geometry_coverage = tonemapping_pass.local_debug_panels.layer_views[13];
	debug.boundary_suppression = tonemapping_pass.local_debug_panels.layer_views[14];
	debug.reconstruction_pyramid =
		&tonemapping_pass.local_reconstruction_pyramid;

	vulkan_end_debug_label(ctx);
	gpu_timestamps_end_scope(ctx, timing_slot);
}

inline const TonemappingDebugViewData& tonemapping_pass_get_debug_view_data()
{
	return tonemapping_pass.debug_view_data;
}
#endif

void tonemapping_pass_draw(
	VulkanContext* ctx,
	const State::TonemappingState& in_state,
	f32 in_bloom_intensity,
	HMM_Vec4 in_bloom_profile_gain,
	f32 in_bloom_auto_exposure_influence)
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
		.bloom_intensity = CLAMP(
			in_bloom_intensity, 0.0f, State::BloomState::MAX_INTENSITY),
		.guide_pixel_size = HMM_V2(
			1.0f / (f32)MAX(guide_width, 1u),
			1.0f / (f32)MAX(guide_height, 1u)),
		.lut_integration_scale = tonemapping_lut_integration_scale(ctx, in_state.method),
		.validation_chart = RuntimeConfig::get().tonemap_validation_chart,
		.bloom_profile_gain = in_bloom_profile_gain,
		.auto_exposure_enabled = RuntimeConfig::get().tonemap_validation_chart == 0
			&& in_state.auto_exposure_enabled ? 1 : 0,
		.auto_white_balance_enabled = RuntimeConfig::get().tonemap_validation_chart == 0
			&& in_state.auto_white_balance_enabled ? 1 : 0,
		.bloom_auto_exposure_influence = CLAMP(
			in_bloom_auto_exposure_influence, 0.0f, 1.0f),
		.local_recovery = HMM_V4(
			in_state.local_shadow_recovery,
			in_state.local_highlight_recovery,
			0.0f, 0.0f),
	};

	tonemapping_pass.final_pipeline.bind(ctx);
	VkDescriptorSet final_set = tonemapping_pass.final_descriptors.current(ctx);
	vkCmdBindDescriptorSets(
		command_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		tonemapping_pass.final_pipeline_layout.layout,
		0, 1, &final_set,
		0, nullptr);
	tonemapping_pass.final_pipeline_layout.push(ctx, constants);
	vulkan_cmd_draw(ctx, 3, 1, 0, 0);
}

void tonemapping_pass_shutdown(VulkanContext* ctx)
{
	gpu_image_destroy(ctx->allocator, ctx->device, tonemapping_pass.tonemapping_lut);
	gpu_image_destroy(
		ctx->allocator, ctx->device, tonemapping_pass.local_exposure_pyramid);
	gpu_image_destroy(
		ctx->allocator, ctx->device, tonemapping_pass.local_weight_pyramid);
	gpu_image_destroy(
		ctx->allocator, ctx->device, tonemapping_pass.local_reconstruction_pyramid);
#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	gpu_image_destroy(
		ctx->allocator, ctx->device, tonemapping_pass.local_debug_laplacian_pyramid);
	gpu_image_destroy(
		ctx->allocator, ctx->device, tonemapping_pass.local_debug_guided);
	gpu_image_destroy(
		ctx->allocator, ctx->device, tonemapping_pass.local_debug_panels);
	tonemapping_pass.local_debug_channel_pipeline.shutdown(ctx);
	tonemapping_pass.local_debug_guided_pipeline.shutdown(ctx);
	tonemapping_pass.local_debug_laplacian_pipeline.shutdown(ctx);
#endif

	tonemapping_pass.local_reconstruct_pipeline.shutdown(ctx);
	tonemapping_pass.local_blend_pipeline.shutdown(ctx);
	tonemapping_pass.local_downsample_pipeline.shutdown(ctx);
	tonemapping_pass.local_proxy_pipeline.shutdown(ctx);
	tonemapping_pass.final_pipeline.shutdown(ctx);
	vkDestroySampler(ctx->device, tonemapping_pass.position_sampler, nullptr);
	tonemapping_pass.local_pipeline_layout.shutdown(ctx);
	tonemapping_pass.final_pipeline_layout.shutdown(ctx);
	tonemapping_pass.local_descriptors.shutdown(ctx);
	tonemapping_pass.final_descriptors.shutdown(ctx);
}
