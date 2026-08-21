#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/fullscreen_pipeline.h"
#include "render/frame_data.h"

// Luma FXAA over the tonemapped LDR target.
// Uses layout B (single sampled input) + a small push constant block; the
// copy pass samples this output when enabled.

namespace FXAAPass
{
	struct PushConstants
	{
		HMM_Vec2 screen_size;
		f32 contrast_threshold;
		f32 relative_threshold;
	};
	static_assert(sizeof(PushConstants) == 16, "Must match fxaa.frag's push constant block");

	inline FullscreenEffect effect;
	inline VkSampler linear_sampler = VK_NULL_HANDLE;	// borrowed from frame_data

	inline void init(VulkanContext* ctx, VkSampler in_linear_sampler)
	{
		linear_sampler = in_linear_sampler;

		// Keep post-tonemap filtering in float; the copy pass owns the only
		// conversion to the display format.
		const DescriptorBindingSpec bindings[] = {
			{ .binding = 0, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
		};
		effect.init(ctx, {
			.vertex_shader_path = "bin/shaders/fxaa.vert.spv",
			.fragment_shader_path = "bin/shaders/fxaa.frag.spv",
			.color_formats = &Render::SCENE_COLOR_FORMAT,
			.color_format_count = 1,
			.bindings = bindings,
			.binding_count = 1,
			.push_constant_size = sizeof(PushConstants),
		});
	}

	// Points this frame's input set at the tonemapped LDR target
	inline void update(VulkanContext* ctx, VkImageView in_tonemapped_view)
	{
		DescriptorWriter writer = effect.writer(ctx);
		writer.sampled(0, linear_sampler, in_tonemapped_view).commit();
	}

	inline void draw(VulkanContext* ctx, HMM_Vec2 in_screen_size)
	{
		// Tuned FXAA thresholds.
		PushConstants push_constants = {
			.screen_size = in_screen_size,
			.contrast_threshold = 0.0312f,
			.relative_threshold = 0.125f,
		};
		effect.draw(ctx, &push_constants);
	}

	inline void shutdown(VulkanContext* ctx)
	{
		effect.shutdown(ctx);
	}
}
