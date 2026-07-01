#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <optional>

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
	/*
			single 2D image for Single
			multiple 2D images for Multi
			single 2D array image for Array
			single Cubemap image for Cubemap
	*/
	StretchyBuffer<GpuImage> images;
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
	void init(const RenderPassDesc& in_desc)
	{	
		assert(in_desc.num_outputs > 0 || in_desc.depth_output.pixel_format != SG_PIXELFORMAT_NONE || in_desc.type == ERenderPassType::Swapchain);

		desc = in_desc;
		if (desc.pipeline_desc)
		{
			const sg_pipeline_desc& pipeline_desc = desc.pipeline_desc.value();
			pipeline = sg_make_pipeline(pipeline_desc);

			// Validate depth output matches our depth_output desc
			if (pipeline_desc.depth.pixel_format != SG_PIXELFORMAT_NONE)
			{
				assert_msgf(
					pipeline_desc.depth.pixel_format == desc.depth_output.pixel_format,
					"RenderPass::init(): pipeline_desc.depth.pixel_format must match depth_output.pixel_format"
				);
			}
		}

		if (desc.initial_width > 0 && desc.initial_height > 0)
		{
			handle_resize(desc.initial_width, desc.initial_height);
		}
	}

	// number of times the execute lambda is invoked on execute
	const i32 get_pass_count() const
	{	
		if (pass_count_override > 0)
		{
			return std::min(pass_count_override, desc.pass_count);
		}

		switch (desc.type)
		{
			case ERenderPassType::Single:		return 1;
			case ERenderPassType::Multi:		return desc.pass_count;
			case ERenderPassType::Array:		return desc.pass_count;
			case ERenderPassType::Cubemap:		return NUM_CUBE_FACES;
			case ERenderPassType::Swapchain:	return 1;
			default:
				printf("invalid type: %i\n", desc.type);
				assert(false);
		}
	}

	// number of images we create per-color-attachment
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
				printf("invalid type: %i\n", desc.type);
				assert(false);
		}
	}

	sg_image_type determine_image_type() const
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
		}
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
			const sg_image_type image_type = determine_image_type();
			const i32 num_slices = (image_type == SG_IMAGETYPE_CUBE) ? NUM_CUBE_FACES
				: (image_type == SG_IMAGETYPE_ARRAY) ? desc.pass_count
				: 1;

			const i32 pass_count = get_pass_count();
			const i32 attachment_image_count = get_attachment_image_count();

			attachments.reset();
			for (i32 i = 0; i < pass_count; ++i)
			{
				sg_attachments new_attachments = {};	
				attachments.add(new_attachments);
			}

			// Clean up old output data
			for (RenderPassOutput& color_output : color_outputs)
			{	
				for (GpuImage& existing_image : color_output.images)
				{
					existing_image.cleanup();
				}
				color_output.images.reset();
			}
			color_outputs.reset();

			if (depth_output.has_value())
			{
				for (GpuImage& existing_depth_image : depth_output.value().images)
				{
					existing_depth_image.cleanup();
				}
				depth_output.reset();
			}

			for (RenderPassOutput& scratch_output : scratch_outputs)
			{
				for (GpuImage& existing_scratch_image : scratch_output.images)
				{
					existing_scratch_image.cleanup();
				}
				scratch_output.images.reset();
			}
			scratch_outputs.reset();

			for (int output_idx = 0; output_idx < desc.num_outputs; ++output_idx)
			{
				const RenderPassOutputDesc& output_desc = desc.outputs[output_idx];

				RenderPassOutput new_color_output;

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
					new_color_output.images.add(GpuImage(image_desc));
				}

				color_outputs.add(new_color_output);

				switch (desc.type)
				{
					case ERenderPassType::Single:
					{
						// Only one attachment for single render passes
						attachments[0].colors[output_idx] = get_color_output(output_idx,0).get_attachment_view(0);
						break;
					}
					case ERenderPassType::Multi:
					{
						// One image per attachment for multi render passes
						for (i32 i = 0; i < attachment_image_count; ++i)
						{
							attachments[i].colors[output_idx] = get_color_output(output_idx,i).get_attachment_view(0);
						}
						break;
					}
					case ERenderPassType::Array:
					{
						// One array slice per attachment for array render passes
						for (i32 i = 0; i < pass_count; ++i)
						{
							attachments[i].colors[output_idx] = get_color_output(output_idx,0).get_attachment_view(i);
						}
						break;
					}
					case ERenderPassType::Cubemap:
					{
						// One cube-face/slice per attachment for Cubemap render passes
						for (i32 i = 0; i < pass_count; ++i)
						{
							attachments[i].colors[output_idx] = get_color_output(output_idx,0).get_attachment_view(i);
						}
						break;
					}
					default: break;
				}
			}

			if (desc.depth_output.pixel_format != SG_PIXELFORMAT_NONE)
			{
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

				RenderPassOutput new_depth_output;
				for (i32 image_idx = 0; image_idx < attachment_image_count; ++image_idx)
				{	
					new_depth_output.images.add(GpuImage(depth_image_desc));
				}

				switch (desc.type)
				{
					case ERenderPassType::Single:
					{
						// Only one attachment for single render passes
						attachments[0].depth_stencil = new_depth_output.images[0].get_attachment_view(0);
						break;
					}
					case ERenderPassType::Multi:
					{
						// One image per attachment for multi render passes
						for (i32 i = 0; i < attachment_image_count; ++i)
						{
							attachments[i].depth_stencil = new_depth_output.images[i].get_attachment_view(0);
						}
						break;
					}
					case ERenderPassType::Array:
					{
						// One array slice per attachment for array render passes
						for (i32 i = 0; i < pass_count; ++i)
						{
							attachments[i].depth_stencil = new_depth_output.images[0].get_attachment_view(i);
						}
						break;
					}
					case ERenderPassType::Cubemap:
					{
						// One cube-face/slice per attachment for Cubemap render passes
						for (i32 i = 0; i < pass_count; ++i)
						{
							attachments[i].depth_stencil = new_depth_output.images[0].get_attachment_view(i);
						}
						break;
					}
					default: break;
				}

				depth_output = new_depth_output;
			}

			if (desc.num_scratch_outputs > 0)
			{
				assert(desc.type == ERenderPassType::Single || desc.type == ERenderPassType::Array);
				assert(desc.num_scratch_outputs <= SG_MAX_COLOR_ATTACHMENTS);

				for (int scratch_output_idx = 0; scratch_output_idx < desc.num_scratch_outputs; ++scratch_output_idx)
				{
					const RenderPassOutputDesc& output_desc = desc.scratch_outputs[scratch_output_idx];
					assert(output_desc.pixel_format != SG_PIXELFORMAT_NONE);

					RenderPassOutput new_scratch_output;
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
						new_scratch_output.images.add(GpuImage(image_desc));
					}

					scratch_outputs.add(new_scratch_output);
				}

			}
		}
	}

	void execute_one(i32 in_pass_idx, std::function<void(const i32 pass_idx)> in_callback)
	{
		assert(current_width > 0 && current_height > 0);

		const bool render_to_swapchain = desc.type == ERenderPassType::Swapchain;
		const i32 pass_count = get_pass_count();
		assert(in_pass_idx >= 0 && in_pass_idx < pass_count);

		char pass_debug_label_buffer[CPU_TIMINGS_MAX_NAME_LENGTH] = {};
		const char* pass_debug_label = get_debug_label_for_pass(
			in_pass_idx,
			false,
			pass_debug_label_buffer,
			sizeof(pass_debug_label_buffer)
		);
		sg_pass pass = {
			.attachments = !render_to_swapchain ? attachments[in_pass_idx] : (sg_attachments){},
			.swapchain = render_to_swapchain ? sglue_swapchain() : (sg_swapchain){},
			.label = pass_debug_label,
		};

		for (int i = 0; i < desc.num_outputs; ++i)
		{
			const RenderPassOutputDesc& output_desc = desc.outputs[i];

			pass.action.colors[i] = {
				.load_action = output_desc.load_action,
				.store_action = output_desc.store_action,
				.clear_value = output_desc.clear_value,
			};
		}

		if (depth_output.has_value())
		{
			const RenderPassOutputDesc& output_desc = desc.depth_output;

			pass.action.depth = {
				.load_action = output_desc.load_action,
				.store_action = output_desc.store_action,
				.clear_value = output_desc.clear_value.r,
			};
		}

		sg_begin_pass(pass);

		{
			CPU_TIMING_SCOPE(pass_debug_label);
			char writes[GPU_TIMINGS_MAX_DEPENDENCY_TEXT_LENGTH] = {};
			render_pass_format_attachment_writes(pass.attachments, render_to_swapchain, writes, sizeof(writes));
			gpu_frame_timings_set_next_scope_writes(writes);
			GpuDebugScope debug_scope(pass_debug_label);

			if (pipeline.id != SG_INVALID_ID)
			{
				sg_apply_pipeline(pipeline);
			}

			in_callback(in_pass_idx);
		}

		{
			CPU_TIMING_BACKEND_SCOPE("sg_end_pass", pass_debug_label);
			sg_end_pass();
		}
	}

	// pass_idx arg on in_callback is used for cubemap render passes
	void execute(std::function<void(const i32 pass_idx)> in_callback)
	{
		const i32 pass_count = get_pass_count();
		for (i32 pass_idx = 0; pass_idx < pass_count; ++pass_idx)
		{
			execute_one(pass_idx, in_callback);
		}
	}

	void execute_scratch(i32 scratch_output_idx, std::function<void(const i32 pass_idx)> in_callback)
	{
		assert(current_width > 0 && current_height > 0);
		assert(scratch_outputs.is_valid_index(scratch_output_idx));

		const i32 pass_count = get_pass_count();
		for (i32 pass_idx = 0; pass_idx < pass_count; ++pass_idx)
		{
			char pass_debug_label_buffer[CPU_TIMINGS_MAX_NAME_LENGTH] = {};
			const char* pass_debug_label = get_debug_label_for_pass(
				pass_idx,
				true,
				pass_debug_label_buffer,
				sizeof(pass_debug_label_buffer)
			);

			sg_attachments scratch_attachment = {};
			if (desc.type == ERenderPassType::Array)
			{
				scratch_attachment.colors[0] = get_scratch_color_output(scratch_output_idx, 0).get_attachment_view(pass_idx);
			}
			else
			{
				scratch_attachment.colors[0] = get_scratch_color_output(scratch_output_idx, pass_idx).get_attachment_view(0);
			}

			sg_pass pass = {
				.attachments = scratch_attachment,
				.label = pass_debug_label,
			};
			pass.action.colors[0] = {
				.load_action = desc.scratch_outputs[scratch_output_idx].load_action,
				.store_action = desc.scratch_outputs[scratch_output_idx].store_action,
				.clear_value = desc.scratch_outputs[scratch_output_idx].clear_value,
			};

			sg_begin_pass(pass);

			{
				CPU_TIMING_SCOPE(pass_debug_label);
				char writes[GPU_TIMINGS_MAX_DEPENDENCY_TEXT_LENGTH] = {};
				render_pass_format_attachment_writes(scratch_attachment, false, writes, sizeof(writes));
				gpu_frame_timings_set_next_scope_writes(writes);
				GpuDebugScope debug_scope(pass_debug_label);

				if (pipeline.id != SG_INVALID_ID)
				{
					sg_apply_pipeline(pipeline);
				}

				in_callback(pass_idx);
			}

			{
				CPU_TIMING_BACKEND_SCOPE("sg_end_pass", pass_debug_label);
				sg_end_pass();
			}
		}
	}
};
