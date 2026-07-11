#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/shader_module.h"
#include "render/frame_data.h"
#include "render/culling.h"
#include "game_object/mesh.h"

#include <cfloat>
#include <cmath>

// Cascaded EVSM shadow maps (port of game/src/render/shadow_depth_pass.h,
// Frustum placement mode only — CenteredSquares arrives in 3b).
// Array pass: one 2048x2048 RGBA16F moments image with MAX_SHADOW_CASCADES
// layers + throwaway D32; each slice renders one cascade. Exponential
// doubling splits (100 * scale * 2^(i-1)), sphere-bounded frustum fit,
// no texel snapping (game/ parity).

namespace ShadowDepthPass
{
	constexpr i32 ShadowMapResolution = 2048;
	constexpr f32 BaselineCascadeDistance = 100.0f;

	inline bool has_valid_shadow_map = false;
	inline HMM_Mat4 shadow_view_projections[MAX_SHADOW_CASCADES] = {};
	inline f32 cascade_distances[MAX_SHADOW_CASCADES] = {};

	struct PushConstants
	{
		HMM_Mat4 light_view_projection;
		i32 object_index;
		i32 skin_matrix_offset;
	};
	// 72 bytes of payload, padded to 80 by HMM_Mat4's 16-byte alignment —
	// still under the 128-byte push constant minimum
	static_assert(sizeof(PushConstants) == 80, "Must fit the 128-byte push constant minimum");

	inline VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	inline VkPipeline pipeline = VK_NULL_HANDLE;
	inline VkPipeline skinned_pipeline = VK_NULL_HANDLE;
	inline VkPipeline bound_pipeline = VK_NULL_HANDLE;

	// Reverse-Z ortho: near/far swapped (game/ shadow_depth_pass.h:21-28)
	inline HMM_Mat4 mat4_orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 near_plane, f32 far_plane)
	{
		return HMM_Orthographic_RH_ZO(left, right, bottom, top, far_plane, near_plane);
	}

	inline i32 get_active_cascade_count(const State& in_state)
	{
		if (in_state.shadow.num_cascades < 1)
		{
			return 1;
		}
		if (in_state.shadow.num_cascades > MAX_SHADOW_CASCADES)
		{
			return MAX_SHADOW_CASCADES;
		}
		return in_state.shadow.num_cascades;
	}

	inline f32 get_cascade_distance(const State& in_state, i32 cascade_idx)
	{
		const f32 active_scale = in_state.shadow.cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares
			? in_state.shadow.centered_square_cascade_distance_scale
			: in_state.shadow.frustum_cascade_distance_scale;
		const f32 scale = fmaxf(0.01f, active_scale);
		if (get_active_cascade_count(in_state) == 1)
		{
			return BaselineCascadeDistance * scale;
		}

		const f32 exponent = (f32) cascade_idx - 1.0f;
		return BaselineCascadeDistance * scale * powf(2.0f, exponent);
	}

	inline f32 get_largest_active_cascade_distance(const State& in_state)
	{
		return get_cascade_distance(in_state, get_active_cascade_count(in_state) - 1);
	}

	inline void get_frustum_slice_corners(
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

	inline Object* get_valid_shadow_sun(State& in_state)
	{
		if (!in_state.scene.primary_sun_id.has_value())
		{
			return nullptr;
		}

		i32 primary_sun_id = in_state.scene.primary_sun_id.value();
		auto found = in_state.scene.objects.find(primary_sun_id);
		if (found == in_state.scene.objects.end())
		{
			return nullptr;
		}

		Object& sun_object = found->second;
		if (!sun_object.visibility || !sun_object.has_light
			|| sun_object.light.type != LightType::Sun || !sun_object.light.sun.cast_shadows)
		{
			return nullptr;
		}

		return &sun_object;
	}

	inline VkPipeline create_pipeline(VulkanContext* ctx, const char* in_vertex_shader_path, bool in_skinned)
	{
		VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, in_vertex_shader_path);
		VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/shadow_depth.frag.spv");

		VkPipelineShaderStageCreateInfo shader_stages[] = {
			{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_VERTEX_BIT,
				.module = vertex_module,
				.pName = "main",
			},
			{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
				.module = fragment_module,
				.pName = "main",
			},
		};

		VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamic_state = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = 2,
			.pDynamicStates = dynamic_states,
		};

		VkVertexInputBindingDescription vertex_bindings[] = {
			{ .binding = 0, .stride = sizeof(Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
			{ .binding = 1, .stride = sizeof(SkinnedVertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
		};
		VkVertexInputAttributeDescription vertex_attributes[] = {
			{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, position) },
			{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, normal) },
			{ .location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, texcoord) },
			{ .location = 3, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(SkinnedVertex, joint_indices) },
			{ .location = 4, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(SkinnedVertex, joint_weights) },
		};
		VkPipelineVertexInputStateCreateInfo vertex_input = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = in_skinned ? 2u : 1u,
			.pVertexBindingDescriptions = vertex_bindings,
			.vertexAttributeDescriptionCount = in_skinned ? 5u : 3u,
			.pVertexAttributeDescriptions = vertex_attributes,
		};

		VkPipelineInputAssemblyStateCreateInfo input_assembly = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};
		VkPipelineViewportStateCreateInfo viewport = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.scissorCount = 1,
		};
		VkPipelineRasterizationStateCreateInfo rasterization = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_NONE,
			.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
			.lineWidth = 1.0f,
		};
		VkPipelineMultisampleStateCreateInfo multisampling = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		};
		VkPipelineDepthStencilStateCreateInfo depth_stencil = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable = VK_TRUE,
			.depthWriteEnable = VK_TRUE,
			.depthCompareOp = Render::DEPTH_COMPARE_OP,
		};
		VkPipelineColorBlendAttachmentState blend_attachment = {
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
							| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
		};
		VkPipelineColorBlendStateCreateInfo color_blending = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = &blend_attachment,
		};

		VkFormat moments_format = Render::SHADOW_MOMENTS_FORMAT;
		VkPipelineRenderingCreateInfo rendering_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = 1,
			.pColorAttachmentFormats = &moments_format,
			.depthAttachmentFormat = Render::SCENE_DEPTH_FORMAT,
		};

		VkGraphicsPipelineCreateInfo pipeline_create_info = {
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.pNext = &rendering_create_info,
			.stageCount = 2,
			.pStages = shader_stages,
			.pVertexInputState = &vertex_input,
			.pInputAssemblyState = &input_assembly,
			.pViewportState = &viewport,
			.pRasterizationState = &rasterization,
			.pMultisampleState = &multisampling,
			.pDepthStencilState = &depth_stencil,
			.pColorBlendState = &color_blending,
			.pDynamicState = &dynamic_state,
			.layout = pipeline_layout,
			.renderPass = VK_NULL_HANDLE,
		};

		VkPipeline out_pipeline = VK_NULL_HANDLE;
		VK_CHECK(vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &out_pipeline));

		vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
		vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
		return out_pipeline;
	}

	inline void init(VulkanContext* ctx)
	{
		VkPushConstantRange push_constant_range = {
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
			.offset = 0,
			.size = sizeof(PushConstants),
		};
		VkPipelineLayoutCreateInfo layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &frame_data.per_frame_layout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_constant_range,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &pipeline_layout));

		pipeline = create_pipeline(ctx, "bin/shaders/shadow_depth.vert.spv", /*in_skinned*/ false);
		skinned_pipeline = create_pipeline(ctx, "bin/shaders/shadow_depth_skinned.vert.spv", /*in_skinned*/ true);
	}

	// Computes all cascade light view-projections on the CPU (port of game/
	// shadow_depth_pass.h:296-408, both placement modes). Must run before the
	// lighting fs_params upload each frame — the matrices feed both the shadow
	// draw and the lighting shader's receiver reprojection. Returns true when
	// the matrices changed and the shadow map should re-render this frame;
	// under depth_freeze the previous matrices are kept (stale map stays
	// consistent with them) unless force_recapture is set.
	inline bool compute_cascade_matrices(State& in_state, const Camera& in_camera)
	{
		if (in_state.shadow.rendering_enable && in_state.shadow.depth_freeze
			&& has_valid_shadow_map && !in_state.shadow.force_recapture)
		{
			return false;
		}

		for (i32 i = 0; i < MAX_SHADOW_CASCADES; ++i)
		{
			shadow_view_projections[i] = HMM_M4D(1.0f);
			cascade_distances[i] = 0.0f;
		}
		has_valid_shadow_map = false;

		if (!in_state.shadow.rendering_enable)
		{
			return false;
		}

		Object* sun_object = get_valid_shadow_sun(in_state);
		if (!sun_object)
		{
			return false;
		}

		Transform transform = sun_object->current_transform;
		HMM_Vec3 sun_dir = HMM_NormV3(HMM_RotateV3Q(HMM_V3(0, 0, -1), transform.rotation));

		HMM_Vec3 light_up = HMM_V3(0.0f, 0.0f, 1.0f);
		if (fabsf(HMM_DotV3(sun_dir, light_up)) > 0.99f)
		{
			light_up = HMM_V3(0.0f, 1.0f, 0.0f);
		}

		const f32 fov = HMM_AngleDeg(60.0f);
		const f32 aspect_ratio = (f32) in_state.window.render_width / (f32) in_state.window.render_height;

		const i32 cascade_count = get_active_cascade_count(in_state);
		for (i32 cascade_idx = 0; cascade_idx < cascade_count; ++cascade_idx)
		{
			// Centered squares: axis-aligned ortho squares around a point
			// ahead of the camera (game/ shadow_depth_pass.h:306-333)
			if (in_state.shadow.cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares)
			{
				const f32 cascade_half_extent = get_cascade_distance(in_state, cascade_idx);
				const f32 largest_half_extent = get_largest_active_cascade_distance(in_state);
				const f32 light_depth_range = fmaxf(100.0f, largest_half_extent * 4.0f);
				HMM_Vec3 light_pos = in_state.shadow.centered_square_center - sun_dir * (light_depth_range * 0.5f);
				HMM_Mat4 light_view = HMM_LookAt_RH(light_pos, in_state.shadow.centered_square_center, light_up);
				HMM_Mat4 light_proj = mat4_orthographic(
					-cascade_half_extent,
					cascade_half_extent,
					-cascade_half_extent,
					cascade_half_extent,
					0.01f,
					light_depth_range
				);

				shadow_view_projections[cascade_idx] = HMM_MulM4(light_proj, light_view);
				cascade_distances[cascade_idx] = cascade_half_extent;
				has_valid_shadow_map = true;
				continue;
			}

			// Frustum-slice fit
			const f32 cascade_near_distance = cascade_idx == 0 ? 0.01f : get_cascade_distance(in_state, cascade_idx - 1);
			const f32 cascade_far_distance = get_cascade_distance(in_state, cascade_idx);

			HMM_Vec3 frustum_corners[8];
			get_frustum_slice_corners(in_camera, cascade_near_distance, cascade_far_distance, fov, aspect_ratio, frustum_corners);

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

			f32 min_x = FLT_MAX, max_x = -FLT_MAX;
			f32 min_y = FLT_MAX, max_y = -FLT_MAX;
			f32 min_z = FLT_MAX, max_z = -FLT_MAX;
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

			shadow_view_projections[cascade_idx] = HMM_MulM4(light_proj, light_view);
			cascade_distances[cascade_idx] = cascade_far_distance;
			has_valid_shadow_map = true;
		}

		return has_valid_shadow_map;
	}

	// Draws shadow casters for one cascade using the matrix stored by
	// compute_cascade_matrices. Runs inside the Array pass execute callback;
	// pass_idx = cascade.
	inline void render_cascade(VulkanContext* ctx, State& in_state, i32 in_cascade_idx)
	{
		if (!has_valid_shadow_map || in_cascade_idx >= get_active_cascade_count(in_state))
		{
			return;
		}

		const HMM_Mat4& light_view_proj = shadow_view_projections[in_cascade_idx];

		// Cull + draw casters
		CullResult cull_result = cull_objects(in_state, light_view_proj, 0.0f);

		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];
		bound_pipeline = VK_NULL_HANDLE;

		vkCmdBindDescriptorSets(
			command_buffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipeline_layout,
			0, 1, &frame_data.per_frame_sets[ctx->frame_index],
			0, nullptr
		);

		for (i32 object_id : cull_result.object_ids)
		{
			auto found = in_state.scene.objects.find(object_id);
			if (found == in_state.scene.objects.end())
			{
				continue;
			}

			Object& object = found->second;
			if (object.render_object_index < 0)
			{
				continue;
			}

			Mesh& mesh = object.mesh;
			const bool skinned = mesh.has_skinned_vertices;
			if (skinned && mesh.skin_matrix_arena_offset < 0)
			{
				continue;
			}

			VkPipeline wanted_pipeline = skinned ? skinned_pipeline : pipeline;
			if (bound_pipeline != wanted_pipeline)
			{
				vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted_pipeline);
				bound_pipeline = wanted_pipeline;
			}

			PushConstants push_constants = {
				.light_view_projection = light_view_proj,
				.object_index = object.render_object_index,
				.skin_matrix_offset = skinned ? mesh.skin_matrix_arena_offset : -1,
			};
			vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push_constants), &push_constants);

			VkBuffer vertex_buffer = mesh.vertex_buffer.get_gpu_buffer();
			VkDeviceSize vertex_offset = 0;
			vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer, &vertex_offset);
			if (skinned)
			{
				VkBuffer skinned_vertex_buffer = mesh.skinned_vertex_buffer.get_gpu_buffer();
				VkDeviceSize skinned_offset = 0;
				vkCmdBindVertexBuffers(command_buffer, 1, 1, &skinned_vertex_buffer, &skinned_offset);
			}
			vkCmdBindIndexBuffer(command_buffer, mesh.index_buffer.get_gpu_buffer(), 0, VK_INDEX_TYPE_UINT32);
			vkCmdDrawIndexed(command_buffer, mesh.index_count, 1, 0, 0, 0);
		}
	}

	inline void shutdown(VulkanContext* ctx)
	{
		vkDestroyPipeline(ctx->device, skinned_pipeline, nullptr);
		vkDestroyPipeline(ctx->device, pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, pipeline_layout, nullptr);
	}
}
