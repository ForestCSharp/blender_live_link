#pragma once

#include <optional>

#include "geometry.compiled.h"

#include "render/render_types.h"
#include "render/render_pass.h"

namespace GeometryPass
{
	// this is lazily created the first time we request the GeometryPass desc
	optional<sg_shader> shader;		
	optional<sg_shader> skinned_shader;
	map<sg_pixel_format, sg_pipeline> skinned_pipelines;
	optional<sg_shader> wireframe_shader;
	optional<sg_pipeline> wireframe_pipeline;

	const i32 num_pass_outputs = 4;

	sg_pipeline_desc make_pipeline_desc(sg_pixel_format depth_format)
	{
		if (!shader.has_value())
		{
			shader = sg_make_shader(geometry_geometry_shader_desc(sg_query_backend()));
		}

		return (sg_pipeline_desc) {
			.shader = shader.value(),
			.layout = {
				.buffers[0].stride = sizeof(Vertex),
				.attrs = {
					[ATTR_geometry_geometry_position].format = SG_VERTEXFORMAT_FLOAT4,
					[ATTR_geometry_geometry_normal].format   = SG_VERTEXFORMAT_FLOAT4,
					[ATTR_geometry_geometry_texcoord].format = SG_VERTEXFORMAT_FLOAT2,
				}
			},
			.depth = {
				.pixel_format = depth_format,
				.compare = Render::DEPTH_COMPARE_FUNC, // Inverse Depth
				.write_enabled = true,
			},
			.color_count = num_pass_outputs,
			.colors[0] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F, // Color
			},
			.colors[1] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F, // World Position
			},
			.colors[2] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F, // World Normal
			},
			.colors[3] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F, // (Roughness, Metallic, Emissive, <Unused>)
			},
			.index_type = SG_INDEXTYPE_UINT32,
			.cull_mode = SG_CULLMODE_NONE,
			.label = "geometry-pipeline"
		};
	}

	RenderPassDesc make_render_pass_desc(sg_pixel_format depth_format)
	{
		return (RenderPassDesc) {
			.pipeline_desc = GeometryPass::make_pipeline_desc(depth_format),
			.num_outputs = num_pass_outputs,
			.outputs[0] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F, // Color
				.load_action = SG_LOADACTION_CLEAR,
				.store_action = SG_STOREACTION_STORE,
				.clear_value = {0.0, 0.0, 0.0, 1.0},
			},
			.outputs[1] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F, // World Position
				.load_action = SG_LOADACTION_CLEAR,
				.store_action = SG_STOREACTION_STORE,
				.clear_value = {0.0, 0.0, 0.0, 0.0},
			},
			.outputs[2] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F, // World Normal
				.load_action = SG_LOADACTION_CLEAR,
				.store_action = SG_STOREACTION_STORE,
				.clear_value = {0.0, 0.0, 0.0, 0.0},
			},
			.outputs[3] = {
				.pixel_format = SG_PIXELFORMAT_RGBA32F, // (Roughness, Metallic, Emissive, <Unused>)
				.load_action = SG_LOADACTION_CLEAR,
				.store_action = SG_STOREACTION_STORE,
				.clear_value = {0.0, 0.0, 0.0, 0.0},
			},	
			.depth_output = {
				.pixel_format = depth_format,
				.load_action = SG_LOADACTION_CLEAR,
				.store_action = SG_STOREACTION_STORE,
				.clear_value = Render::DEPTH_CLEAR_VALUE,
			},
			.debug_label = "Geometry",
		};
	}

	sg_pipeline get_skinned_pipeline(sg_pixel_format depth_format)
	{
		if (!skinned_shader.has_value())
		{
			skinned_shader = sg_make_shader(geometry_skinned_geometry_shader_desc(sg_query_backend()));
		}

		if (skinned_pipelines.find(depth_format) != skinned_pipelines.end())
		{
			return skinned_pipelines[depth_format];
		}

		{
			sg_pipeline_desc desc = {};
			desc.shader = skinned_shader.value();
			desc.layout.buffers[0].stride = sizeof(Vertex);
			desc.layout.buffers[1].stride = sizeof(SkinnedVertex);
			desc.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT4;
			desc.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT4;
			desc.layout.attrs[2].format = SG_VERTEXFORMAT_FLOAT2;
			desc.layout.attrs[3].format = SG_VERTEXFORMAT_FLOAT4;
			desc.layout.attrs[3].buffer_index = 1;
			desc.layout.attrs[4].format = SG_VERTEXFORMAT_FLOAT4;
			desc.layout.attrs[4].buffer_index = 1;
			desc.depth.pixel_format = depth_format;
			desc.depth.compare = Render::DEPTH_COMPARE_FUNC;
			desc.depth.write_enabled = true;
			desc.color_count = num_pass_outputs;
			desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA32F;
			desc.colors[1].pixel_format = SG_PIXELFORMAT_RGBA32F;
			desc.colors[2].pixel_format = SG_PIXELFORMAT_RGBA32F;
			desc.colors[3].pixel_format = SG_PIXELFORMAT_RGBA32F;
			desc.index_type = SG_INDEXTYPE_UINT32;
			desc.cull_mode = SG_CULLMODE_NONE;
			desc.label = "skinned-geometry-pipeline";
			skinned_pipelines[depth_format] = sg_make_pipeline(desc);
		}

		return skinned_pipelines[depth_format];
	}

	sg_pipeline get_wireframe_pipeline(sg_pixel_format depth_format)
	{
		if (!wireframe_shader.has_value())
		{
			wireframe_shader = sg_make_shader(geometry_wireframe_shader_desc(sg_query_backend()));
		}

		if (!wireframe_pipeline.has_value())
		{
			wireframe_pipeline = sg_make_pipeline((sg_pipeline_desc) {
				.shader = wireframe_shader.value(),
				.layout = {
					.buffers[0].stride = sizeof(Vertex),
					.attrs = {
						[ATTR_geometry_wireframe_position].format = SG_VERTEXFORMAT_FLOAT4,
						[ATTR_geometry_wireframe_normal].format   = SG_VERTEXFORMAT_FLOAT4,
						[ATTR_geometry_wireframe_texcoord].format = SG_VERTEXFORMAT_FLOAT2,
					}
				},
				.primitive_type = SG_PRIMITIVETYPE_LINES,
				.depth = {
					.pixel_format = depth_format,
					.compare = Render::DEPTH_COMPARE_FUNC,
					.write_enabled = false,
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
				.index_type = SG_INDEXTYPE_UINT32,
				.cull_mode = SG_CULLMODE_NONE,
				.label = "geometry-wireframe-pipeline",
			});
		}

		return wireframe_pipeline.value();
	}
}
