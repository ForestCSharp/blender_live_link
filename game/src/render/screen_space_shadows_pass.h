#pragma once

#include <optional>

#include "screen_space_shadows.compiled.h"

#include "render/render_types.h"
#include "render/render_pass.h"

namespace ScreenSpaceShadowsPass
{
	optional<sg_shader> trace_shader;
	optional<sg_shader> filter_shader;
	optional<sg_pipeline> filter_pipeline;

	sg_pipeline_desc make_trace_pipeline_desc(sg_pixel_format color_format)
	{
		if (!trace_shader.has_value())
		{
			trace_shader = sg_make_shader(screen_space_shadows_trace_shader_desc(sg_query_backend()));
		}

		return (sg_pipeline_desc) {
			.shader = trace_shader.value(),
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.color_count = 1,
			.colors[0] = {
				.pixel_format = color_format,
			},
			.cull_mode = SG_CULLMODE_NONE,
			.label = "screen-space-shadows-trace-pipeline",
		};
	}

	sg_pipeline make_filter_pipeline(sg_pixel_format color_format)
	{
		if (!filter_shader.has_value())
		{
			filter_shader = sg_make_shader(screen_space_shadows_filter_shader_desc(sg_query_backend()));
		}

		if (!filter_pipeline.has_value())
		{
			sg_pipeline_desc desc = {};
			desc.shader = filter_shader.value();
			desc.depth.pixel_format = SG_PIXELFORMAT_NONE;
			desc.color_count = 1;
			desc.colors[0].pixel_format = color_format;
			desc.cull_mode = SG_CULLMODE_NONE;
			desc.label = "screen-space-shadows-filter-pipeline";
			filter_pipeline = sg_make_pipeline(desc);
		}

		return filter_pipeline.value();
	}

	RenderPassDesc make_render_pass_desc(sg_pixel_format color_format)
	{
		return (RenderPassDesc) {
			.pipeline_desc = make_trace_pipeline_desc(color_format),
			.num_outputs = 1,
			.outputs[0] = {
				.pixel_format = color_format,
				.load_action = SG_LOADACTION_CLEAR,
				.store_action = SG_STOREACTION_STORE,
				.clear_value = {1.0f, 1.0f, 1.0f, 1.0f},
			},
			.num_scratch_outputs = 1,
			.scratch_outputs[0] = {
				.pixel_format = color_format,
				.load_action = SG_LOADACTION_CLEAR,
				.store_action = SG_STOREACTION_STORE,
				.clear_value = {1.0f, 1.0f, 1.0f, 1.0f},
			},
			.width_scale = 0.5f,
			.height_scale = 0.5f,
			.debug_label = "Screen Space Shadows Filter",
			.scratch_debug_label = "Screen Space Shadows Trace",
		};
	}

	void execute(
		RenderPass& in_pass,
		sg_view in_position_view,
		sg_view in_normal_view,
		sg_sampler in_sampler,
		HMM_Mat4 in_view,
		HMM_Mat4 in_projection,
		HMM_Vec3 in_light_direction,
		f32 in_ray_length,
		f32 in_thickness,
		f32 in_jitter_strength,
		i32 in_max_steps,
		i32 in_filter_radius
	)
	{
		in_pass.execute_scratch(
			0,
			[&](const i32)
			{
				const screen_space_shadows_trace_fs_params_t trace_fs_params = {
					.screen_size = HMM_V2((f32)in_pass.current_width, (f32)in_pass.current_height),
					.view = in_view,
					.projection = in_projection,
					.light_direction = in_light_direction,
					.ray_length = in_ray_length,
					.thickness = in_thickness,
					.jitter_strength = in_jitter_strength,
					.max_steps = in_max_steps,
					.enable = 1,
				};
				sg_apply_uniforms(0, SG_RANGE(trace_fs_params));

				sg_bindings bindings = {
					.views = {
						[0] = in_position_view,
						[1] = in_normal_view,
					},
					.samplers[0] = in_sampler,
				};
				gpu_apply_bindings(&bindings);
				sg_draw(0, 6, 1);
			}
		);

		in_pass.execute(
			[&](const i32)
			{
				sg_apply_pipeline(make_filter_pipeline(in_pass.desc.outputs[0].pixel_format));

				const screen_space_shadows_filter_fs_params_t filter_fs_params = {
					.screen_size = HMM_V2((f32)in_pass.current_width, (f32)in_pass.current_height),
					.filter_radius = in_filter_radius,
				};
				sg_apply_uniforms(0, SG_RANGE(filter_fs_params));

				sg_bindings bindings = {
					.views = {
						[0] = in_pass.get_scratch_color_output(0).get_texture_view(0),
						[1] = in_position_view,
						[2] = in_normal_view,
					},
					.samplers[0] = in_sampler,
				};
				gpu_apply_bindings(&bindings);
				sg_draw(0, 6, 1);
			}
		);
	}
}
