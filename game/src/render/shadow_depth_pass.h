#pragma once

#include <optional>

#include "shadow_depth.compiled.h"

#include "render/render_types.h"
#include "render/render_pass.h"
#include "state/state.h"

#if defined(SOKOL_D3D11) || defined(SOKOL_WGPU) || defined(SOKOL_METAL)
    #define SHADOW_ORTHOGRAPHIC_FUNCTION HMM_Orthographic_RH_ZO
#else
    #define SHADOW_ORTHOGRAPHIC_FUNCTION HMM_Orthographic_RH_NO
#endif

namespace ShadowDepthPass
{
	inline HMM_Mat4 mat4_orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 near_plane, f32 far_plane)
	{
	#if USE_INVERSE_DEPTH
		return SHADOW_ORTHOGRAPHIC_FUNCTION(left, right, bottom, top, far_plane, near_plane);
	#else
		return SHADOW_ORTHOGRAPHIC_FUNCTION(left, right, bottom, top, near_plane, far_plane);
	#endif
	}

	optional<sg_shader> shader;		

	const i32 num_pass_outputs = 0;
	const i32 ShadowMapResolution = 4096;
	bool has_valid_shadow_map = false;
	HMM_Mat4 shadow_view_projection = HMM_M4D(1.0f);

	sg_pipeline_desc make_pipeline_desc(sg_pixel_format depth_format)
	{
		if (!shader.has_value())
		{
			shader = sg_make_shader(shadow_depth_shadow_depth_shader_desc(sg_query_backend()));
		}

		sg_pipeline_desc desc = {};
		desc.shader = shader.value();
		desc.layout.buffers[0].stride = sizeof(Vertex);
		desc.layout.attrs[ATTR_shadow_depth_shadow_depth_position].format = SG_VERTEXFORMAT_FLOAT4;
		desc.layout.attrs[ATTR_shadow_depth_shadow_depth_normal].format = SG_VERTEXFORMAT_FLOAT4;
		desc.layout.attrs[ATTR_shadow_depth_shadow_depth_texcoord].format = SG_VERTEXFORMAT_FLOAT2;
		desc.depth.pixel_format = depth_format;
		desc.depth.compare = Render::DEPTH_COMPARE_FUNC;
		desc.depth.write_enabled = true;
		desc.colors[0].pixel_format = SG_PIXELFORMAT_NONE;
		desc.index_type = SG_INDEXTYPE_UINT32;
		desc.cull_mode = SG_CULLMODE_NONE;
		desc.label = "shadow-depth-pipeline";
		return desc;
	}

	RenderPassDesc make_render_pass_desc(sg_pixel_format depth_format)
	{
		RenderPassDesc desc = {};
		desc.initial_width = ShadowMapResolution;
		desc.initial_height = ShadowMapResolution;
		desc.pipeline_desc = ShadowDepthPass::make_pipeline_desc(depth_format);
		desc.num_outputs = num_pass_outputs;
		desc.depth_output.pixel_format = depth_format;
		desc.depth_output.load_action = SG_LOADACTION_CLEAR;
		desc.depth_output.store_action = SG_STOREACTION_STORE;
		desc.depth_output.clear_value = Render::DEPTH_CLEAR_VALUE;
		desc.resize_with_window = false;
		return desc;
	}

	void render(State& in_state)
	{
		has_valid_shadow_map = false;

		if (!in_state.primary_sun_id.has_value())
		{
			return;
		}

		i32 primary_sun_id = in_state.primary_sun_id.value();
		if (!in_state.objects.contains(primary_sun_id))
		{
			return;
		}

		Object& sun_object = in_state.objects[primary_sun_id];
		if (!sun_object.visibility || !sun_object.has_light || sun_object.light.type != LightType::Sun || !sun_object.light.sun.cast_shadows)
		{
			return;
		}

		Transform transform = sun_object.current_transform;
		HMM_Vec3 sun_dir = HMM_NormV3(HMM_RotateV3Q(HMM_V3(0,0,-1), transform.rotation));

		Camera& active_camera = get_active_camera();
		HMM_Vec3 light_up = HMM_V3(0.0f, 0.0f, 1.0f);
		if (fabsf(HMM_DotV3(sun_dir, light_up)) > 0.99f)
		{
			light_up = HMM_V3(0.0f, 1.0f, 0.0f);
		}

		HMM_Vec3 light_pos = active_camera.location - sun_dir * 50.0f;
		HMM_Mat4 light_view = HMM_LookAt_RH(light_pos, active_camera.location, light_up);

		f32 half_width = 100.0f;
		f32 half_height = 100.0f;
		f32 near_plane = 0.1f;
		f32 far_plane = 100.0f;
		HMM_Mat4 light_proj = mat4_orthographic(-half_width, half_width, -half_height, half_height, near_plane, far_plane);
		HMM_Mat4 light_view_proj = HMM_MulM4(light_proj, light_view);
		shadow_view_projection = light_view_proj;
		has_valid_shadow_map = true;

		shadow_depth_vs_params_t vs_params;
		vs_params.view = light_view;
		vs_params.projection = light_proj;

		// Apply Vertex Uniforms
		sg_apply_uniforms(0, SG_RANGE(vs_params));

		// Cull objects
		CullResult cull_result = cull_objects(in_state.objects, light_view_proj);

		// Submit draw calls for objects after culling
		for (auto& [unique_id, object_ptr] : cull_result.objects)
		{
			assert(object_ptr);
			Object& object = *object_ptr;

			if (object.has_mesh)
			{
				Mesh& mesh = object.mesh;

				sg_bindings bindings = {};
				bindings.vertex_buffers[0] = mesh.vertex_buffer.get_gpu_buffer();
				bindings.index_buffer = mesh.index_buffer.get_gpu_buffer();
				bindings.views[0] = object.storage_buffer.get_storage_view();
				sg_apply_bindings(&bindings);
				sg_draw(0, mesh.index_count, 1);
			}
		}
	}
}
