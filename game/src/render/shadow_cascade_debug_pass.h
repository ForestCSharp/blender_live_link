#pragma once

#include "render/render_types.h"
#include "render/render_pass.h"
#include "render/shadow_depth_pass.h"

#include "shadow_cascade_debug.compiled.h"

namespace ShadowCascadeDebugPass
{
	optional<sg_shader> shader;

	sg_pipeline_desc make_pipeline_desc()
	{
		if (!shader.has_value())
		{
			shader = sg_make_shader(shadow_cascade_debug_shadow_cascade_debug_shader_desc(sg_query_backend()));
		}

		sg_pipeline_desc desc = {};
		desc.shader = shader.value();
		desc.depth.pixel_format = SG_PIXELFORMAT_NONE;
		desc.color_count = 1;
		desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA16F;
		desc.cull_mode = SG_CULLMODE_NONE;
		desc.label = "shadow-cascade-debug-pipeline";
		return desc;
	}

	RenderPassDesc make_render_pass_desc()
	{
		RenderPassDesc desc = {};
		desc.initial_width = ShadowDepthPass::ShadowMapResolution;
		desc.initial_height = ShadowDepthPass::ShadowMapResolution;
		desc.pipeline_desc = ShadowCascadeDebugPass::make_pipeline_desc();
		desc.num_outputs = 1;
		desc.outputs[0].pixel_format = SG_PIXELFORMAT_RGBA16F;
		desc.outputs[0].load_action = SG_LOADACTION_CLEAR;
		desc.outputs[0].store_action = SG_STOREACTION_STORE;
		desc.outputs[0].clear_value = {0.0f, 0.0f, 0.0f, 1.0f};
		desc.resize_with_window = false;
		desc.debug_label = "Shadow Cascade Debug";
		return desc;
	}

	void render(
		RenderPass& in_pass,
		sg_view in_shadow_moments_view,
		sg_sampler in_sampler,
		i32 in_cascade_index,
		i32 in_view_mode
	)
	{
		in_pass.execute(
			[&](const i32)
			{
				const shadow_cascade_debug_fs_params_t fs_params = {
					.cascade_index = in_cascade_index,
					.view_mode = in_view_mode,
				};
				sg_apply_uniforms(0, SG_RANGE(fs_params));

				sg_bindings bindings = {
					.views = {
						[0] = in_shadow_moments_view,
					},
					.samplers[0] = in_sampler,
				};
				sg_apply_bindings(&bindings);
				sg_draw(0, 6, 1);
			}
		);
	}
}
