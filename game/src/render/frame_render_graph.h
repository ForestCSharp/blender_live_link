#pragma once

#include <functional>

#include "render/render_pass.h"

// Lightweight ordered frame graph. It does not schedule or own images: it
// resolves persistent/imported resources, batches declared reads immediately
// before a node, and delegates attachment writes to RenderPass::execute.
struct FrameGraphImage
{
	GpuImage* image = nullptr;

	VkImageView view() const
	{
		assert(image);
		return image->view;
	}

	explicit operator bool() const { return image != nullptr; }
};

inline FrameGraphImage frame_graph_color(
	RenderPass& in_target,
	i32 in_output_index = 0,
	i32 in_image_index = 0)
{
	return { .image = &in_target.get_color_output(in_output_index, in_image_index) };
}

inline FrameGraphImage frame_graph_select(
	bool in_condition,
	FrameGraphImage in_enabled,
	FrameGraphImage in_fallback)
{
	return in_condition ? in_enabled : in_fallback;
}

struct FrameRenderGraph
{
	explicit FrameRenderGraph(VulkanContext* in_ctx) : ctx(in_ctx)
	{
		pending_reads.images.reserve(16);
	}

	FrameGraphImage sampled(FrameGraphImage in_resource)
	{
		assert(in_resource.image);
		GpuImage& image = *in_resource.image;
		pending_reads.images.add({
			.image = &image,
			.range = {
				.aspectMask = image.aspects,
				.baseMipLevel = 0,
				.levelCount = VK_REMAINING_MIP_LEVELS,
				.baseArrayLayer = 0,
				.layerCount = VK_REMAINING_ARRAY_LAYERS,
			},
			.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		});
		return in_resource;
	}

	void apply_reads()
	{
		vulkan_apply_pass_resource_usage(ctx, pending_reads);
		pending_reads.images.clear();
		pending_reads.buffers.clear();
	}

	void make_sampled(FrameGraphImage in_resource)
	{
		sampled(in_resource);
		apply_reads();
	}

	void execute(
		RenderPass& in_target,
		const std::function<void(i32)>& in_callback,
		i32 in_pass_count = -1)
	{
		apply_reads();
		in_target.execute(ctx, in_callback, in_pass_count);
	}

private:
	VulkanContext* ctx = nullptr;
	PassResourceUsage pending_reads;
};
