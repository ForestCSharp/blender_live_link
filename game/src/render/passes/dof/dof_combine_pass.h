#pragma once

#include "core/types.h"
#include "render/core/vulkan_context.h"
#include "render/core/render_types.h"
#include "render/core/fullscreen_pipeline.h"
#include "render/core/frame_render_graph.h"
#include "state/state.h"

// Single-pass gather depth-of-field.
// Reads the post-fog scene color and
// Composite world position (geometry=1, cloud-only=2, sky=0); writes the
// DOF'd scene color.

// Mirrors dof_combine.frag's fs_params block (std140).
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
static_assert(sizeof(DofCombineFsParams) == 64, "DofCombineFsParams must match dof_combine.frag's fs_params std140 layout");

struct DofCombinePass
{
	TypedFullscreenEffect<DofCombineFsParams> effect;
	VkSampler linear_sampler = VK_NULL_HANDLE;	// borrowed from frame_data
};

static DofCombinePass dof_combine_pass;

void dof_combine_pass_init(VulkanContext* ctx, VkSampler in_linear_sampler)
{
	dof_combine_pass.linear_sampler = in_linear_sampler;

	const DescriptorBindingSpec bindings[] = {
		{ .binding = 0, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
		{ .binding = 1, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
		{ .binding = 2, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
	};
	VkFormat color_format = Render::SCENE_COLOR_FORMAT;
	dof_combine_pass.effect.init(ctx, {
		.vertex_shader_path = "bin/shaders/dof_combine.vert.spv",
		.fragment_shader_path = "bin/shaders/dof_combine.frag.spv",
		.color_formats = &color_format,
		.color_format_count = 1,
		.bindings = bindings,
		.binding_count = (u32)(sizeof(bindings) / sizeof(bindings[0])),
	}, 0, "DofCombinePass::fs_params");
}

FrameGraphImage dof_combine_pass_record(
	FrameRenderGraph& graph, State& in_state, const Camera& camera,
	FrameGraphImage scene_color, FrameGraphImage position)
{
	if (!in_state.dof.enable) return scene_color;

	DofCombineFsParams dof_combine_fs_params = {
		.cam_pos = HMM_V4V(camera.location, 1.0f),
		.cam_forward = HMM_V4V(camera.forward, 0.0f),
		.screen_size = HMM_V2((f32) in_state.window.render_width, (f32) in_state.window.render_height),
		.focus_distance = in_state.dof.focus_distance,
		.focus_range = in_state.dof.focus_range,
		.max_coc_radius = in_state.dof.max_coc_radius,
		.foreground_blur_scale = in_state.dof.foreground_blur_scale,
		.background_blur_scale = in_state.dof.background_blur_scale,
		.debug_mode = in_state.dof.debug_show_coc ? 1 : 0,
	};
	DescriptorWriter writer = dof_combine_pass.effect.writer(&in_state.vk, dof_combine_fs_params);
	writer.sampled(1, dof_combine_pass.linear_sampler, graph.sampled(scene_color).view())
		.sampled(2, dof_combine_pass.linear_sampler, graph.sampled(position).view())
		.commit();

	RenderPass& target = in_state.render_targets.get(RenderTargetId::DofCombine);
	graph.execute(target, [&](i32) { dof_combine_pass.effect.draw(&in_state.vk); });
	return frame_graph_color(target);
}

void dof_combine_pass_shutdown(VulkanContext* ctx)
{
	dof_combine_pass.effect.shutdown(ctx);
}
