#pragma once

#include <optional>

#include "fxaa.compiled.h"

#include "render/render_pass.h"

using std::optional;

namespace FXAAPass
{
	optional<sg_shader> shader;
	const i32 num_pass_outputs = 1;

	sg_pipeline_desc make_pipeline_desc(sg_pixel_format color_format)
	{
		if (!shader.has_value())
		{
			shader = sg_make_shader(fxaa_fxaa_shader_desc(sg_query_backend()));
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
			.label = "fxaa-pipeline",
		};
	}

	RenderPassDesc make_render_pass_desc(sg_pixel_format color_format)
	{
		return (RenderPassDesc) {
			.pipeline_desc = FXAAPass::make_pipeline_desc(color_format),
			.num_outputs = num_pass_outputs,
			.outputs[0] = {
				.pixel_format = color_format,
				.load_action = SG_LOADACTION_DONTCARE,
				.store_action = SG_STOREACTION_STORE,
			},
			.debug_label = "FXAA",
		};
	}
}
