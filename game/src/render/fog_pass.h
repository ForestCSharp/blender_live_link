#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/fullscreen_pipeline.h"
#include "render/gpu_buffer.h"
#include "render/bruneton_atmosphere_pass.h"

// Exponential height fog over the lit scene and, when active, the cloud
// composite. Runs after lighting and cloud compositing when an enabled fog
// controller exists; downstream passes read this output instead of the
// pre-fog scene color.

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
	i32 atmosphere_enabled;
	f32 atmosphere_planet_center_z;
	f32 _atmosphere_pad0;
	f32 _atmosphere_pad1;
	i32 cloud_enabled;
	f32 _cloud_pad0[3];
};
static_assert(sizeof(FogFsParams) == 128, "FogFsParams must match fog.frag's fs_params std140 layout");

struct FogPass
{
	TypedFullscreenEffect<FogFsParams> effect;
	VkSampler linear_sampler = VK_NULL_HANDLE;	// borrowed from frame_data
	VkSampler nearest_sampler = VK_NULL_HANDLE;
};

static FogPass fog_pass;

void fog_pass_init(VulkanContext* ctx, VkSampler in_linear_sampler)
{
	fog_pass.linear_sampler = in_linear_sampler;
	VkSamplerCreateInfo nearest_create_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	VK_CHECK(vkCreateSampler(ctx->device, &nearest_create_info, nullptr,
		&fog_pass.nearest_sampler));

	const DescriptorBindingSpec bindings[] = {
		{ .binding = 0, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
		{ .binding = 1, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
		{ .binding = 2, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
		{ .binding = 3, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
		{ .binding = 4, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
		{ .binding = 5, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
		{ .binding = 6, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
	};
	VkFormat color_format = Render::SCENE_COLOR_FORMAT;
	fog_pass.effect.init(ctx, {
		.vertex_shader_path = "bin/shaders/fog.vert.spv",
		.fragment_shader_path = "bin/shaders/fog.frag.spv",
		.color_formats = &color_format,
		.color_format_count = 1,
		.bindings = bindings,
		.binding_count = (u32)(sizeof(bindings) / sizeof(bindings[0])),
	}, 0, "FogPass::fs_params");
}

// Uploads fs_params + rewrites this frame's set (after the fence wait, before
// any binds record)
void fog_pass_update(
	VulkanContext* ctx,
	const FogFsParams& in_fs_params,
	VkImageView in_composite_color_view,
	VkImageView in_geometry_position_view,
	VkBuffer in_atmosphere_parameters_buffer,
	VkImageView in_atmosphere_transmittance_view,
	VkImageView in_background_color_view,
	VkImageView in_cloud_metadata_view
)
{
	DescriptorWriter writer = fog_pass.effect.writer(ctx, in_fs_params);
	writer.sampled(1, fog_pass.linear_sampler, in_composite_color_view)
		.sampled(2, fog_pass.nearest_sampler, in_geometry_position_view)
		.buffer(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			in_atmosphere_parameters_buffer, sizeof(BrunetonAtmosphereGpu))
		.sampled(4, fog_pass.linear_sampler, in_atmosphere_transmittance_view)
		.sampled(5, fog_pass.linear_sampler, in_background_color_view)
		.sampled(6, fog_pass.nearest_sampler, in_cloud_metadata_view)
		.commit();
}

void fog_pass_draw(VulkanContext* ctx)
{
	fog_pass.effect.draw(ctx);
}

void fog_pass_shutdown(VulkanContext* ctx)
{
	fog_pass.effect.shutdown(ctx);
	vkDestroySampler(ctx->device, fog_pass.nearest_sampler, nullptr);
}
