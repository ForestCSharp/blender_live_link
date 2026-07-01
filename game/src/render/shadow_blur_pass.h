#pragma once

#include "render/render_types.h"
#include "render/render_pass.h"
#include "render/shadow_depth_pass.h"

#include "shadow_blur.compiled.h"

namespace ShadowBlurPass
{
	optional<sg_shader> shader;

	sg_pipeline_desc make_pipeline_desc()
	{
		if (!shader.has_value())
		{
			shader = sg_make_shader(shadow_blur_shadow_blur_shader_desc(sg_query_backend()));
		}

		sg_pipeline_desc desc = {};
		desc.shader = shader.value();
		desc.depth.pixel_format = SG_PIXELFORMAT_NONE;
		desc.color_count = 1;
		desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA16F;
		desc.cull_mode = SG_CULLMODE_NONE;
		desc.label = "shadow-blur-pipeline";
		return desc;
	}

	RenderPassDesc make_render_pass_desc()
	{
		RenderPassDesc desc = {};
		desc.initial_width = ShadowDepthPass::ShadowMapResolution;
		desc.initial_height = ShadowDepthPass::ShadowMapResolution;
		desc.pipeline_desc = ShadowBlurPass::make_pipeline_desc();
		desc.num_outputs = 1;
		desc.outputs[0].pixel_format = SG_PIXELFORMAT_RGBA16F;
		desc.outputs[0].load_action = SG_LOADACTION_CLEAR;
		desc.outputs[0].store_action = SG_STOREACTION_STORE;
		desc.outputs[0].clear_value = {1.0f, 1.0f, 0.0f, 0.0f};
		desc.num_scratch_outputs = 1;
		desc.scratch_outputs[0].pixel_format = SG_PIXELFORMAT_RGBA16F;
		desc.scratch_outputs[0].load_action = SG_LOADACTION_CLEAR;
		desc.scratch_outputs[0].store_action = SG_STOREACTION_STORE;
		desc.scratch_outputs[0].clear_value = {1.0f, 1.0f, 0.0f, 0.0f};
		desc.resize_with_window = false;
		desc.type = ERenderPassType::Array;
		desc.pass_count = MAX_SHADOW_CASCADES;
		desc.debug_label = "Shadow Blur Vertical";
		desc.scratch_debug_label = "Shadow Blur Horizontal";
		desc.debug_label_formatter = render_pass_format_cascade_debug_label;
		desc.scratch_debug_label_formatter = render_pass_format_cascade_debug_label;
		return desc;
	}

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
			[&](const RenderPassExecutionContext& context)
			{
				const shadow_blur_fs_params_t blur_fs_params = {
					.screen_size = in_screen_size,
					.direction = HMM_V2(1.0f, 0.0f),
					.blur_size = in_blur_size,
					.array_layer = context.slice_idx,
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

		in_pass.execute(
			[&](const RenderPassExecutionContext& context)
			{
				const shadow_blur_fs_params_t blur_fs_params = {
					.screen_size = in_screen_size,
					.direction = HMM_V2(0.0f, 1.0f),
					.blur_size = in_blur_size,
					.array_layer = context.slice_idx,
				};
				sg_apply_uniforms(0, SG_RANGE(blur_fs_params));

				sg_bindings bindings = {
					.views = {
						[0] = in_pass.get_scratch_color_output(0).get_texture_array_view(),
					},
					.samplers[0] = in_sampler,
				};
				gpu_apply_bindings(&bindings);
				sg_draw(0, 6, 1);
			}
		);
	}
}
