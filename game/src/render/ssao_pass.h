#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/fullscreen_pipeline.h"
#include "render/gpu_buffer.h"
#include "render/frame_data.h"

#include <random>

#include "ssao_constants.h"

// Hemisphere-kernel SSAO over the G-buffer at half render resolution.
// The raw
// output goes through the generic BlurPass before lighting samples it.

// Mirrors ssao.frag's fs_params block (std140).
struct SsaoFsParams
{
	HMM_Vec2 screen_size;
	f32 _pad0[2];
	HMM_Mat4 view;
	HMM_Mat4 projection;
	HMM_Vec4 kernel_samples[SSAO_KERNEL_SIZE];
	i32 ssao_enable;
	f32 _pad1[3];
};
static_assert(sizeof(SsaoFsParams) == 928, "SsaoFsParams must match ssao.frag's fs_params std140 layout");

struct SsaoPass
{
	TypedFullscreenEffect<SsaoFsParams> effect;

	// Inputs for the SSAO blur (BlurPass): horizontal samples the raw SSAO
	// output, vertical samples the blur's intermediate target. Per-frame
	// because the half-res targets are recreated on resize.
	PerFrameDescriptorSets blur_horizontal_sets;
	PerFrameDescriptorSets blur_vertical_sets;
	SsaoFsParams fs_params_template = {};	// kernel filled once at init

	GpuImage noise_texture;
	VkSampler linear_sampler = VK_NULL_HANDLE;	// borrowed from frame_data
};

static SsaoPass ssao_pass;

void ssao_pass_init(VulkanContext* ctx, VkSampler in_linear_sampler)
{
	ssao_pass.linear_sampler = in_linear_sampler;

	// Noise texture and hemisphere kernel.
	{
		std::uniform_real_distribution<f32> randomf32s(0.0, 1.0);
		std::default_random_engine generator;

		HMM_Vec4 ssao_noise[SSAO_TEXTURE_SIZE];
		for (u32 i = 0; i < SSAO_TEXTURE_SIZE; ++i)
		{
			ssao_noise[i] = HMM_V4(
				randomf32s(generator) * 2.0f - 1.0f,
				randomf32s(generator) * 2.0f - 1.0f,
				0.0f,
				0.0f
			);
		}
		ssao_pass.noise_texture = gpu_image_create_from_data(
			ctx,
			SSAO_TEXTURE_WIDTH,
			SSAO_TEXTURE_WIDTH,
			VK_FORMAT_R32G32B32A32_SFLOAT,
			ssao_noise,
			sizeof(ssao_noise)
		);

		for (u32 i = 0; i < SSAO_KERNEL_SIZE; ++i)
		{
			HMM_Vec3 sample = HMM_V3(
				randomf32s(generator) * 2.0f - 1.0f,
				randomf32s(generator) * 2.0f - 1.0f,
				randomf32s(generator)
			);
			sample = HMM_NormV3(sample);
			sample *= randomf32s(generator);

			// Scale samples s.t. they're more aligned to the kernel center
			f32 scale = (f32) i / (f32) SSAO_KERNEL_SIZE;
			scale = HMM_Lerp(0.1f, scale * scale, 1.0f);
			sample *= scale;

			ssao_pass.fs_params_template.kernel_samples[i] = HMM_V4(sample.X, sample.Y, sample.Z, 0.0f);
		}
	}

	const DescriptorBindingSpec bindings[] = {
		{ .binding = 0, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
		{ .binding = 1, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
		{ .binding = 2, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
		{ .binding = 3, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
	};
	VkFormat ssao_format = Render::SSAO_FORMAT;
	ssao_pass.effect.init(ctx, {
		.vertex_shader_path = "bin/shaders/ssao.vert.spv",
		.fragment_shader_path = "bin/shaders/ssao.frag.spv",
		.color_formats = &ssao_format,
		.color_format_count = 1,
		.bindings = bindings,
		.binding_count = (u32)(sizeof(bindings) / sizeof(bindings[0])),
	}, 0, "SsaoPass::fs_params");
	ssao_pass.blur_horizontal_sets.init_persistent(ctx, frame_data.sampled_input_layout);
	ssao_pass.blur_vertical_sets.init_persistent(ctx, frame_data.sampled_input_layout);
}

// Uploads fs_params + rewrites this frame's set (after the fence wait, before
// any binds record). Pass the SSAO pass target size (half render res).
void ssao_pass_update(
	VulkanContext* ctx,
	HMM_Vec2 in_target_size,
	const HMM_Mat4& in_view,
	const HMM_Mat4& in_projection,
	bool in_enable,
	VkImageView in_gbuffer_position_view,
	VkImageView in_gbuffer_normal_view,
	VkImageView in_ssao_output_view,
	VkImageView in_blur_intermediate_view
)
{
	SsaoFsParams fs_params = ssao_pass.fs_params_template;
	fs_params.screen_size = in_target_size;
	fs_params.view = in_view;
	fs_params.projection = in_projection;
	fs_params.ssao_enable = in_enable ? 1 : 0;
	DescriptorWriter writer = ssao_pass.effect.writer(ctx, fs_params);
	writer.sampled(1, ssao_pass.linear_sampler, in_gbuffer_position_view)
		.sampled(2, ssao_pass.linear_sampler, in_gbuffer_normal_view)
		.sampled(3, ssao_pass.linear_sampler, ssao_pass.noise_texture.view)
		.commit();

	VkDescriptorImageInfo blur_input_infos[] = {
		descriptor_sampled(ssao_pass.linear_sampler, in_ssao_output_view),
		descriptor_sampled(ssao_pass.linear_sampler, in_blur_intermediate_view),
	};
	VkWriteDescriptorSet writes[2] = {};
	writes[0] = descriptor_write_image(ssao_pass.blur_horizontal_sets.current(ctx), 0,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blur_input_infos[0]);
	writes[1] = descriptor_write_image(ssao_pass.blur_vertical_sets.current(ctx), 0,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blur_input_infos[1]);
	vulkan_update_descriptor_sets(ctx, 2, writes);
}

void ssao_pass_draw(VulkanContext* ctx)
{
	ssao_pass.effect.draw(ctx);
}

void ssao_pass_shutdown(VulkanContext* ctx)
{
	ssao_pass.effect.shutdown(ctx);
	vulkan_context_retire_image(ctx, ssao_pass.noise_texture);
}
