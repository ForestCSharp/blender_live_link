#pragma once

#include "render/render_types.h"
#include "render/render_pass.h"

#include "blur.compiled.h"

namespace BlurPass
{
	void execute_separable(
		RenderPass& in_pass,
		sg_view in_input_view,
		sg_sampler in_sampler,
		HMM_Vec2 in_screen_size,
		i32 in_blur_size
	)
	{
		in_pass.execute_scratch(
			0,
			[&](const i32 pass_idx)
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
				sg_apply_bindings(&bindings);
				sg_draw(0, 6, 1);
			}
		);

		in_pass.execute(
			[&](const i32 pass_idx)
			{
				const blur_fs_params_t blur_fs_params = {
					.screen_size = in_screen_size,
					.direction = HMM_V2(0.0f, 1.0f),
					.blur_size = in_blur_size,
				};
				sg_apply_uniforms(0, SG_RANGE(blur_fs_params));

				sg_bindings bindings = {
					.views = {
						[0] = in_pass.get_scratch_color_output(0).get_texture_view(0),
					},
					.samplers[0] = in_sampler,
				};
				sg_apply_bindings(&bindings);
				sg_draw(0, 6, 1);
			}
		);
	}
}
