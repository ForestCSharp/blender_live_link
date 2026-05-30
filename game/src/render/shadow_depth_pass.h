#pragma once

#include <cfloat>
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

	const i32 num_pass_outputs = 1;
	const i32 ShadowMapResolution = 4096;
	const f32 BaselineCascadeDistance = 100.0f;
	bool has_valid_shadow_map = false;
	bool has_valid_shadow_blur = false;
	HMM_Mat4 shadow_view_projections[MAX_SHADOW_CASCADES] = {};
	f32 cascade_distances[MAX_SHADOW_CASCADES] = {};

	i32 get_active_cascade_count(const State& in_state)
	{
		if (in_state.shadow_num_cascades < 1)
		{
			return 1;
		}
		if (in_state.shadow_num_cascades > MAX_SHADOW_CASCADES)
		{
			return MAX_SHADOW_CASCADES;
		}
		return in_state.shadow_num_cascades;
	}

	f32 get_cascade_distance(const State& in_state, i32 cascade_idx)
	{
		const f32 active_scale = in_state.shadow_cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares
			? in_state.shadow_centered_square_cascade_distance_scale
			: in_state.shadow_frustum_cascade_distance_scale;
		const f32 scale = fmaxf(0.01f, active_scale);
		if (get_active_cascade_count(in_state) == 1)
		{
			return BaselineCascadeDistance * scale;
		}

		const f32 exponent = (f32)cascade_idx - 1.0f;
		return BaselineCascadeDistance * scale * powf(2.0f, exponent);
	}

	f32 get_largest_active_cascade_distance(const State& in_state)
	{
		return get_cascade_distance(in_state, get_active_cascade_count(in_state) - 1);
	}

	void get_frustum_slice_corners(
		const Camera& in_camera,
		f32 in_near_distance,
		f32 in_far_distance,
		f32 in_fov_radians,
		f32 in_aspect_ratio,
		HMM_Vec3 out_corners[8]
	)
	{
		const HMM_Vec3 forward = HMM_NormV3(in_camera.forward);
		const HMM_Vec3 right = HMM_NormV3(HMM_Cross(forward, in_camera.up));
		const HMM_Vec3 up = HMM_NormV3(HMM_Cross(right, forward));
		const f32 tan_half_fov = tanf(in_fov_radians * 0.5f);

		const f32 near_half_height = tan_half_fov * in_near_distance;
		const f32 near_half_width = near_half_height * in_aspect_ratio;
		const f32 far_half_height = tan_half_fov * in_far_distance;
		const f32 far_half_width = far_half_height * in_aspect_ratio;

		const HMM_Vec3 near_center = in_camera.location + forward * in_near_distance;
		const HMM_Vec3 far_center = in_camera.location + forward * in_far_distance;

		out_corners[0] = near_center - right * near_half_width - up * near_half_height;
		out_corners[1] = near_center + right * near_half_width - up * near_half_height;
		out_corners[2] = near_center - right * near_half_width + up * near_half_height;
		out_corners[3] = near_center + right * near_half_width + up * near_half_height;
		out_corners[4] = far_center - right * far_half_width - up * far_half_height;
		out_corners[5] = far_center + right * far_half_width - up * far_half_height;
		out_corners[6] = far_center - right * far_half_width + up * far_half_height;
		out_corners[7] = far_center + right * far_half_width + up * far_half_height;
	}

	Object* get_valid_shadow_sun(State& in_state)
	{
		if (!in_state.primary_sun_id.has_value())
		{
			return nullptr;
		}

		i32 primary_sun_id = in_state.primary_sun_id.value();
		if (!in_state.objects.contains(primary_sun_id))
		{
			return nullptr;
		}

		Object& sun_object = in_state.objects[primary_sun_id];
		if (!sun_object.visibility || !sun_object.has_light || sun_object.light.type != LightType::Sun || !sun_object.light.sun.cast_shadows)
		{
			return nullptr;
		}

		return &sun_object;
	}

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
		desc.color_count = num_pass_outputs;
		desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA16F;
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
		desc.outputs[0].pixel_format = SG_PIXELFORMAT_RGBA16F;
		desc.outputs[0].load_action = SG_LOADACTION_CLEAR;
		desc.outputs[0].store_action = SG_STOREACTION_STORE;
		desc.outputs[0].clear_value = {1.0f, 1.0f, 0.0f, 0.0f};
		desc.depth_output.pixel_format = depth_format;
		desc.depth_output.load_action = SG_LOADACTION_CLEAR;
		desc.depth_output.store_action = SG_STOREACTION_STORE;
		desc.depth_output.clear_value = Render::DEPTH_CLEAR_VALUE;
		desc.resize_with_window = false;
		desc.type = ERenderPassType::Array;
		desc.pass_count = MAX_SHADOW_CASCADES;
		return desc;
	}

	void render(State& in_state, i32 cascade_idx)
	{
		if (cascade_idx == 0)
		{
			has_valid_shadow_map = false;
			for (i32 i = 0; i < MAX_SHADOW_CASCADES; ++i)
			{
				shadow_view_projections[i] = HMM_M4D(1.0f);
				cascade_distances[i] = 0.0f;
			}
		}

		if (cascade_idx >= get_active_cascade_count(in_state))
		{
			return;
		}

		Object* sun_object = get_valid_shadow_sun(in_state);
		if (!sun_object)
		{
			return;
		}

		Transform transform = sun_object->current_transform;
		HMM_Vec3 sun_dir = HMM_NormV3(HMM_RotateV3Q(HMM_V3(0,0,-1), transform.rotation));

		Camera& active_camera = get_active_camera();
		HMM_Vec3 light_up = HMM_V3(0.0f, 0.0f, 1.0f);
		if (fabsf(HMM_DotV3(sun_dir, light_up)) > 0.99f)
		{
			light_up = HMM_V3(0.0f, 1.0f, 0.0f);
		}

		if (in_state.shadow_cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares)
		{
			const f32 cascade_half_extent = get_cascade_distance(in_state, cascade_idx);
			const f32 largest_half_extent = get_largest_active_cascade_distance(in_state);
			const f32 light_depth_range = fmaxf(100.0f, largest_half_extent * 4.0f);
			HMM_Vec3 light_pos = in_state.shadow_centered_square_center - sun_dir * (light_depth_range * 0.5f);
			HMM_Mat4 light_view = HMM_LookAt_RH(light_pos, in_state.shadow_centered_square_center, light_up);
			f32 near_plane = 0.01f;
			f32 far_plane = light_depth_range;
			HMM_Mat4 light_proj = mat4_orthographic(
				-cascade_half_extent,
				cascade_half_extent,
				-cascade_half_extent,
				cascade_half_extent,
				near_plane,
				far_plane
			);
			HMM_Mat4 light_view_proj = HMM_MulM4(light_proj, light_view);
			shadow_view_projections[cascade_idx] = light_view_proj;
			cascade_distances[cascade_idx] = cascade_half_extent;
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

			return;
		}

		const f32 cascade_near_distance = cascade_idx == 0 ? 0.01f : get_cascade_distance(in_state, cascade_idx - 1);
		const f32 cascade_far_distance = get_cascade_distance(in_state, cascade_idx);
		const f32 fov = HMM_AngleDeg(60.0f);
		const f32 aspect_ratio = sapp_widthf() / sapp_heightf();

		HMM_Vec3 frustum_corners[8];
		get_frustum_slice_corners(active_camera, cascade_near_distance, cascade_far_distance, fov, aspect_ratio, frustum_corners);

		HMM_Vec3 frustum_center = HMM_V3(0.0f, 0.0f, 0.0f);
		for (i32 i = 0; i < 8; ++i)
		{
			frustum_center += frustum_corners[i] * 0.125f;
		}

		f32 bounding_radius = 0.0f;
		for (i32 i = 0; i < 8; ++i)
		{
			bounding_radius = fmaxf(bounding_radius, HMM_LenV3(frustum_corners[i] - frustum_center));
		}

		const f32 depth_margin = fmaxf(10.0f, bounding_radius * 0.25f);
		HMM_Vec3 light_pos = frustum_center - sun_dir * (bounding_radius + depth_margin);
		HMM_Mat4 light_view = HMM_LookAt_RH(light_pos, frustum_center, light_up);

		f32 min_x = FLT_MAX;
		f32 max_x = -FLT_MAX;
		f32 min_y = FLT_MAX;
		f32 max_y = -FLT_MAX;
		f32 min_z = FLT_MAX;
		f32 max_z = -FLT_MAX;
		for (i32 i = 0; i < 8; ++i)
		{
			HMM_Vec4 light_space_corner = HMM_MulM4V4(light_view, HMM_V4V(frustum_corners[i], 1.0f));
			min_x = fminf(min_x, light_space_corner.X);
			max_x = fmaxf(max_x, light_space_corner.X);
			min_y = fminf(min_y, light_space_corner.Y);
			max_y = fmaxf(max_y, light_space_corner.Y);
			min_z = fminf(min_z, light_space_corner.Z);
			max_z = fmaxf(max_z, light_space_corner.Z);
		}

		f32 near_plane = fmaxf(0.01f, -max_z - depth_margin);
		f32 far_plane = fmaxf(near_plane + 1.0f, -min_z + depth_margin);
		HMM_Mat4 light_proj = mat4_orthographic(min_x, max_x, min_y, max_y, near_plane, far_plane);
		HMM_Mat4 light_view_proj = HMM_MulM4(light_proj, light_view);
		shadow_view_projections[cascade_idx] = light_view_proj;
		cascade_distances[cascade_idx] = cascade_far_distance;
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
