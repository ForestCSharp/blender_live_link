#pragma once

#include <optional>

#include "geometry.compiled.h"

#include "render/render_types.h"
#include "render/render_pass.h"

namespace GeometryPass
{
	// this is lazily created the first time we request the GeometryPass desc
	optional<sg_shader> shader;		

	RenderPassDesc make_render_pass_desc(sg_pixel_format depth_format)
	{
		if (!shader.has_value())
		{
			shader = sg_make_shader(geometry_geometry_shader_desc(sg_query_backend()));
		}

		const i32 num_geometry_pass_outputs = 4;
		return (RenderPassDesc) {
			.pipeline_desc = (sg_pipeline_desc) {
				.layout = {
					.buffers[0].stride = sizeof(Vertex),
					.attrs = {
						[ATTR_geometry_geometry_position].format = SG_VERTEXFORMAT_FLOAT4,
						[ATTR_geometry_geometry_normal].format   = SG_VERTEXFORMAT_FLOAT4,
						[ATTR_geometry_geometry_texcoord].format = SG_VERTEXFORMAT_FLOAT2,
					}
				},
				.shader = shader.value(),
				.index_type = SG_INDEXTYPE_UINT32,
				.cull_mode = SG_CULLMODE_NONE,
				.depth = {
					.pixel_format = depth_format,
					.write_enabled = true,
					.compare = SG_COMPAREFUNC_LESS_EQUAL,
				},
				.color_count = num_geometry_pass_outputs,
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
				.label = "geometry-pipeline"
			},
			.num_outputs = num_geometry_pass_outputs,
			.outputs[0] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F,
				.load_action = SG_LOADACTION_CLEAR,
				.store_action = SG_STOREACTION_STORE,
				.clear_value = {0.25, 0.25, 0.25, 1.0},
			},
			.outputs[1] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F,
				.load_action = SG_LOADACTION_CLEAR,
				.store_action = SG_STOREACTION_STORE,
				.clear_value = {0.0, 0.0, 0.0, 0.0},
			},
			.outputs[2] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F,
				.load_action = SG_LOADACTION_CLEAR,
				.store_action = SG_STOREACTION_STORE,
				.clear_value = {0.0, 0.0, 0.0, 0.0},
			},
			.outputs[3] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F,
				.load_action = SG_LOADACTION_CLEAR,
				.store_action = SG_STOREACTION_STORE,
				.clear_value = {0.0, 0.0, 0.0, 0.0},
			},	
			.depth_output = {
				.pixel_format = depth_format,
			},
		};
	}
}
