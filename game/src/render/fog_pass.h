#pragma once

#include <optional>

#include "fog.compiled.h"

#include "render/render_pass.h"

namespace FogPass
{
	optional<sg_shader> shader;

	const i32 num_pass_outputs = 1;

	sg_pipeline_desc make_pipeline_desc(sg_pixel_format color_format)
	{
		if (!shader.has_value())
		{
			shader = sg_make_shader(fog_fog_shader_desc(sg_query_backend()));
		}

		return (sg_pipeline_desc) {
			.shader = shader.value(),
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.color_count = num_pass_outputs,
			.colors[0] = {
				.pixel_format = color_format,
			},
			.cull_mode = SG_CULLMODE_NONE,
			.label = "fog-pipeline",
		};
	}

	RenderPassDesc make_render_pass_desc(sg_pixel_format color_format)
	{
		return (RenderPassDesc) {
			.pipeline_desc = FogPass::make_pipeline_desc(color_format),
			.num_outputs = num_pass_outputs,
			.outputs[0] = {
				.pixel_format = color_format,
				.load_action = SG_LOADACTION_DONTCARE,
				.store_action = SG_STOREACTION_STORE,
			},
			.debug_label = "Fog",
		};
	}
}
