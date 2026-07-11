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

// Single/Swapchain (Phase 1), Array (Phase 3a shadows), Multi/Cubemap
// (Phase 3c GI captures)
enum class ERenderPassType
{
	Single,
	Multi,		// pass_count independent 2D target sets
	Array,		// one layered image; one slice per pass
	Cubemap,	// one cube image; one face per pass, sampled as CUBE
	Swapchain,
};

static constexpr i32 NUM_CUBE_FACES = 6;

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

	// Single/Array/Cubemap: one image per color output (Array/Cubemap are
	// layered with per-layer attachment views). Multi: one image per output
	// per pass instance, flat-indexed [image_idx * num_outputs + output_idx].
	// depth_outputs mirrors the image-set count.
	StretchyBuffer<GpuImage> color_outputs;
	StretchyBuffer<GpuImage> depth_outputs;

	i32 current_width = -1;
	i32 current_height = -1;

	// Renders only the first N slices of an Array pass this frame
	// (game/'s set_pass_count_override; -1 = all)
	i32 pass_count_override = -1;

	void validate_desc()
	{
		const bool has_any_output = desc.num_outputs > 0 || desc.depth_output.format != VK_FORMAT_UNDEFINED;
		assert(has_any_output || desc.type == ERenderPassType::Swapchain);
		assert(desc.num_outputs <= RENDER_PASS_MAX_COLOR_OUTPUTS);
		assert(desc.type != ERenderPassType::Array || desc.pass_count >= 1);
		assert(desc.type != ERenderPassType::Multi || desc.pass_count >= 1);
	}

	bool has_depth() const
	{
		return desc.depth_output.format != VK_FORMAT_UNDEFINED;
	}

	// Independent target sets (Multi renders each instance into its own images)
	i32 get_image_set_count() const
	{
		return desc.type == ERenderPassType::Multi ? desc.pass_count : 1;
	}

	i32 get_natural_pass_count() const
	{
		switch (desc.type)
		{
			case ERenderPassType::Single:		return 1;
			case ERenderPassType::Multi:		return desc.pass_count;
			case ERenderPassType::Array:		return desc.pass_count;
			case ERenderPassType::Cubemap:		return NUM_CUBE_FACES;
			case ERenderPassType::Swapchain:	return 1;
		}
		assert(false);
		return 1;
	}

	i32 get_pass_count() const
	{
		const i32 natural_pass_count = get_natural_pass_count();
		if (pass_count_override >= 0)
		{
			return MIN(pass_count_override, natural_pass_count);
		}
		return natural_pass_count;
	}

	void set_pass_count_override(i32 in_count)
	{
		pass_count_override = in_count;
	}

	void release_targets()
	{
		for (GpuImage& image : color_outputs)
		{
			gpu_image_destroy(g_vulkan_context->allocator, g_vulkan_context->device, image);
		}
		color_outputs.reset();

		for (GpuImage& image : depth_outputs)
		{
			gpu_image_destroy(g_vulkan_context->allocator, g_vulkan_context->device, image);
		}
		depth_outputs.reset();
	}

	void allocate_outputs()
	{
		if (desc.type == ERenderPassType::Swapchain)
		{
			return;
		}

		const bool is_cubemap = desc.type == ERenderPassType::Cubemap;
		const u32 array_layers = desc.type == ERenderPassType::Array ? (u32) desc.pass_count
								: is_cubemap ? (u32) NUM_CUBE_FACES
								: 1u;
		const i32 image_set_count = get_image_set_count();

		for (i32 image_idx = 0; image_idx < image_set_count; ++image_idx)
		{
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
					.array_layers = array_layers,
					.cubemap = is_cubemap,
				}));
			}

			if (has_depth())
			{
				depth_outputs.add(gpu_image_create(g_vulkan_context->allocator, g_vulkan_context->device, (GpuImageDesc) {
					.width = (u32) current_width,
					.height = (u32) current_height,
					.format = desc.depth_output.format,
					.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
					.aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
					.array_layers = array_layers,
					.cubemap = is_cubemap,
				}));
			}
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

	GpuImage& get_color_output(i32 in_output_idx = 0, i32 in_image_idx = 0)
	{
		const i32 flat_idx = in_image_idx * desc.num_outputs + in_output_idx;
		assert(color_outputs.is_valid_index(flat_idx));
		return color_outputs[flat_idx];
	}

	GpuImage& get_depth_output(i32 in_image_idx = 0)
	{
		assert(depth_outputs.is_valid_index(in_image_idx));
		return depth_outputs[in_image_idx];
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
	// Array passes loop once per slice (callback receives the slice index).
	void execute(VulkanContext* ctx, const std::function<void(i32)>& in_callback)
	{
		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];

		CPU_TIMING_SCOPE(desc.debug_label ? desc.debug_label : "RenderPass");
		const i32 gpu_timing_slot = gpu_timestamps_begin_scope(ctx, desc.debug_label ? desc.debug_label : "RenderPass");

		const bool is_swapchain = desc.type == ERenderPassType::Swapchain;
		const bool is_multi = desc.type == ERenderPassType::Multi;
		const bool is_sliced = desc.type == ERenderPassType::Array || desc.type == ERenderPassType::Cubemap;

		// Own outputs -> attachment layouts (before BeginRendering; the
		// swapchain image was already transitioned by begin_frame).
		// Transitions span all layers and image sets.
		if (!is_swapchain)
		{
			for (i32 flat_idx = 0; flat_idx < (i32) color_outputs.length(); ++flat_idx)
			{
				const i32 output_idx = desc.num_outputs > 0 ? flat_idx % desc.num_outputs : 0;
				const bool discard = desc.outputs[output_idx].load_op != VK_ATTACHMENT_LOAD_OP_LOAD;
				gpu_image_transition(command_buffer, color_outputs[flat_idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, discard);
			}
			for (GpuImage& depth_image : depth_outputs)
			{
				const bool discard = desc.depth_output.load_op != VK_ATTACHMENT_LOAD_OP_LOAD;
				gpu_image_transition(command_buffer, depth_image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, discard);
			}
		}

		const VkExtent2D render_extent = is_swapchain
			? ctx->swapchain_extent
			: (VkExtent2D) { (u32) current_width, (u32) current_height };

		const i32 pass_count = get_pass_count();
		for (i32 pass_idx = 0; pass_idx < pass_count; ++pass_idx)
		{
			VkRenderingAttachmentInfo color_attachments[RENDER_PASS_MAX_COLOR_OUTPUTS] = {};
			u32 color_attachment_count = 0;

			if (is_swapchain)
			{
				// Swapchain passes render straight to the acquired image. Ops
				// come from outputs[0] when declared, else overwrite defaults.
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
					GpuImage& output_image = get_color_output(output_idx, is_multi ? pass_idx : 0);
					color_attachments[color_attachment_count++] = (VkRenderingAttachmentInfo) {
						.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
						.imageView = is_sliced ? output_image.layer_views[pass_idx] : output_image.view,
						.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						.loadOp = desc.outputs[output_idx].load_op,
						.storeOp = desc.outputs[output_idx].store_op,
						.clearValue = desc.outputs[output_idx].clear_value,
					};
				}
			}

			VkRenderingAttachmentInfo depth_attachment = {};
			if (has_depth() && !is_swapchain)
			{
				GpuImage& depth_image = get_depth_output(is_multi ? pass_idx : 0);
				depth_attachment = (VkRenderingAttachmentInfo) {
					.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
					.imageView = is_sliced ? depth_image.layer_views[pass_idx] : depth_image.view,
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
				.pDepthAttachment = (has_depth() && !is_swapchain) ? &depth_attachment : nullptr,
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

			in_callback(pass_idx);

			vkCmdEndRendering(command_buffer);
		}

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
