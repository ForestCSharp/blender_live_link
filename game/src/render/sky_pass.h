#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/shader_module.h"
#include "render/frame_data.h"
#include "render/bruneton_atmosphere_pass.h"
#include "render/solar_calibration.h"
#include "state/state.h"

// The physical LUTs are the only sky cache. Visible pixels and GI capture rays
// evaluate them at their real world-space observer positions.
struct SkyCompositePushConstants
{
	HMM_Vec4 sun_direction_and_cos_radius;
	HMM_Vec4 sun_tint_and_disc_intensity;
	HMM_Vec4 planet_center_z_and_sky_intensity;
};

struct SkyPass
{
	VkPipelineLayout composite_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline composite_pipeline = VK_NULL_HANDLE;

	BrunetonProbeSkySignature last_probe_signature = {};
	SkyAtmosphere active_parameters = {};
	HMM_Vec3 active_sun_direction = HMM_V3(0.0f, 0.0f, 1.0f);
	HMM_Vec3 active_sun_color = HMM_V3(0.0f, 0.0f, 0.0f);
	HMM_Vec3 active_sun_tint = HMM_V3(0.0f, 0.0f, 0.0f);
	f32 active_sun_irradiance_w_m2 = 0.0f;
	bool has_active_atmosphere = false;
	bool has_probe_signature = false;
};

static SkyPass sky_pass;

inline void sky_pass_init(VulkanContext* ctx)
{
	bruneton_atmosphere_pass.init(ctx);

	VkDescriptorSetLayout set_layouts[] = {
		frame_data.per_frame_layout,
		bruneton_atmosphere_pass.descriptors.layout,
	};
	VkPushConstantRange push_range = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(SkyCompositePushConstants),
	};
	VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 2,
		.pSetLayouts = set_layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};
	VK_CHECK(vkCreatePipelineLayout(
		ctx->device, &layout_info, nullptr, &sky_pass.composite_pipeline_layout));

	VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dynamic_states,
	};
	VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
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
	VkPipelineColorBlendAttachmentState blend_attachments[Render::GBUFFER_OUTPUT_COUNT] = {};
	VkFormat color_formats[Render::GBUFFER_OUTPUT_COUNT] = {};
	for (i32 idx = 0; idx < Render::GBUFFER_OUTPUT_COUNT; ++idx)
	{
		blend_attachments[idx].colorWriteMask = VK_COLOR_COMPONENT_R_BIT
			| VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		color_formats[idx] = Render::GBUFFER_FORMAT;
	}
	VkPipelineColorBlendStateCreateInfo color_blending = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = Render::GBUFFER_OUTPUT_COUNT,
		.pAttachments = blend_attachments,
	};
	VkPipelineRenderingCreateInfo rendering_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = Render::GBUFFER_OUTPUT_COUNT,
		.pColorAttachmentFormats = color_formats,
		.depthAttachmentFormat = Render::SCENE_DEPTH_FORMAT,
	};
	VkShaderModule vertex_module =
		create_shader_module_from_file(ctx->device, "bin/shaders/sky.vert.spv");
	VkShaderModule fragment_module =
		create_shader_module_from_file(ctx->device, "bin/shaders/sky.frag.spv");
	VkPipelineShaderStageCreateInfo stages[] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex_module, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment_module, .pName = "main" },
	};
	VkGraphicsPipelineCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &rendering_info,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &vertex_input,
		.pInputAssemblyState = &input_assembly,
		.pViewportState = &viewport,
		.pRasterizationState = &rasterization,
		.pMultisampleState = &multisampling,
		.pDepthStencilState = &depth_stencil,
		.pColorBlendState = &color_blending,
		.pDynamicState = &dynamic_state,
		.layout = sky_pass.composite_pipeline_layout,
	};
	VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_info, &sky_pass.composite_pipeline));
	vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
	vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
}

// Updates the active controller and records a full LUT precompute only when
// physical atmosphere inputs changed. Camera motion is intentionally absent.
inline bool sky_pass_update_atmosphere(
	FrameRenderGraph& graph, VulkanContext* ctx, State& state)
{
	if (!state.scene.active_sky_controller_id)
	{
		if (sky_pass.has_active_atmosphere && state.gi.render_sky_to_probes)
			state.gi.is_updating = true;
		sky_pass.has_active_atmosphere = false;
		sky_pass.has_probe_signature = false;
		return false;
	}

	const i32 controller_id = *state.scene.active_sky_controller_id;
	auto found = state.scene.objects.find(controller_id);
	if (found == state.scene.objects.end()
		|| !found->second.has_sky_atmosphere
		|| !object_is_sun_light(found->second))
	{
		if (sky_pass.has_active_atmosphere && state.gi.render_sky_to_probes)
			state.gi.is_updating = true;
		sky_pass.has_active_atmosphere = false;
		sky_pass.has_probe_signature = false;
		return false;
	}

	Object& sun = found->second;
	const SkyAtmosphere& atmosphere = sun.sky_atmosphere;
	const HMM_Vec3 sun_direction = -HMM_NormV3(HMM_RotateV3Q(
		HMM_V3(0.0f, 0.0f, -1.0f), sun.current_transform.rotation));
	const HMM_Vec3 sun_color = sun.light.color
		* solar_irradiance_scale(sun.light.sun.power);

	bruneton_atmosphere_pass.update(ctx, atmosphere);
	bruneton_atmosphere_pass.precompute_if_needed(graph, ctx, atmosphere);

	const BrunetonProbeSkySignature probe_signature = {
		.controller_id = controller_id,
		.sun_direction = sun_direction,
		.sun_color_energy = sun_color,
		.sky_intensity = atmosphere.sky_intensity,
		.planet_center_z_m = atmosphere.planet_center_z_m,
		.lut_generation = bruneton_atmosphere_pass.precompute_count,
	};
	if ((!sky_pass.has_probe_signature
			|| !bruneton_probe_sky_signature_equal(
				probe_signature, sky_pass.last_probe_signature))
		&& state.gi.render_sky_to_probes)
	{
		state.gi.is_updating = true;
	}

	sky_pass.last_probe_signature = probe_signature;
	sky_pass.has_probe_signature = true;
	sky_pass.active_parameters = atmosphere;
	sky_pass.active_sun_direction = sun_direction;
	sky_pass.active_sun_color = sun_color;
	sky_pass.active_sun_tint = sun.light.color;
	sky_pass.active_sun_irradiance_w_m2 = sun.light.sun.power;
	sky_pass.has_active_atmosphere = true;
	return true;
}

inline void sky_pass_draw_composite(VulkanContext* ctx)
{
	if (!sky_pass.has_active_atmosphere || !bruneton_atmosphere_pass.has_precomputed)
		return;

	const i32 timing_slot = gpu_timestamps_begin_scope(ctx, "Sky Direct LUT");
	VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
	vkCmdBindPipeline(
		command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pass.composite_pipeline);
	VkDescriptorSet sets[] = {
		frame_data.per_frame_sets[ctx->frame_index],
		bruneton_atmosphere_pass.descriptors.current(ctx),
	};
	vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
		sky_pass.composite_pipeline_layout, 0, 2, sets, 0, nullptr);
	const f32 radius_radians = HMM_AngleDeg(
		sky_pass.active_parameters.sun_disc_angular_diameter_degrees * 0.5f);
	SkyCompositePushConstants push = {
		.sun_direction_and_cos_radius =
			HMM_V4V(sky_pass.active_sun_direction, cosf(radius_radians)),
		.sun_tint_and_disc_intensity = HMM_V4V(
			sky_pass.active_sun_color, sky_pass.active_parameters.sun_disc_intensity),
		.planet_center_z_and_sky_intensity = HMM_V4(
			sky_pass.active_parameters.planet_center_z_m,
			sky_pass.active_parameters.sky_intensity, 0.0f, 0.0f),
	};
	vkCmdPushConstants(command_buffer, sky_pass.composite_pipeline_layout,
		VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	vulkan_cmd_draw(ctx, 3, 1, 0, 0);
	gpu_timestamps_end_scope(ctx, timing_slot);
}

inline void sky_pass_shutdown(VulkanContext* ctx)
{
	vkDestroyPipeline(ctx->device, sky_pass.composite_pipeline, nullptr);
	vkDestroyPipelineLayout(ctx->device, sky_pass.composite_pipeline_layout, nullptr);
	bruneton_atmosphere_pass.shutdown(ctx);
}
