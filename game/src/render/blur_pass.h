#pragma once

#include "render/render_types.h"
#include "render/render_pass.h"

#include "blur.compiled.h"

namespace BlurPass
{
	optional<sg_shader> shader;

	sg_pipeline_desc make_pipeline_desc(sg_pixel_format color_format)
	{
		if (!shader.has_value())
		{
			shader = sg_make_shader(blur_blur_shader_desc(sg_query_backend()));
		}

		return (sg_pipeline_desc) {
			.shader = shader.value(),
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.color_count = 1,
			.colors[0] = {
				.pixel_format = color_format,
			},
			.cull_mode = SG_CULLMODE_NONE,
			.label = "blur-pipeline",
		};
	}

	RenderPassDesc make_render_pass_desc(
		sg_pixel_format color_format,
		f32 width_scale,
		f32 height_scale,
		const char* debug_label
	)
	{
		return (RenderPassDesc) {
			.pipeline_desc = make_pipeline_desc(color_format),
			.num_outputs = 1,
			.outputs[0] = {
				.pixel_format = color_format,
				.load_action = SG_LOADACTION_DONTCARE,
				.store_action = SG_STOREACTION_STORE,
			},
			.width_scale = width_scale,
			.height_scale = height_scale,
			.debug_label = debug_label,
		};
	}

	void init_separable(
		RenderPassEntry& in_entry,
		sg_pixel_format color_format,
		f32 width_scale,
		f32 height_scale,
		const char* horizontal_label,
		const char* vertical_label
	)
	{
		in_entry.init_intermediate(make_render_pass_desc(color_format, width_scale, height_scale, horizontal_label));
		in_entry.init_final(make_render_pass_desc(color_format, width_scale, height_scale, vertical_label));
	}

	void execute_separable(
		RenderPassEntry& in_entry,
		sg_view in_input_view,
		sg_sampler in_sampler,
		HMM_Vec2 in_screen_size,
		i32 in_blur_size
	)
	{
		RenderPass& horizontal_pass = in_entry.intermediate_pass();
		RenderPass& vertical_pass = in_entry.final_pass();

		horizontal_pass.execute(
			[&](const i32)
			{
				const blur_fs_params_t blur_fs_params = {
					.screen_size = in_screen_size,
					.direction = HMM_V2(1.0f, 0.0f),
					.blur_size = in_blur_size,
				};
				sg_apply_uniforms(0, SG_RANGE(blur_fs_params));

				sg_bindings bindings = {
					.views = {
						[0] = in_input_view,
					},
					.samplers[0] = in_sampler,
				};
				gpu_apply_bindings(&bindings);
				sg_draw(0, 6, 1);
			}
		);

		vertical_pass.execute(
			[&](const i32)
			{
				const blur_fs_params_t blur_fs_params = {
					.screen_size = in_screen_size,
					.direction = HMM_V2(0.0f, 1.0f),
					.blur_size = in_blur_size,
				};
				sg_apply_uniforms(0, SG_RANGE(blur_fs_params));

				sg_bindings bindings = {
					.views = {
						[0] = horizontal_pass.get_color_output(0).get_texture_view(0),
					},
					.samplers[0] = in_sampler,
				};
				gpu_apply_bindings(&bindings);
				sg_draw(0, 6, 1);
			}
		);
	}
}
