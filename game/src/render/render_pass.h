#pragma once

#include <cassert>
#include <functional>
#include <optional>

#include "core/types.h"
#include "core/stretchy_buffer.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_glue.h"

using std::optional;

static constexpr i32 NUM_CUBE_FACES = 6;

struct RenderPassOutputDesc {
	sg_pixel_format pixel_format	= SG_PIXELFORMAT_NONE;
	sg_load_action load_action		= SG_LOADACTION_DONTCARE;
	sg_store_action store_action	= SG_STOREACTION_STORE;
	sg_color clear_value			= {0.0f, 0.0f, 0.0f, 0.0f };
};

enum class ERenderPassType
{
	Default,
	Cubemap,
	Swapchain,
};

struct RenderPassDesc {
	i32 initial_width = -1;
	i32 initial_height = -1;

	optional<sg_pipeline_desc> pipeline_desc;
	int num_outputs = 0;
	RenderPassOutputDesc outputs[SG_MAX_COLOR_ATTACHMENTS];
	RenderPassOutputDesc depth_output;

	ERenderPassType type = ERenderPassType::Default;
};

struct RenderPass {
public: // Variables
	sg_pipeline pipeline = {};

	sg_image color_outputs[SG_MAX_COLOR_ATTACHMENTS] = {};

	optional<sg_image> depth_image;

	StretchyBuffer<sg_attachments> attachments;

	RenderPassDesc desc = {};

	i32 current_width = -1;
	i32 current_height = -1;

public: // Functions
	void init(const RenderPassDesc& in_desc)
	{	
		assert(in_desc.num_outputs > 0 || in_desc.type == ERenderPassType::Swapchain);

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

	const i32 get_pass_count() const
	{	
		const bool is_cubemap = desc.type == ERenderPassType::Cubemap;
		const i32 pass_count = is_cubemap ? NUM_CUBE_FACES : 1;
		return pass_count;
	}

	void handle_resize(i32 in_new_width, i32 in_new_height)
	{
		current_width = in_new_width;
		current_height = in_new_height;

		// Create render target if we aren't rendering directly to swapchain
		if (desc.type != ERenderPassType::Swapchain)
		{
			const bool is_cubemap = desc.type == ERenderPassType::Cubemap;
			sg_image_type image_type	= is_cubemap
										? SG_IMAGETYPE_CUBE
										: SG_IMAGETYPE_2D;

			const i32 attachments_descs_count = get_pass_count();

			StretchyBuffer<sg_attachments_desc> attachments_descs;
			for (i32 i = 0; i < attachments_descs_count; ++i)
			{
				sg_attachments_desc new_desc = {};	
				attachments_descs.add(new_desc);
			}

			for (int output_idx = 0; output_idx < desc.num_outputs; ++output_idx)
			{
				const RenderPassOutputDesc& output_desc = desc.outputs[output_idx];
				sg_image& color_image = color_outputs[output_idx];	
				if (color_image.id != SG_INVALID_ID)
				{
					sg_destroy_image(color_image);
				}

				sg_image_desc color_image_desc = {
					.type = image_type,	
					.usage = {
						.render_attachment = true,
					},
					.width = current_width,
					.height = current_height,
					.pixel_format = output_desc.pixel_format,
					.sample_count = 1,
					.label = "color_image",
				};

				color_image = sg_make_image(&color_image_desc);

				for (i32 i = 0; i < attachments_descs_count; ++i)
				{
					attachments_descs[i].colors[output_idx] = {
						.image = color_image,
						.mip_level = 0,
						.slice = i,
					};
				}	
			}

			if (depth_image.has_value() && depth_image->id != SG_INVALID_ID)
			{
				sg_destroy_image(depth_image.value());
			}

			if (desc.depth_output.pixel_format != SG_PIXELFORMAT_NONE)
			{
				sg_image_desc depth_image_desc = {
					.type = image_type,	
					.usage = {
						.render_attachment = true,
					},
					.width = current_width,
					.height = current_height,
					.pixel_format = desc.depth_output.pixel_format,
					.sample_count = 1,
					.label = "depth-image"
				};

				depth_image = sg_make_image(&depth_image_desc);

				for (i32 i = 0; i < attachments_descs_count; ++i)
				{
					attachments_descs[i].depth_stencil = {
						.image = depth_image.value(),
						.mip_level = 0,
						.slice = i,
					};
				}
			}	

			// Clean up old attachments
			for (i32 i = 0; i < attachments.length(); ++i)
			{
				if (attachments[i].id != SG_INVALID_ID)
				{
					sg_destroy_attachments(attachments[i]);
				}
			}
			attachments.reset();

			// Setup new attachments
			for (i32 i = 0; i < attachments_descs_count; ++i)
			{
				attachments.add(sg_make_attachments(attachments_descs[i]));
			}
		}
	}

	void execute(std::function<void()> in_callback)
	{
		const bool render_to_swapchain = desc.type == ERenderPassType::Swapchain;

		const i32 pass_count = get_pass_count();
		for (i32 pass_idx = 0; pass_idx < pass_count; ++pass_idx)
		{
			sg_pass pass = {
				.attachments = !render_to_swapchain ? attachments[pass_idx] : (sg_attachments){},
				.swapchain = render_to_swapchain ? sglue_swapchain() : (sg_swapchain){},
			};

			for (int i = 0; i < SG_MAX_COLOR_ATTACHMENTS; ++i)
			{
				const RenderPassOutputDesc& output_desc = desc.outputs[i];

				pass.action.colors[i] = {
					.load_action = output_desc.load_action,
					.store_action = output_desc.store_action,
					.clear_value = output_desc.clear_value,
				};
			}

			sg_begin_pass(pass);

			if (pipeline.id != SG_INVALID_ID)
			{
				sg_apply_pipeline(pipeline);
			}

			in_callback();

			sg_end_pass();
		}
	}
};
