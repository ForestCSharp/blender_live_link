#pragma once

#include <optional>

#include "ankerl/unordered_dense.h"
using ankerl::unordered_dense::map;

#include "sky_pass.compiled.h"
#include "sky_bake.compiled.h"

#include "render/render_types.h"
#include "render/render_pass.h"

namespace SkyBakePass
{
	const i32 SkyBakeResolution = 256;
	const i32 num_pass_outputs = 1;

	optional<sg_shader> shader;
	optional<sg_pipeline_desc> pipeline_desc;
	optional<RenderPass> render_pass;
	bool has_baked_sky = false;
	HMM_Vec3 last_baked_sun_dir = HMM_V3(0,0,1);

	sg_pipeline_desc get_pipeline_desc()
	{
		if (!shader.has_value())
		{
			shader = sg_make_shader(sky_bake_sky_bake_shader_desc(sg_query_backend()));
		}

		if (!pipeline_desc.has_value())
		{
			pipeline_desc = (sg_pipeline_desc) {
				.shader = shader.value(),
				.depth = {
					.pixel_format = SG_PIXELFORMAT_NONE,
				},
				.color_count = num_pass_outputs,
				.colors[0] = {
					.pixel_format = SG_PIXELFORMAT_RGBA32F,
				},
				.cull_mode = SG_CULLMODE_NONE,
				.label = "sky-bake-pipeline"
			};
		}

		return pipeline_desc.value();
	}

	RenderPass& get_render_pass()
	{
		if (!render_pass.has_value())
		{
			RenderPassDesc render_pass_desc = {
				.initial_width = SkyBakeResolution,
				.initial_height = SkyBakeResolution,
				.pipeline_desc = SkyBakePass::get_pipeline_desc(),
				.num_outputs = num_pass_outputs,
				.outputs[0] = {
					.pixel_format = SG_PIXELFORMAT_RGBA32F,
					.load_action = SG_LOADACTION_CLEAR,
					.store_action = SG_STOREACTION_STORE,
					.clear_value = {0.0, 0.0, 0.0, 0.0},
				},
				.debug_label = "Sky Bake",
			};
			render_pass.emplace();
			render_pass.value().init(render_pass_desc);
		}

		return render_pass.value();
	}

	void cleanup()
	{
		if (render_pass.has_value())
		{
			render_pass.value().cleanup();
			render_pass.reset();
		}
	}

	HMM_Vec3 get_sun_dir(State& in_state)
	{
		HMM_Vec3 sun_dir = HMM_V3(0,0,1);
		if (in_state.scene.primary_sun_id.has_value())
		{
			i32 primary_sun_id = in_state.scene.primary_sun_id.value();
			if (in_state.scene.objects.contains(primary_sun_id))
			{
				Object& sun_object = in_state.scene.objects[primary_sun_id];
				if (sun_object.has_light && sun_object.light.type == LightType::Sun)
				{
					Transform transform = sun_object.current_transform;
					sun_dir = -HMM_NormV3(HMM_RotateV3Q(HMM_V3(0,0,-1), transform.rotation));
				}
			}
		}
		return sun_dir;
	}

	bool should_bake_sky(const HMM_Vec3 in_sun_dir)
	{
		if (!has_baked_sky)
		{
			return true;
		}

		const HMM_Vec3 sun_dir_delta = in_sun_dir - last_baked_sun_dir;
		return HMM_LenSqrV3(sun_dir_delta) > 0.000001f;
	}

	void render(State& in_state)
	{
		const HMM_Vec3 sun_dir = get_sun_dir(in_state);
		if (!should_bake_sky(sun_dir))
		{
			return;
		}

		SkyBakePass::get_render_pass().execute(
			[&](const i32)
			{
				sky_bake_fs_params_t sky_bake_params = {
					.sun_dir = sun_dir,
				};
				sg_apply_uniforms(0, SG_RANGE(sky_bake_params));

				sg_draw(0,6,1);
			}
		);

		last_baked_sun_dir = sun_dir;
		has_baked_sky = true;
	}

	GpuImage& get_baked_sky_image()
	{
		return SkyBakePass::get_render_pass().get_color_output(0);
	}

	sg_view get_baked_sky_image_view()
	{
		return SkyBakePass::get_baked_sky_image().get_texture_view(0);
	}
}

namespace SkyPass
{
	// this is lazily created the first time we request the GeometryPass desc
	optional<sg_shader> shader;
	map<sg_pixel_format, sg_pipeline> pipelines;
	const i32 num_pass_outputs = 4;

	sg_pipeline get_pipeline(sg_pixel_format depth_format)
	{
		if (!shader.has_value())
		{
			shader = sg_make_shader(sky_pass_sky_pass_shader_desc(sg_query_backend()));
		}

		if (pipelines.find(depth_format) != pipelines.end())
		{
			return pipelines[depth_format];
		}

		pipelines[depth_format] = sg_make_pipeline((sg_pipeline_desc) {
			.shader = shader.value(),
			.depth = {
				.pixel_format = depth_format,
				.compare = Render::DEPTH_COMPARE_FUNC,
				.write_enabled = true,
			},
			.color_count = num_pass_outputs,
			.colors[0] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F,
			},
			.colors[1] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F,
			},
			.colors[2] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F,
			},
			.colors[3] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F,
			},
			.cull_mode = SG_CULLMODE_NONE,
			.label = "sky-pipeline"
		});
		return pipelines[depth_format];
	}

	// Should be called during geometry pass
	void render(const HMM_Mat4& in_view_proj, const HMM_Vec3& in_camera_position, sg_pixel_format in_depth_format)
	{
		sg_apply_pipeline(SkyPass::get_pipeline(in_depth_format));

		sky_pass_fs_params_t sky_params = {
			.inv_view_proj = HMM_InvGeneralM4(in_view_proj),
			.camera_position = in_camera_position,
		};
		sg_apply_uniforms(0, SG_RANGE(sky_params));

		sg_bindings bindings = (sg_bindings){
			.views = {
				[0] = SkyBakePass::get_baked_sky_image_view(),
			},
			.samplers[0] = state.gpu.linear_sampler,
		};
		gpu_apply_bindings(&bindings);

		sg_draw(0,6,1);
	}
}
