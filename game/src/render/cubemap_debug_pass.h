#pragma once

#include <optional>

#include "cubemap_debug.compiled.h"

#include "render/render_types.h"
#include "render/render_pass.h"

namespace CubemapDebugPass
{
	// this is lazily created the first time we request the GeometryPass desc
	optional<sg_shader> shader;
	optional<sg_pipeline> pipeline;

	optional<RenderPass> pass;
	optional<Mesh> debug_mesh;

	const i32 num_pass_outputs = 4;

	sg_pipeline_desc make_pipeline_desc(sg_pixel_format depth_format)
	{
		if (!shader.has_value())
		{
			shader = sg_make_shader(cubemap_debug_cubemap_debug_shader_desc(sg_query_backend()));
		}

		return (sg_pipeline_desc) {
			.layout = {
				.buffers[0].stride = sizeof(Vertex),
				.attrs = {
					[ATTR_geometry_geometry_position].format = SG_VERTEXFORMAT_FLOAT4,
					[ATTR_geometry_geometry_normal].format   = SG_VERTEXFORMAT_FLOAT4,
					[ATTR_geometry_geometry_texcoord].format = SG_VERTEXFORMAT_FLOAT2,
				},
			},
			.shader = shader.value(),
			.index_type = SG_INDEXTYPE_UINT32,
			.cull_mode = SG_CULLMODE_NONE,
			.depth = {
					.pixel_format = depth_format,
					.write_enabled = true,
					.compare = Render::DEPTH_COMPARE_FUNC,
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
			.label = "cubemap-debug-pipeline"
		};
	}

	sg_pipeline get_pipeline(sg_pixel_format depth_format)
	{
		if (!pipeline.has_value())
		{
			pipeline = sg_make_pipeline(make_pipeline_desc(depth_format));
		}

		return pipeline.value();
	}

	void render(HMM_Vec3 location, GpuImage& in_cubemap_image)
	{
		//get_pass().execute()
	}
}
