#pragma once

#include <optional>

#include "wire_overlay.compiled.h"

#include "render/render_pass.h"

using std::optional;

namespace WireOverlayPass
{
	optional<sg_shader> copy_shader;
	optional<sg_shader> mesh_overlay_shader;
	optional<sg_pipeline> mesh_overlay_pipeline;

	RenderPassDesc make_render_pass_desc(sg_pixel_format color_format)
	{
		if (!copy_shader.has_value())
		{
			copy_shader = sg_make_shader(wire_overlay_copy_shader_desc(sg_query_backend()));
		}

		return (RenderPassDesc) {
			.pipeline_desc = (sg_pipeline_desc) {
				.shader = copy_shader.value(),
				.depth = {
					.pixel_format = SG_PIXELFORMAT_NONE,
				},
				.color_count = 1,
				.colors[0] = {
					.pixel_format = color_format,
				},
				.cull_mode = SG_CULLMODE_NONE,
				.label = "wire-overlay-copy-pipeline",
			},
			.num_outputs = 1,
			.outputs[0] = {
				.pixel_format = color_format,
				.load_action = SG_LOADACTION_DONTCARE,
				.store_action = SG_STOREACTION_STORE,
			},
			.debug_label = "Wire Overlay",
		};
	}

	sg_pipeline get_mesh_overlay_pipeline(sg_pixel_format color_format)
	{
		if (!mesh_overlay_shader.has_value())
		{
			mesh_overlay_shader = sg_make_shader(wire_overlay_mesh_overlay_shader_desc(sg_query_backend()));
		}

		if (!mesh_overlay_pipeline.has_value())
		{
			mesh_overlay_pipeline = sg_make_pipeline((sg_pipeline_desc) {
				.shader = mesh_overlay_shader.value(),
				.depth = {
					.pixel_format = SG_PIXELFORMAT_NONE,
				},
				.color_count = 1,
				.colors[0] = {
					.pixel_format = color_format,
					.blend = {
						.enabled = true,
						.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
						.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
						.op_rgb = SG_BLENDOP_ADD,
						.src_factor_alpha = SG_BLENDFACTOR_ONE,
						.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
						.op_alpha = SG_BLENDOP_ADD,
					},
				},
				.primitive_type = SG_PRIMITIVETYPE_TRIANGLES,
				.cull_mode = SG_CULLMODE_NONE,
				.label = "wire-overlay-mesh-pipeline",
			});
		}

		return mesh_overlay_pipeline.value();
	}
}
