#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <optional>
#include <utility>

#include "core/types.h"
#include "core/stretchy_buffer.h"
#include "core/timings.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_glue.h"
#include "render/sokol_helpers.h"

using std::optional;

using RenderPassDebugLabelFormatter = const char* (*)(const char* base_label, i32 pass_idx, char* out_label, size_t out_label_size);

static const char* render_pass_format_index_debug_label(const char* base_label, i32 pass_idx, char* out_label, size_t out_label_size)
{
	snprintf(out_label, out_label_size, "%s: Pass %d", base_label ? base_label : "(unnamed)", pass_idx);
	return out_label;
}

static const char* render_pass_format_face_debug_label(const char* base_label, i32 pass_idx, char* out_label, size_t out_label_size)
{
	snprintf(out_label, out_label_size, "%s: Face %d", base_label ? base_label : "(unnamed)", pass_idx);
	return out_label;
}

static const char* render_pass_format_cascade_debug_label(const char* base_label, i32 pass_idx, char* out_label, size_t out_label_size)
{
	snprintf(out_label, out_label_size, "%s: Cascade %d", base_label ? base_label : "(unnamed)", pass_idx);
	return out_label;
}

static void render_pass_format_attachment_writes(const sg_attachments& in_attachments, bool in_swapchain, char* out_writes, size_t in_writes_size)
{
	if (!out_writes || in_writes_size == 0)
	{
		return;
	}

	out_writes[0] = '\0';
	if (in_swapchain)
	{
		snprintf(out_writes, in_writes_size, "%s", "Swapchain");
		return;
	}

	for (i32 color_index = 0; color_index < SG_MAX_COLOR_ATTACHMENTS; ++color_index)
	{
		const sg_view view = in_attachments.colors[color_index];
		if (view.id == SG_INVALID_ID)
		{
			continue;
		}

		const char* view_name = gpu_profiler_lookup_view_name(view);
		char fallback_name[64] = {};
		if (!view_name)
		{
			snprintf(fallback_name, sizeof(fallback_name), "color attachment:%u", view.id);
			view_name = fallback_name;
		}
		gpu_profiler_append_dependency_name(out_writes, in_writes_size, view_name);
	}

	if (in_attachments.depth_stencil.id != SG_INVALID_ID)
	{
		const char* view_name = gpu_profiler_lookup_view_name(in_attachments.depth_stencil);
		char fallback_name[64] = {};
		if (!view_name)
		{
			snprintf(fallback_name, sizeof(fallback_name), "depth attachment:%u", in_attachments.depth_stencil.id);
			view_name = fallback_name;
		}
		gpu_profiler_append_dependency_name(out_writes, in_writes_size, view_name);
	}
}

struct RenderPassOutputDesc {
	sg_pixel_format pixel_format	= SG_PIXELFORMAT_NONE;
	sg_load_action load_action		= SG_LOADACTION_DONTCARE;
	sg_store_action store_action	= SG_STOREACTION_STORE;
	sg_color clear_value			= {0.0f, 0.0f, 0.0f, 0.0f };
};

enum class ERenderPassType
{
	Single,
	Multi,
	Array,
	Cubemap,
	Swapchain,
};

struct RenderPassDesc {
	i32 initial_width = -1;
	i32 initial_height = -1;
	i32 pass_count = 1;

	optional<sg_pipeline_desc> pipeline_desc;
	int num_outputs = 0;
	RenderPassOutputDesc outputs[SG_MAX_COLOR_ATTACHMENTS];
	RenderPassOutputDesc depth_output;
	int num_scratch_outputs = 0;
	RenderPassOutputDesc scratch_outputs[SG_MAX_COLOR_ATTACHMENTS];

	f32 width_scale = 1.0f;
	f32 height_scale = 1.0f;
	bool resize_with_window = true;
	ERenderPassType type = ERenderPassType::Single;
	const char* debug_label = nullptr;
	const char* scratch_debug_label = nullptr;
	RenderPassDebugLabelFormatter debug_label_formatter = nullptr;
	RenderPassDebugLabelFormatter scratch_debug_label_formatter = nullptr;
};

struct RenderPassOutput {
	RenderPassOutput() = default;
	RenderPassOutput(const RenderPassOutput&) = delete;
	RenderPassOutput& operator=(const RenderPassOutput&) = delete;
	RenderPassOutput(RenderPassOutput&&) noexcept = default;
	RenderPassOutput& operator=(RenderPassOutput&&) noexcept = default;

	/*
			single 2D image for Single
			multiple 2D images for Multi
			single 2D array image for Array
			single Cubemap image for Cubemap
	*/
	StretchyBuffer<GpuImage> images;

	void cleanup()
	{
		for (GpuImage& image : images)
		{
			image.cleanup();
		}
		images.reset();
	}
};

struct RenderPassTopology
{
	const RenderPassDesc& desc;
	i32 pass_count_override = -1;

	i32 get_natural_pass_count() const
	{
		switch (desc.type)
		{
			case ERenderPassType::Single:		return 1;
			case ERenderPassType::Multi:		return desc.pass_count;
			case ERenderPassType::Array:		return desc.pass_count;
			case ERenderPassType::Cubemap:		return NUM_CUBE_FACES;
			case ERenderPassType::Swapchain:	return 1;
			default:
				printf("invalid render pass type: %i\n", desc.type);
				assert(false);
				return 1;
		}
	}

	i32 get_pass_count() const
	{
		const i32 natural_pass_count = get_natural_pass_count();
		if (pass_count_override > 0)
		{
			return std::min(pass_count_override, natural_pass_count);
		}
		return natural_pass_count;
	}

	i32 get_attachment_image_count() const
	{
		switch (desc.type)
		{
			case ERenderPassType::Single:		return 1;
			case ERenderPassType::Multi:		return desc.pass_count;
			case ERenderPassType::Array:		return 1;
			case ERenderPassType::Cubemap:		return 1;
			case ERenderPassType::Swapchain:	return 1;
			default:
				printf("invalid render pass type: %i\n", desc.type);
				assert(false);
				return 1;
		}
	}

	sg_image_type get_image_type() const
	{
		switch (desc.type)
		{
			case ERenderPassType::Single:
			case ERenderPassType::Multi:
				return SG_IMAGETYPE_2D;
			case ERenderPassType::Array:
				return SG_IMAGETYPE_ARRAY;
			case ERenderPassType::Cubemap:
				return SG_IMAGETYPE_CUBE;
			default:
				assert(false);
				return SG_IMAGETYPE_2D;
		}
	}

	i32 get_slice_count() const
	{
		const sg_image_type image_type = get_image_type();
		if (image_type == SG_IMAGETYPE_CUBE)
		{
			return NUM_CUBE_FACES;
		}
		if (image_type == SG_IMAGETYPE_ARRAY)
		{
			return desc.pass_count;
		}
		return 1;
	}

	i32 get_image_index_for_pass(i32 pass_idx) const
	{
		return desc.type == ERenderPassType::Multi ? pass_idx : 0;
	}

	i32 get_slice_index_for_pass(i32 pass_idx) const
	{
		return (desc.type == ERenderPassType::Array || desc.type == ERenderPassType::Cubemap) ? pass_idx : 0;
	}
};

struct RenderPassExecutionContext
{
	i32 pass_idx = 0;
	i32 image_idx = 0;
	i32 slice_idx = 0;
	bool is_scratch = false;
};

struct RenderPass {
public: // Variables
	sg_pipeline pipeline = {};

	StretchyBuffer<RenderPassOutput> color_outputs;

	optional<RenderPassOutput> depth_output;

	StretchyBuffer<sg_attachments> attachments;
	StretchyBuffer<RenderPassOutput> scratch_outputs;

	RenderPassDesc desc = {};

	i32 current_width = -1;
	i32 current_height = -1;
	i32 pass_count_override = -1;

public: // Functions
	RenderPass() = default;
	RenderPass(const RenderPass&) = delete;
	RenderPass& operator=(const RenderPass&) = delete;
	RenderPass(RenderPass&&) noexcept = default;
	RenderPass& operator=(RenderPass&&) noexcept = default;

	void init(const RenderPassDesc& in_desc)
	{	
		cleanup();

		desc = in_desc;
		validate_desc();
		if (desc.pipeline_desc)
		{
			const sg_pipeline_desc& pipeline_desc = desc.pipeline_desc.value();
			pipeline = sg_make_pipeline(pipeline_desc);
		}

		if (desc.initial_width > 0 && desc.initial_height > 0)
		{
			handle_resize(desc.initial_width, desc.initial_height);
		}
	}

	void cleanup()
	{
		release_targets();
		if (pipeline.id != SG_INVALID_ID)
		{
			sg_destroy_pipeline(pipeline);
			pipeline = {};
		}
		desc = {};
		current_width = -1;
		current_height = -1;
		pass_count_override = -1;
	}

	RenderPassTopology get_topology() const
	{
		return RenderPassTopology {
			.desc = desc,
			.pass_count_override = pass_count_override,
		};
	}

	// number of times the execute lambda is invoked on execute
	const i32 get_pass_count() const
	{	
		return get_topology().get_pass_count();
	}

	// number of images we create per-color-attachment
	i32 get_attachment_image_count() const
	{
		return get_topology().get_attachment_image_count();
	}

	sg_image_type determine_image_type() const
	{
		return get_topology().get_image_type();
	}

	i32 get_num_color_outputs() const
	{
		return color_outputs.length();
	}

	void set_pass_count_override(i32 in_pass_count_override)
	{
		pass_count_override = in_pass_count_override;
	}

	const char* get_debug_label_for_pass(i32 pass_idx, bool in_scratch_pass, char* out_label, size_t out_label_size) const
	{
		const char* base_label = in_scratch_pass
			? (desc.scratch_debug_label ? desc.scratch_debug_label : desc.debug_label)
			: desc.debug_label;
		base_label = base_label ? base_label : "(unnamed)";

		RenderPassDebugLabelFormatter formatter = in_scratch_pass
			? (desc.scratch_debug_label_formatter ? desc.scratch_debug_label_formatter : desc.debug_label_formatter)
			: desc.debug_label_formatter;
		if (!formatter)
		{
			if (get_pass_count() <= 1)
			{
				return base_label;
			}

			formatter = desc.type == ERenderPassType::Cubemap
				? render_pass_format_face_debug_label
				: render_pass_format_index_debug_label;
		}

		const char* formatted_label = formatter(base_label, pass_idx, out_label, out_label_size);
		return formatted_label ? formatted_label : base_label;
	}

	GpuImage& get_color_output(i32 color_output_idx, i32 pass_idx = 0)
	{
		assert(color_outputs.is_valid_index(color_output_idx));
		assert(color_outputs[color_output_idx].images.is_valid_index(pass_idx));
		return color_outputs[color_output_idx].images[pass_idx];
	}

	GpuImage& get_depth_output(i32 pass_idx = 0)
	{
		assert(depth_output.has_value());
		assert(depth_output.value().images.is_valid_index(pass_idx));
		return depth_output.value().images[pass_idx];
	}

	GpuImage& get_scratch_color_output(i32 scratch_output_idx, i32 pass_idx = 0)
	{
		assert(scratch_outputs.is_valid_index(scratch_output_idx));
		assert(scratch_outputs[scratch_output_idx].images.is_valid_index(pass_idx));
		return scratch_outputs[scratch_output_idx].images[pass_idx];
	}

	void validate_desc() const
	{
		assert_msgf(
			desc.num_outputs > 0 || desc.depth_output.pixel_format != SG_PIXELFORMAT_NONE || desc.type == ERenderPassType::Swapchain,
			"RenderPass::init(): render pass must have a color output, depth output, or be a swapchain pass"
		);
		assert_msgf(desc.pass_count > 0, "RenderPass::init(): pass_count must be greater than zero");
		assert_msgf(desc.num_outputs >= 0 && desc.num_outputs <= SG_MAX_COLOR_ATTACHMENTS, "RenderPass::init(): invalid color output count");
		assert_msgf(desc.num_scratch_outputs >= 0 && desc.num_scratch_outputs <= SG_MAX_COLOR_ATTACHMENTS, "RenderPass::init(): invalid scratch output count");
		assert_msgf(desc.width_scale > 0.0f && desc.height_scale > 0.0f, "RenderPass::init(): render pass scales must be positive");

		for (i32 output_idx = 0; output_idx < desc.num_outputs; ++output_idx)
		{
			assert_msgf(
				desc.outputs[output_idx].pixel_format != SG_PIXELFORMAT_NONE,
				"RenderPass::init(): color outputs must declare a pixel format"
			);
		}

		if (desc.num_scratch_outputs > 0)
		{
			assert_msgf(
				desc.type == ERenderPassType::Single || desc.type == ERenderPassType::Array,
				"RenderPass::init(): scratch outputs currently support Single and Array render passes"
			);
			for (i32 scratch_output_idx = 0; scratch_output_idx < desc.num_scratch_outputs; ++scratch_output_idx)
			{
				assert_msgf(
					desc.scratch_outputs[scratch_output_idx].pixel_format != SG_PIXELFORMAT_NONE,
					"RenderPass::init(): scratch outputs must declare a pixel format"
				);
			}
		}

		if (desc.pipeline_desc)
		{
			const sg_pipeline_desc& pipeline_desc = desc.pipeline_desc.value();
			if (desc.type != ERenderPassType::Swapchain)
			{
				assert_msgf(
					pipeline_desc.color_count == desc.num_outputs,
					"RenderPass::init(): pipeline_desc.color_count must match num_outputs"
				);
				for (i32 output_idx = 0; output_idx < desc.num_outputs; ++output_idx)
				{
					assert_msgf(
						pipeline_desc.colors[output_idx].pixel_format == desc.outputs[output_idx].pixel_format,
						"RenderPass::init(): pipeline color pixel formats must match output pixel formats"
					);
				}
			}

			if (pipeline_desc.depth.pixel_format != SG_PIXELFORMAT_NONE)
			{
				assert_msgf(
					pipeline_desc.depth.pixel_format == desc.depth_output.pixel_format,
					"RenderPass::init(): pipeline_desc.depth.pixel_format must match depth_output.pixel_format"
				);
			}
		}
	}

	void release_targets()
	{
		for (RenderPassOutput& color_output : color_outputs)
		{
			color_output.cleanup();
		}
		color_outputs.reset();

		if (depth_output.has_value())
		{
			depth_output.value().cleanup();
			depth_output.reset();
		}

		for (RenderPassOutput& scratch_output : scratch_outputs)
		{
			scratch_output.cleanup();
		}
		scratch_outputs.reset();
		attachments.reset();
	}

	void allocate_outputs()
	{
		const RenderPassTopology topology = get_topology();
		const sg_image_type image_type = topology.get_image_type();
		const i32 num_slices = topology.get_slice_count();
		const i32 attachment_image_count = topology.get_attachment_image_count();

		for (int output_idx = 0; output_idx < desc.num_outputs; ++output_idx)
		{
			const RenderPassOutputDesc& output_desc = desc.outputs[output_idx];
			RenderPassOutput& new_color_output = color_outputs.emplace();

			GpuImageDesc image_desc = {
				.type = image_type,
				.usage = {
					.color_attachment = true,
				},
				.width = current_width,
				.height = current_height,
				.num_slices = num_slices,
				.pixel_format = output_desc.pixel_format,
				.label = "color_image",
			};

			for (i32 image_idx = 0; image_idx < attachment_image_count; ++image_idx)
			{
				new_color_output.images.emplace(image_desc);
			}
		}

		if (desc.depth_output.pixel_format != SG_PIXELFORMAT_NONE)
		{
			RenderPassOutput& new_depth_output = depth_output.emplace();
			GpuImageDesc depth_image_desc = {
				.type = image_type,
				.usage = {
					.depth_stencil_attachment = true,
				},
				.width = current_width,
				.height = current_height,
				.num_slices = num_slices,
				.pixel_format = desc.depth_output.pixel_format,
				.label = "depth-image"
			};

			for (i32 image_idx = 0; image_idx < attachment_image_count; ++image_idx)
			{
				new_depth_output.images.emplace(depth_image_desc);
			}
		}

		for (int scratch_output_idx = 0; scratch_output_idx < desc.num_scratch_outputs; ++scratch_output_idx)
		{
			const RenderPassOutputDesc& output_desc = desc.scratch_outputs[scratch_output_idx];
			RenderPassOutput& new_scratch_output = scratch_outputs.emplace();
			GpuImageDesc image_desc = {
				.type = image_type,
				.usage = {
					.color_attachment = true,
				},
				.width = current_width,
				.height = current_height,
				.num_slices = num_slices,
				.pixel_format = output_desc.pixel_format,
				.label = "scratch-color-image",
			};

			for (i32 image_idx = 0; image_idx < attachment_image_count; ++image_idx)
			{
				new_scratch_output.images.emplace(image_desc);
			}
		}
	}

	void build_attachments()
	{
		const RenderPassTopology topology = get_topology();
		const i32 pass_count = topology.get_pass_count();
		for (i32 pass_idx = 0; pass_idx < pass_count; ++pass_idx)
		{
			attachments.emplace();
		}

		for (i32 pass_idx = 0; pass_idx < pass_count; ++pass_idx)
		{
			const i32 image_idx = topology.get_image_index_for_pass(pass_idx);
			const i32 slice_idx = topology.get_slice_index_for_pass(pass_idx);
			for (int output_idx = 0; output_idx < desc.num_outputs; ++output_idx)
			{
				attachments[pass_idx].colors[output_idx] = get_color_output(output_idx, image_idx).get_attachment_view(slice_idx);
			}

			if (depth_output.has_value())
			{
				attachments[pass_idx].depth_stencil = get_depth_output(image_idx).get_attachment_view(slice_idx);
			}
		}
	}

	void handle_resize(i32 in_new_width, i32 in_new_height)
	{
		if (!desc.resize_with_window)
		{
			if (current_width > 0 && current_height > 0)
			{
				return;
			}

			assert(desc.initial_width > 0 && desc.initial_height > 0);
			in_new_width = desc.initial_width;
			in_new_height = desc.initial_height;
		}

		current_width = std::max(1, (i32)((f32)in_new_width * desc.width_scale + 0.5f));
		current_height = std::max(1, (i32)((f32)in_new_height * desc.height_scale + 0.5f));

		// Create render target if we aren't rendering directly to swapchain
		if (desc.type != ERenderPassType::Swapchain)
		{
			release_targets();
			allocate_outputs();
			build_attachments();
		}
	}

	RenderPassExecutionContext make_execution_context(i32 in_pass_idx, bool in_scratch_pass) const
	{
		const RenderPassTopology topology = get_topology();
		return RenderPassExecutionContext {
			.pass_idx = in_pass_idx,
			.image_idx = topology.get_image_index_for_pass(in_pass_idx),
			.slice_idx = topology.get_slice_index_for_pass(in_pass_idx),
			.is_scratch = in_scratch_pass,
		};
	}

	const char* get_debug_label_for_context(const RenderPassExecutionContext& in_context, char* out_label, size_t out_label_size) const
	{
		return get_debug_label_for_pass(
			in_context.pass_idx,
			in_context.is_scratch,
			out_label,
			out_label_size
		);
	}

	void apply_pass_actions(sg_pass& in_pass) const
	{
		for (int i = 0; i < desc.num_outputs; ++i)
		{
			const RenderPassOutputDesc& output_desc = desc.outputs[i];

			in_pass.action.colors[i] = {
				.load_action = output_desc.load_action,
				.store_action = output_desc.store_action,
				.clear_value = output_desc.clear_value,
			};
		}

		if (depth_output.has_value())
		{
			const RenderPassOutputDesc& output_desc = desc.depth_output;

			in_pass.action.depth = {
				.load_action = output_desc.load_action,
				.store_action = output_desc.store_action,
				.clear_value = output_desc.clear_value.r,
			};
		}
	}

	void apply_scratch_pass_actions(sg_pass& in_pass, i32 scratch_output_idx) const
	{
		in_pass.action.colors[0] = {
			.load_action = desc.scratch_outputs[scratch_output_idx].load_action,
			.store_action = desc.scratch_outputs[scratch_output_idx].store_action,
			.clear_value = desc.scratch_outputs[scratch_output_idx].clear_value,
		};
	}

	void execute_pass(
		const RenderPassExecutionContext& in_context,
		sg_pass& in_pass,
		bool in_render_to_swapchain,
		std::function<void(const RenderPassExecutionContext& context)> in_callback
	)
	{
		char pass_debug_label_buffer[CPU_TIMINGS_MAX_NAME_LENGTH] = {};
		const char* pass_debug_label = get_debug_label_for_context(
			in_context,
			pass_debug_label_buffer,
			sizeof(pass_debug_label_buffer)
		);

		in_pass.label = pass_debug_label;

		sg_begin_pass(in_pass);

		{
			CPU_TIMING_SCOPE(pass_debug_label);
			char writes[GPU_TIMINGS_MAX_DEPENDENCY_TEXT_LENGTH] = {};
			render_pass_format_attachment_writes(in_pass.attachments, in_render_to_swapchain, writes, sizeof(writes));
			gpu_frame_timings_set_next_scope_writes(writes);
			GpuDebugScope debug_scope(pass_debug_label);

			if (pipeline.id != SG_INVALID_ID)
			{
				sg_apply_pipeline(pipeline);
			}

			in_callback(in_context);
		}

		{
			CPU_TIMING_BACKEND_SCOPE("sg_end_pass", pass_debug_label);
			sg_end_pass();
		}
	}

	void execute_one(i32 in_pass_idx, std::function<void(const RenderPassExecutionContext& context)> in_callback)
	{
		assert(current_width > 0 && current_height > 0);

		const bool render_to_swapchain = desc.type == ERenderPassType::Swapchain;
		const i32 pass_count = get_pass_count();
		assert(in_pass_idx >= 0 && in_pass_idx < pass_count);

		const RenderPassExecutionContext context = make_execution_context(in_pass_idx, false);
		sg_pass pass = {
			.attachments = !render_to_swapchain ? attachments[in_pass_idx] : (sg_attachments){},
			.swapchain = render_to_swapchain ? sglue_swapchain() : (sg_swapchain){},
		};
		apply_pass_actions(pass);
		execute_pass(context, pass, render_to_swapchain, in_callback);
	}

	void execute_one(i32 in_pass_idx, std::function<void(const i32 pass_idx)> in_callback)
	{
		execute_one(
			in_pass_idx,
			[&](const RenderPassExecutionContext& context)
			{
				in_callback(context.pass_idx);
			}
		);
	}

	void execute(std::function<void(const RenderPassExecutionContext& context)> in_callback)
	{
		const i32 pass_count = get_pass_count();
		for (i32 pass_idx = 0; pass_idx < pass_count; ++pass_idx)
		{
			execute_one(pass_idx, in_callback);
		}
	}

	// pass_idx arg on in_callback is used for layered render passes
	void execute(std::function<void(const i32 pass_idx)> in_callback)
	{
		execute(
			[&](const RenderPassExecutionContext& context)
			{
				in_callback(context.pass_idx);
			}
		);
	}

	void execute_scratch(i32 scratch_output_idx, std::function<void(const RenderPassExecutionContext& context)> in_callback)
	{
		assert(current_width > 0 && current_height > 0);
		assert(scratch_outputs.is_valid_index(scratch_output_idx));

		const i32 pass_count = get_pass_count();
		for (i32 pass_idx = 0; pass_idx < pass_count; ++pass_idx)
		{
			const RenderPassExecutionContext context = make_execution_context(pass_idx, true);
			sg_attachments scratch_attachment = {};
			scratch_attachment.colors[0] = get_scratch_color_output(scratch_output_idx, context.image_idx).get_attachment_view(context.slice_idx);

			sg_pass pass = {
				.attachments = scratch_attachment,
			};
			apply_scratch_pass_actions(pass, scratch_output_idx);
			execute_pass(context, pass, false, in_callback);
		}
	}

	void execute_scratch(i32 scratch_output_idx, std::function<void(const i32 pass_idx)> in_callback)
	{
		execute_scratch(
			scratch_output_idx,
			[&](const RenderPassExecutionContext& context)
			{
				in_callback(context.pass_idx);
			}
		);
	}
};
