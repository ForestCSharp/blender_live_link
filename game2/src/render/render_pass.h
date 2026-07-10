#pragma once

#include <cassert>
#include <functional>
#include <optional>

#include "core/types.h"
#include "core/stretchy_buffer.h"
#include "core/timings.h"
#include "render/vulkan_context.h"

using std::optional;

// Port of game/src/render/render_pass.h onto Vulkan dynamic rendering.
//
// The framework owns pass targets (allocation + resize), image layout
// transitions for its own outputs, render begin/end, the uniform
// negative-height (Y-flip) viewport, and timing scopes. Pass files own
// pipelines and record draws in the execute callback.
//
// Cross-pass wiring is imperative, like game/: a pass reads another pass's
// output by fetching get_color_output(...) and transitioning it to
// SHADER_READ_ONLY_OPTIMAL *before* calling execute (barriers are illegal
// inside dynamic rendering). There is no dependency graph — passes run in
// ERenderPass enum order.

// Phase 1 implements Single and Swapchain; Multi/Array/Cubemap arrive with
// the passes that need them (Phase 3)
enum class ERenderPassType
{
	Single,
	Multi,
	Array,
	Cubemap,
	Swapchain,
};

static constexpr i32 RENDER_PASS_MAX_COLOR_OUTPUTS = 4;

struct RenderPassOutputDesc
{
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	VkAttachmentStoreOp store_op = VK_ATTACHMENT_STORE_OP_STORE;
	VkClearValue clear_value = {};
};

struct RenderPassDesc
{
	i32 initial_width = -1;
	i32 initial_height = -1;
	i32 pass_count = 1;

	i32 num_outputs = 0;
	RenderPassOutputDesc outputs[RENDER_PASS_MAX_COLOR_OUTPUTS];
	RenderPassOutputDesc depth_output;	// format == UNDEFINED means no depth

	f32 width_scale = 1.0f;
	f32 height_scale = 1.0f;
	bool resize_with_window = true;
	ERenderPassType type = ERenderPassType::Single;
	const char* debug_label = nullptr;
};

struct RenderPass
{
	RenderPassDesc desc = {};

	// One image per color output (Single); Multi/Array/Cubemap grow this in Phase 3
	StretchyBuffer<GpuImage> color_outputs;
	optional<GpuImage> depth_output;

	i32 current_width = -1;
	i32 current_height = -1;

	void validate_desc()
	{
		const bool has_any_output = desc.num_outputs > 0 || desc.depth_output.format != VK_FORMAT_UNDEFINED;
		assert(has_any_output || desc.type == ERenderPassType::Swapchain);
		assert(desc.num_outputs <= RENDER_PASS_MAX_COLOR_OUTPUTS);
		assert(desc.type == ERenderPassType::Single || desc.type == ERenderPassType::Swapchain); // Multi/Array/Cubemap: Phase 3
	}

	void release_targets()
	{
		for (GpuImage& image : color_outputs)
		{
			gpu_image_destroy(g_vulkan_context->allocator, g_vulkan_context->device, image);
		}
		color_outputs.reset();

		if (depth_output.has_value())
		{
			gpu_image_destroy(g_vulkan_context->allocator, g_vulkan_context->device, *depth_output);
			depth_output.reset();
		}
	}

	void allocate_outputs()
	{
		if (desc.type == ERenderPassType::Swapchain)
		{
			return;
		}

		for (i32 output_idx = 0; output_idx < desc.num_outputs; ++output_idx)
		{
			color_outputs.add(gpu_image_create(g_vulkan_context->allocator, g_vulkan_context->device, (GpuImageDesc) {
				.width = (u32) current_width,
				.height = (u32) current_height,
				.format = desc.outputs[output_idx].format,
				.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
					   | VK_IMAGE_USAGE_SAMPLED_BIT
					   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
				.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
			}));
		}

		if (desc.depth_output.format != VK_FORMAT_UNDEFINED)
		{
			depth_output = gpu_image_create(g_vulkan_context->allocator, g_vulkan_context->device, (GpuImageDesc) {
				.width = (u32) current_width,
				.height = (u32) current_height,
				.format = desc.depth_output.format,
				.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				.aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
			});
		}
	}

	void init(const RenderPassDesc& in_desc)
	{
		cleanup();
		desc = in_desc;
		validate_desc();

		if (desc.initial_width > 0 && desc.initial_height > 0)
		{
			handle_resize(desc.initial_width, desc.initial_height);
		}
	}

	GpuImage& get_color_output(i32 in_output_idx = 0)
	{
		assert(color_outputs.is_valid_index(in_output_idx));
		return color_outputs[in_output_idx];
	}

	GpuImage& get_depth_output()
	{
		assert(depth_output.has_value());
		return *depth_output;
	}

	// Recreates targets at the new size. Destruction is immediate (the
	// deletion queue handles buffers only) — callers must have idled the
	// device (main.cpp's handle_resize does).
	void handle_resize(i32 in_width, i32 in_height)
	{
		if (!desc.resize_with_window)
		{
			// Fixed-size passes allocate once at initial_* and never resize
			if (current_width > 0)
			{
				return;
			}
			in_width = desc.initial_width;
			in_height = desc.initial_height;
		}

		const i32 new_width = MAX(1, (i32)(in_width * desc.width_scale + 0.5f));
		const i32 new_height = MAX(1, (i32)(in_height * desc.height_scale + 0.5f));
		if (new_width == current_width && new_height == current_height)
		{
			return;
		}

		current_width = new_width;
		current_height = new_height;

		if (desc.type != ERenderPassType::Swapchain)
		{
			release_targets();
			allocate_outputs();
		}
	}

	// Transitions outputs, begins dynamic rendering with the declared
	// attachments, sets the Y-flipped viewport + scissor, runs the callback,
	// ends rendering. The callback binds its own pipeline/sets and draws.
	void execute(VulkanContext* ctx, const std::function<void(i32)>& in_callback)
	{
		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];

		CPU_TIMING_SCOPE(desc.debug_label ? desc.debug_label : "RenderPass");
		const i32 gpu_timing_slot = gpu_timestamps_begin_scope(ctx, desc.debug_label ? desc.debug_label : "RenderPass");

		const bool is_swapchain = desc.type == ERenderPassType::Swapchain;

		// Own outputs -> attachment layouts (before BeginRendering; the
		// swapchain image was already transitioned by begin_frame)
		if (!is_swapchain)
		{
			for (i32 output_idx = 0; output_idx < desc.num_outputs; ++output_idx)
			{
				const bool discard = desc.outputs[output_idx].load_op != VK_ATTACHMENT_LOAD_OP_LOAD;
				gpu_image_transition(command_buffer, color_outputs[output_idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, discard);
			}
			if (depth_output.has_value())
			{
				const bool discard = desc.depth_output.load_op != VK_ATTACHMENT_LOAD_OP_LOAD;
				gpu_image_transition(command_buffer, *depth_output, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, discard);
			}
		}

		const VkExtent2D render_extent = is_swapchain
			? ctx->swapchain_extent
			: (VkExtent2D) { (u32) current_width, (u32) current_height };

		VkRenderingAttachmentInfo color_attachments[RENDER_PASS_MAX_COLOR_OUTPUTS] = {};
		u32 color_attachment_count = 0;

		if (is_swapchain)
		{
			// Swapchain passes render straight to the acquired image. Ops come
			// from outputs[0] when declared, else overwrite-everything defaults.
			const RenderPassOutputDesc& output_desc = desc.outputs[0];
			color_attachments[color_attachment_count++] = (VkRenderingAttachmentInfo) {
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = ctx->swapchain_image_views[ctx->swapchain_image_index],
				.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				.loadOp = desc.num_outputs > 0 ? output_desc.load_op : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.storeOp = desc.num_outputs > 0 ? output_desc.store_op : VK_ATTACHMENT_STORE_OP_STORE,
				.clearValue = output_desc.clear_value,
			};
		}
		else
		{
			for (i32 output_idx = 0; output_idx < desc.num_outputs; ++output_idx)
			{
				color_attachments[color_attachment_count++] = (VkRenderingAttachmentInfo) {
					.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
					.imageView = color_outputs[output_idx].view,
					.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					.loadOp = desc.outputs[output_idx].load_op,
					.storeOp = desc.outputs[output_idx].store_op,
					.clearValue = desc.outputs[output_idx].clear_value,
				};
			}
		}

		VkRenderingAttachmentInfo depth_attachment = {};
		if (depth_output.has_value())
		{
			depth_attachment = (VkRenderingAttachmentInfo) {
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = depth_output->view,
				.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
				.loadOp = desc.depth_output.load_op,
				.storeOp = desc.depth_output.store_op,
				.clearValue = desc.depth_output.clear_value,
			};
		}

		VkRenderingInfo rendering_info = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea = {
				.offset = { 0, 0 },
				.extent = render_extent,
			},
			.layerCount = 1,
			.colorAttachmentCount = color_attachment_count,
			.pColorAttachments = color_attachment_count > 0 ? color_attachments : nullptr,
			.pDepthAttachment = depth_output.has_value() ? &depth_attachment : nullptr,
		};

		vkCmdBeginRendering(command_buffer, &rendering_info);

		// Uniform Y-flip convention across all passes
		VkViewport flipped_viewport = {
			.x = 0.0f,
			.y = (f32) render_extent.height,
			.width = (f32) render_extent.width,
			.height = -(f32) render_extent.height,
			.minDepth = 0.0f,
			.maxDepth = 1.0f,
		};
		vkCmdSetViewport(command_buffer, 0, 1, &flipped_viewport);

		VkRect2D scissor = {
			.offset = { 0, 0 },
			.extent = render_extent,
		};
		vkCmdSetScissor(command_buffer, 0, 1, &scissor);

		in_callback(0);

		vkCmdEndRendering(command_buffer);

		gpu_timestamps_end_scope(ctx, gpu_timing_slot);
	}

	void cleanup()
	{
		release_targets();
		current_width = -1;
		current_height = -1;
	}
};

// Entry with an optional intermediate pass (separable blurs etc. — game/ parity)
struct RenderPassEntry
{
	RenderPass final;
	optional<RenderPass> intermediate;

	RenderPass& final_pass() { return final; }

	RenderPass& intermediate_pass()
	{
		assert(intermediate.has_value());
		return *intermediate;
	}

	RenderPass& ensure_intermediate_pass()
	{
		if (!intermediate.has_value())
		{
			intermediate.emplace();
		}
		return *intermediate;
	}

	void init_final(const RenderPassDesc& in_desc) { final.init(in_desc); }
	void init_intermediate(const RenderPassDesc& in_desc) { ensure_intermediate_pass().init(in_desc); }

	void handle_resize(i32 in_width, i32 in_height)
	{
		if (intermediate.has_value())
		{
			intermediate->handle_resize(in_width, in_height);
		}
		final.handle_resize(in_width, in_height);
	}

	void cleanup()
	{
		if (intermediate.has_value())
		{
			intermediate->cleanup();
			intermediate.reset();
		}
		final.cleanup();
	}
};
