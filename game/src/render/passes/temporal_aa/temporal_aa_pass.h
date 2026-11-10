#pragma once

#include "core/types.h"
#include "render/core/vulkan_context.h"
#include "render/core/render_types.h"
#include "render/core/fullscreen_pipeline.h"
#include "render/core/frame_render_graph.h"
#include "state/state.h"

// Decima-style two-phase jitter. Each history target stores resolved color
// and history; the shader samples history from the opposite target.

// Mirrors temporal_aa.frag's fs_params (std140)
struct TemporalAaFsParams
{
	HMM_Mat4 previous_view_projection;
	HMM_Vec2 screen_size;
	HMM_Vec2 sharpen_axis;
	f32 blend_alpha;
	f32 sharpen_strength;
	f32 rejection_threshold;
	i32 history_valid;
	i32 debug_mode;
	i32 reactive_enable;
	f32 _pad0[2];
};
static_assert(sizeof(TemporalAaFsParams) == 112, "TemporalAaFsParams must match temporal_aa.frag's fs_params std140 layout");

namespace TemporalAAPass
{
	inline TypedFullscreenEffect<TemporalAaFsParams> effect;
	inline VkSampler linear_sampler = VK_NULL_HANDLE;	// borrowed from frame_data
	inline VkSampler nearest_sampler = VK_NULL_HANDLE;	// owned (position reads)

	inline void invalidate_history(State& in_state)
	{
		in_state.temporal_aa.history_valid = false;
	}

	inline HMM_Vec2 get_decima_jitter_pixels(i32 jitter_phase)
	{
		return (jitter_phase & 1) == 1
			? HMM_V2(0.5f, 0.0f)
			: HMM_V2(0.0f, 0.5f);
	}

	inline HMM_Mat4 apply_projection_jitter(HMM_Mat4 in_projection, HMM_Vec2 jitter_pixels, HMM_Vec2 screen_size)
	{
		if (screen_size.X <= 0.0f || screen_size.Y <= 0.0f)
		{
			return in_projection;
		}

		const HMM_Vec2 jitter_ndc = HMM_V2(
			(2.0f * jitter_pixels.X) / screen_size.X,
			(-2.0f * jitter_pixels.Y) / screen_size.Y
		);
		for (i32 column = 0; column < 4; ++column)
		{
			in_projection.Elements[column][0] += jitter_ndc.X * in_projection.Elements[column][3];
			in_projection.Elements[column][1] += jitter_ndc.Y * in_projection.Elements[column][3];
		}
		return in_projection;
	}

	inline void init(VulkanContext* ctx, VkSampler in_linear_sampler)
	{
		linear_sampler = in_linear_sampler;

		VkSamplerCreateInfo nearest_create_info = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_NEAREST,
			.minFilter = VK_FILTER_NEAREST,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};
		VK_CHECK(vkCreateSampler(ctx->device, &nearest_create_info, nullptr, &nearest_sampler));

		const DescriptorBindingSpec bindings[] = {
			{ .binding = 0, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
			{ .binding = 1, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
			{ .binding = 2, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
			{ .binding = 3, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
			{ .binding = 4, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
		};
		const VkFormat formats[] = { Render::SCENE_COLOR_FORMAT, Render::SCENE_COLOR_FORMAT };
		effect.init(ctx, {
			.vertex_shader_path = "bin/shaders/temporal_aa.vert.spv",
			.fragment_shader_path = "bin/shaders/temporal_aa.frag.spv",
			.color_formats = formats,
			.color_format_count = 2,
			.bindings = bindings,
			.binding_count = 5,
		}, 0, "TemporalAAPass::fs_params");
	}

	inline FrameGraphImage record(
		FrameRenderGraph& graph, State& in_state, const HMM_Mat4& view_projection,
		FrameGraphImage scene_color, FrameGraphImage position, FrameGraphImage cloud_reactive_mask)
	{
		if (!in_state.temporal_aa.enable) return scene_color;

		const bool write_first = in_state.temporal_aa.history_index == 0;
		RenderPass& target = in_state.render_targets.get(write_first
			? RenderTargetId::TemporalAAHistory0 : RenderTargetId::TemporalAAHistory1);
		RenderPass& previous = in_state.render_targets.get(write_first
			? RenderTargetId::TemporalAAHistory1 : RenderTargetId::TemporalAAHistory0);
		const FrameGraphImage mask = cloud_reactive_mask ? cloud_reactive_mask
			: frame_graph_color(in_state.render_targets.get(RenderTargetId::Geometry));
		TemporalAaFsParams temporal_aa_fs_params = {
			.previous_view_projection = in_state.temporal_aa.previous_view_projection,
			.screen_size = HMM_V2((f32) in_state.window.render_width, (f32) in_state.window.render_height),
			.sharpen_axis = (in_state.temporal_aa.jitter_phase & 1) == 1 ? HMM_V2(1.0f, 0.0f) : HMM_V2(0.0f, 1.0f),
			.blend_alpha = in_state.temporal_aa.blend_alpha,
			.sharpen_strength = in_state.temporal_aa.sharpen_strength,
			.rejection_threshold = in_state.temporal_aa.rejection_threshold,
			.history_valid = in_state.temporal_aa.history_valid ? 1 : 0,
			.debug_mode = in_state.temporal_aa.debug_mode,
			.reactive_enable = cloud_reactive_mask ? 1 : 0,
		};
		DescriptorWriter writer = effect.writer(&in_state.vk, temporal_aa_fs_params);
		writer.sampled(1, linear_sampler, graph.sampled(scene_color).view())
			.sampled(2, nearest_sampler, graph.sampled(position).view())
			.sampled(3, linear_sampler, graph.sampled(frame_graph_color(previous, 1)).view())
			.sampled(4, linear_sampler, graph.sampled(mask).view())
			.commit();
		graph.execute(target, [&](i32) { effect.draw(&in_state.vk); });

		in_state.temporal_aa.previous_view_projection = view_projection;
		in_state.temporal_aa.history_valid = true;
		in_state.temporal_aa.history_index = write_first ? 1 : 0;
		return frame_graph_color(target);
	}

	inline void shutdown(VulkanContext* ctx)
	{
		effect.shutdown(ctx);
		vkDestroySampler(ctx->device, nearest_sampler, nullptr);
	}
}
