#pragma once

#include <cstring>

#include "render/core/frame_render_graph.h"
#include "render/core/render_pass.h"
#include "render/passes/shadows/shadow_depth_pass.h"
#include "state/state.h"

// GI sun shadow map: one orthographic EVSM layer covering the whole GI scene.
// The main shadow cascades follow the camera, so probe lighting far from it
// had no sun shadows (probes saw every surface as sunlit). Casters are static
// GI geometry only, so moving objects never invalidate it; it re-renders only
// when the sun direction or the GI scene bounds change.
namespace GIShadowMap
{
	static constexpr i32 resolution = 2048;

	inline RenderPass pass;
	inline HMM_Mat4 view_projection = HMM_M4D(1.0f);
	inline bool valid = false;
	inline HMM_Vec3 rendered_sun_direction = {};
	inline BoundingBox rendered_bounds = {};
	inline DynamicArray<i32> visible_scratch;

	inline void init()
	{
		// shortcut: 2 layers only so the sampled view is 2D_ARRAY, matching
		// lighting.frag's sampler2DArray; layer 1 stays cleared (fully lit).
		pass.init((RenderPassDesc) {
			.pass_count = 2,
			.num_outputs = 1,
			.outputs = {
				{
					.format = Render::SHADOW_MOMENTS_FORMAT,
					.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
					.store_op = VK_ATTACHMENT_STORE_OP_STORE,
					.clear_value = {{{ 1.0f, 1.0f, 0.0f, 0.0f }}},	// fully lit EVSM moments
				},
			},
			.depth_output = {
				.format = Render::SCENE_DEPTH_FORMAT,
				.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.clear_value = { .depthStencil = { .depth = Render::DEPTH_CLEAR_VALUE } },
			},
			.extent = render_target_extent_fixed(resolution, resolution),
			.type = ERenderPassType::Array,
			.debug_label = "GI Sun Shadow",
		});
	}

	inline void cleanup()
	{
		pass.cleanup();
		valid = false;
	}

	inline bool bounds_equal(const BoundingBox& in_a, const BoundingBox& in_b)
	{
		return memcmp(&in_a, &in_b, sizeof(BoundingBox)) == 0;
	}

	// Re-renders when the sun direction or scene bounds changed (or in_force).
	// Returns true when the map changed, which means probe lighting is stale.
	inline bool update(FrameRenderGraph& graph, VulkanContext* ctx, State& in_state,
		const BoundingBox& in_scene_bounds, const bool in_force)
	{
		Object* sun = ShadowDepthPass::get_valid_shadow_sun(in_state);
		if (!sun)
		{
			const bool changed = valid;
			valid = false;
			return changed;
		}

		const HMM_Vec3 sun_direction = HMM_NormV3(HMM_RotateV3Q(HMM_V3(0, 0, -1), sun->current_transform.rotation));
		if (!in_force && valid
			&& HMM_DotV3(sun_direction, rendered_sun_direction) > 0.999999f
			&& bounds_equal(in_scene_bounds, rendered_bounds))
		{
			return false;
		}

		// Orthographic box around the scene's bounding sphere, seen from the sun.
		const HMM_Vec3 center = (in_scene_bounds.min + in_scene_bounds.max) * 0.5f;
		const f32 radius = std::max(HMM_LenV3(in_scene_bounds.max - in_scene_bounds.min) * 0.5f, 0.01f);
		HMM_Vec3 light_up = HMM_V3(0.0f, 0.0f, 1.0f);
		if (fabsf(HMM_DotV3(sun_direction, light_up)) > 0.99f)
		{
			light_up = HMM_V3(0.0f, 1.0f, 0.0f);
		}
		const HMM_Vec3 light_position = center - sun_direction * (radius * 2.0f);
		const HMM_Mat4 light_view = HMM_LookAt_RH(light_position, center, light_up);
		const HMM_Mat4 light_projection = ShadowDepthPass::mat4_orthographic(
			-radius, radius, -radius, radius, 0.01f, radius * 4.0f);
		view_projection = HMM_MulM4(light_projection, light_view);

		graph.execute(pass, [&](const i32 in_layer)
		{
			if (in_layer != 0)
			{
				return;
			}
			cull_objects(in_state, view_projection,
				in_state.tessellation.enabled ? in_state.tessellation.bounds_padding : 0.0f,
				visible_scratch);
			VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
			vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
				ShadowDepthPass::pipeline_layout, 0, 1, &frame_data.per_frame_sets[ctx->frame_index], 0, nullptr);
			const ShadowDepthPass::PushConstants push_constants = { .light_view_projection = view_projection };
			vulkan_cmd_push_constants(ctx, ShadowDepthPass::pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
				0, sizeof(push_constants), &push_constants);
			MeshDrawBindings bindings;
			for (const i32 render_object_index : visible_scratch)
			{
				auto found = in_state.scene.objects.find(in_state.cull_entries[render_object_index].object_id);
				if (found == in_state.scene.objects.end() || !object_contributes_to_gi_scene(found->second))
				{
					continue;
				}
				draw_indexed_mesh(ctx, found->second.mesh, render_object_index,
					ShadowDepthPass::pipeline, ShadowDepthPass::skinned_pipeline, bindings);
			}
		});
		graph.make_sampled(pass);

		printf("GI sun shadow map updated (radius %.1f)\n", radius);
		valid = true;
		rendered_sun_direction = sun_direction;
		rendered_bounds = in_scene_bounds;
		return true;
	}
}
