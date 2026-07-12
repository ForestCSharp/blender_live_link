#pragma once

#include <optional>

#include "temporal_aa.compiled.h"

#include "render/render_pass.h"
#include "state/state.h"

using std::optional;

namespace TemporalAAPass
{
	optional<sg_shader> shader;
	const i32 num_pass_outputs = 2;
	const i32 history_pass_count = 2;

	void invalidate_history(State& in_state)
	{
		in_state.temporal_aa.history_valid = false;
	}

	HMM_Vec2 get_decima_jitter_pixels(i32 jitter_phase)
	{
		return (jitter_phase & 1) == 1
			? HMM_V2(0.5f, 0.0f)
			: HMM_V2(0.0f, 0.5f);
	}

	HMM_Mat4 apply_projection_jitter(HMM_Mat4 in_projection, HMM_Vec2 jitter_pixels, HMM_Vec2 screen_size)
	{
		if (screen_size.X <= 0.0f || screen_size.Y <= 0.0f)
		{
			return in_projection;
		}

		const HMM_Vec2 jitter_ndc = HMM_V2(
			(2.0f * jitter_pixels.X) / screen_size.X,
			(-2.0f * jitter_pixels.Y) / screen_size.Y
		);
		for (i32 column = 0; column < 4; ++column)
		{
			in_projection.Elements[column][0] += jitter_ndc.X * in_projection.Elements[column][3];
			in_projection.Elements[column][1] += jitter_ndc.Y * in_projection.Elements[column][3];
		}
		return in_projection;
	}

	sg_pipeline_desc make_pipeline_desc(sg_pixel_format color_format)
	{
		if (!shader.has_value())
		{
			shader = sg_make_shader(temporal_aa_temporal_aa_shader_desc(sg_query_backend()));
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
			.colors[1] = {
				.pixel_format = color_format,
			},
			.cull_mode = SG_CULLMODE_NONE,
			.label = "temporal-aa-pipeline",
		};
	}

	RenderPassDesc make_render_pass_desc(sg_pixel_format color_format)
	{
		return (RenderPassDesc) {
			.pass_count = history_pass_count,
			.pipeline_desc = TemporalAAPass::make_pipeline_desc(color_format),
			.num_outputs = num_pass_outputs,
			.outputs[0] = {
				.pixel_format = color_format,
				.load_action = SG_LOADACTION_DONTCARE,
				.store_action = SG_STOREACTION_STORE,
			},
			.outputs[1] = {
				.pixel_format = color_format,
				.load_action = SG_LOADACTION_DONTCARE,
				.store_action = SG_STOREACTION_STORE,
			},
			.type = ERenderPassType::Multi,
			.debug_label = "Temporal AA",
		};
	}
}
