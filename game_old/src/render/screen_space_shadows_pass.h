#pragma once

#include <optional>

#include "screen_space_shadows.compiled.h"

#include "render/render_types.h"
#include "render/render_pass.h"

namespace ScreenSpaceShadowsPass
{
	optional<sg_shader> trace_shader;
	optional<sg_shader> filter_shader;

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

	sg_pipeline_desc make_filter_pipeline_desc(sg_pixel_format color_format)
	{
		if (!filter_shader.has_value())
		{
			filter_shader = sg_make_shader(screen_space_shadows_filter_shader_desc(sg_query_backend()));
		}

		sg_pipeline_desc desc = {};
		desc.shader = filter_shader.value();
		desc.depth.pixel_format = SG_PIXELFORMAT_NONE;
		desc.color_count = 1;
		desc.colors[0].pixel_format = color_format;
		desc.cull_mode = SG_CULLMODE_NONE;
		desc.label = "screen-space-shadows-filter-pipeline";
		return desc;
	}

	RenderPassDesc make_trace_render_pass_desc(sg_pixel_format color_format)
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
			.width_scale = 0.5f,
			.height_scale = 0.5f,
			.debug_label = "Screen Space Shadows Trace",
		};
	}

	RenderPassDesc make_filter_render_pass_desc(sg_pixel_format color_format)
	{
		return (RenderPassDesc) {
			.pipeline_desc = make_filter_pipeline_desc(color_format),
			.num_outputs = 1,
			.outputs[0] = {
				.pixel_format = color_format,
				.load_action = SG_LOADACTION_CLEAR,
				.store_action = SG_STOREACTION_STORE,
				.clear_value = {1.0f, 1.0f, 1.0f, 1.0f},
			},
			.width_scale = 0.5f,
			.height_scale = 0.5f,
			.debug_label = "Screen Space Shadows Filter",
		};
	}

	void init(RenderPassEntry& in_entry, sg_pixel_format color_format)
	{
		in_entry.init_intermediate(make_trace_render_pass_desc(color_format));
		in_entry.init_final(make_filter_render_pass_desc(color_format));
	}

	void execute(
		RenderPassEntry& in_entry,
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
		RenderPass& trace_pass = in_entry.intermediate_pass();
		RenderPass& filter_pass = in_entry.final_pass();

		trace_pass.execute(
			[&](const i32)
			{
				const screen_space_shadows_trace_fs_params_t trace_fs_params = {
					.screen_size = HMM_V2((f32)trace_pass.current_width, (f32)trace_pass.current_height),
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

		filter_pass.execute(
			[&](const i32)
			{
				const screen_space_shadows_filter_fs_params_t filter_fs_params = {
					.screen_size = HMM_V2((f32)filter_pass.current_width, (f32)filter_pass.current_height),
					.filter_radius = in_filter_radius,
				};
				sg_apply_uniforms(0, SG_RANGE(filter_fs_params));

				sg_bindings bindings = {
					.views = {
						[0] = trace_pass.get_color_output(0).get_texture_view(0),
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
