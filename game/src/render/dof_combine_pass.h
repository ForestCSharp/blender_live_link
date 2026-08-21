#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/fullscreen_pipeline.h"
#include "render/gpu_buffer.h"

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

// Uploads fs_params + rewrites this frame's set (after the fence wait, before
// any binds record)
void dof_combine_pass_update(
	VulkanContext* ctx,
	const DofCombineFsParams& in_fs_params,
	VkImageView in_scene_color_view,
	VkImageView in_gbuffer_position_view
)
{
	DescriptorWriter writer = dof_combine_pass.effect.writer(ctx, in_fs_params);
	writer.sampled(1, dof_combine_pass.linear_sampler, in_scene_color_view)
		.sampled(2, dof_combine_pass.linear_sampler, in_gbuffer_position_view)
		.commit();
}

void dof_combine_pass_draw(VulkanContext* ctx)
{
	dof_combine_pass.effect.draw(ctx);
}

void dof_combine_pass_shutdown(VulkanContext* ctx)
{
	dof_combine_pass.effect.shutdown(ctx);
}
