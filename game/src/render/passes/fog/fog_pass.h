#pragma once

#include "core/types.h"
#include "render/core/vulkan_context.h"
#include "render/core/render_types.h"
#include "render/core/fullscreen_pipeline.h"
#include "render/core/gpu_buffer.h"
#include "render/core/frame_render_graph.h"
#include "render/passes/sky/sky_pass.h"

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

FrameGraphImage fog_pass_record(
	FrameRenderGraph& graph, State& in_state, const PerFrameData& per_frame_data,
	FrameGraphImage scene_color, FrameGraphImage geometry_position,
	FrameGraphImage lighting, FrameGraphImage cloud_metadata)
{
	const auto controller_id = in_state.fog.active_fog_controller_id;
	if (!in_state.fog.active || !in_state.fog.debug_active || !controller_id)
		return scene_color;
	const auto controller = in_state.scene.objects.find(*controller_id);
	if (controller == in_state.scene.objects.end()) return scene_color;

	const FogController& fog_controller = controller->second.fog_controller;
	FogFsParams fog_fs_params = {
		.camera_position = per_frame_data.camera_position.XYZ,
		.fog_base_height = fog_controller.base_height,
		.fog_color = fog_controller.fog_color,
		.density = fog_controller.density,
		.scale_height = fog_controller.scale_height,
		.max_distance = fog_controller.max_distance,
		.ceiling_enabled = fog_controller.ceiling_enabled ? 1 : 0,
		.ceiling_height = fog_controller.ceiling_height,
		.ceiling_fade = fog_controller.ceiling_fade,
		.ambient_intensity = fog_controller.ambient_intensity,
		.sun_intensity = fog_controller.sun_intensity,
		.anisotropy = fog_controller.anisotropy,
		// No real sun means no sun in-scatter; the PerFrameData fallback
		// sun drives only the sky.
		.sun_direction = in_state.scene.primary_sun_id ? per_frame_data.sun_direction.XYZ : HMM_V3(0.0f, 0.0f, -1.0f),
		.sun_color = in_state.scene.primary_sun_id ? per_frame_data.sun_color.XYZ : HMM_V3(0.0f, 0.0f, 0.0f),
		.atmosphere_enabled = sky_pass.has_active_atmosphere
			&& bruneton_atmosphere_pass.has_precomputed ? 1 : 0,
		.atmosphere_planet_center_z = sky_pass.active_parameters.planet_center_z_m,
		.cloud_enabled = cloud_metadata ? 1 : 0,
	};
	const VkBuffer atmosphere_parameters =
		bruneton_atmosphere_pass.parameter_buffers[in_state.vk.frame_index].get_gpu_buffer();
	const FrameGraphImage transmittance = frame_graph_color(bruneton_atmosphere_pass.transmittance_pass);
	const FrameGraphImage metadata = cloud_metadata ? cloud_metadata
		: frame_graph_color(in_state.render_targets.get(RenderTargetId::Geometry));
	graph.uniform(frame_graph_buffer(atmosphere_parameters), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
	DescriptorWriter writer = fog_pass.effect.writer(&in_state.vk, fog_fs_params);
	writer.sampled(1, fog_pass.linear_sampler, graph.sampled(scene_color).view())
		.sampled(2, fog_pass.nearest_sampler, graph.sampled(geometry_position).view())
		.buffer(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, atmosphere_parameters, sizeof(BrunetonAtmosphereGpu))
		.sampled(4, fog_pass.linear_sampler, graph.sampled(transmittance).view())
		.sampled(5, fog_pass.linear_sampler, graph.sampled(lighting).view())
		.sampled(6, fog_pass.nearest_sampler, graph.sampled(metadata).view())
		.commit();

	RenderPass& target = in_state.render_targets.get(RenderTargetId::Fog);
	graph.execute(target, [&](i32) { fog_pass.effect.draw(&in_state.vk); });
	return frame_graph_color(target);
}

void fog_pass_shutdown(VulkanContext* ctx)
{
	fog_pass.effect.shutdown(ctx);
	vkDestroySampler(ctx->device, fog_pass.nearest_sampler, nullptr);
}
