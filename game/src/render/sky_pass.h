#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/shader_module.h"
#include "render/frame_data.h"
#include "render/render_pass.h"

// Sky rendering (port of game/src/render/sky_pass.h):
//  - SkyBakePass: integrates the atmosphere into a 256x256 octahedral map,
//    re-run only when the sun direction changes
//  - Sky composite: fullscreen quad at the far plane drawn INSIDE the
//    geometry pass, writing all 4 G-buffer attachments with the
//    invalid-geometry sentinel

static constexpr i32 SKY_BAKE_RESOLUTION = 256;

struct SkyPass
{
	// Bake target lives outside the ERenderPass chain (fixed size, lazily
	// executed) — game/ manages it the same way
	RenderPass bake_render_pass;
	VkPipelineLayout bake_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline bake_pipeline = VK_NULL_HANDLE;

	VkPipelineLayout composite_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline composite_pipeline = VK_NULL_HANDLE;
	VkDescriptorSet composite_input_sets[MAX_FRAMES_IN_FLIGHT] = {};

	HMM_Vec3 last_baked_sun_dir = HMM_V3(0.0f, 0.0f, 0.0f);
	bool has_baked = false;
};

static SkyPass sky_pass;

void sky_pass_init(VulkanContext* ctx)
{
	// Bake target: single RGBA32F, fixed 256x256
	sky_pass.bake_render_pass.init((RenderPassDesc) {
		.initial_width = SKY_BAKE_RESOLUTION,
		.initial_height = SKY_BAKE_RESOLUTION,
		.num_outputs = 1,
		.outputs = {
			{
				.format = VK_FORMAT_R32G32B32A32_SFLOAT,
				.load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.store_op = VK_ATTACHMENT_STORE_OP_STORE,
			},
		},
		.resize_with_window = false,
		.type = ERenderPassType::Single,
		.debug_label = "Sky Bake",
	});

	// Bake pipeline: fullscreen, push-constant sun direction, no sets
	{
		VkPushConstantRange push_constant_range = {
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			.offset = 0,
			.size = sizeof(HMM_Vec4),
		};
		VkPipelineLayoutCreateInfo layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_constant_range,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &sky_pass.bake_pipeline_layout));
	}

	// Composite input sets (layout B, baked sky) from frame_data's pool
	for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
	{
		VkDescriptorSetAllocateInfo allocate_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = frame_data.pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &frame_data.sampled_input_layout,
		};
		VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &sky_pass.composite_input_sets[frame_idx]));
	}

	// Composite pipeline layout: set 0 = per-frame (layout A), set 1 = baked sky (layout B)
	{
		VkDescriptorSetLayout set_layouts[] = {
			frame_data.per_frame_layout,
			frame_data.sampled_input_layout,
		};
		VkPipelineLayoutCreateInfo layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 2,
			.pSetLayouts = set_layouts,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &sky_pass.composite_pipeline_layout));
	}

	// Shared fixed-function state
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

	// Bake pipeline
	{
		VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, "bin/shaders/sky_bake.vert.spv");
		VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/sky_bake.frag.spv");

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

		VkPipelineDepthStencilStateCreateInfo depth_stencil = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
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
		VkFormat bake_format = VK_FORMAT_R32G32B32A32_SFLOAT;
		VkPipelineRenderingCreateInfo rendering_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = 1,
			.pColorAttachmentFormats = &bake_format,
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
			.layout = sky_pass.bake_pipeline_layout,
			.renderPass = VK_NULL_HANDLE,
		};
		VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_create_info, &sky_pass.bake_pipeline));

		vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
		vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
	}

	// Composite pipeline: 4 G-buffer MRTs, depth GREATER_EQUAL + write at
	// the far plane (fills only background pixels)
	{
		VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, "bin/shaders/sky.vert.spv");
		VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/sky.frag.spv");

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

		VkPipelineDepthStencilStateCreateInfo depth_stencil = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable = VK_TRUE,
			.depthWriteEnable = VK_TRUE,
			.depthCompareOp = Render::DEPTH_COMPARE_OP,
		};

		VkPipelineColorBlendAttachmentState blend_attachments[Render::GBUFFER_OUTPUT_COUNT];
		VkFormat color_formats[Render::GBUFFER_OUTPUT_COUNT];
		for (i32 attachment_idx = 0; attachment_idx < Render::GBUFFER_OUTPUT_COUNT; ++attachment_idx)
		{
			blend_attachments[attachment_idx] = (VkPipelineColorBlendAttachmentState) {
				.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
								| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
			};
			color_formats[attachment_idx] = Render::GBUFFER_FORMAT;
		}
		VkPipelineColorBlendStateCreateInfo color_blending = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = Render::GBUFFER_OUTPUT_COUNT,
			.pAttachments = blend_attachments,
		};
		VkPipelineRenderingCreateInfo rendering_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = Render::GBUFFER_OUTPUT_COUNT,
			.pColorAttachmentFormats = color_formats,
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
			.layout = sky_pass.composite_pipeline_layout,
			.renderPass = VK_NULL_HANDLE,
		};
		VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_create_info, &sky_pass.composite_pipeline));

		vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
		vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
	}
}

// Writes this frame's composite input set (after fence wait)
void sky_pass_update(VulkanContext* ctx)
{
	VkDescriptorImageInfo image_info = {
		.sampler = frame_data.linear_sampler,
		.imageView = sky_pass.bake_render_pass.get_color_output(0).view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = sky_pass.composite_input_sets[ctx->frame_index],
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &image_info,
	};
	vulkan_update_descriptor_sets(ctx, 1, &write);
}

// Re-bakes the octahedral sky when the sun direction changed (records into
// the current frame's command buffer, before the geometry pass samples it)
void sky_pass_bake_if_needed(VulkanContext* ctx, HMM_Vec3 in_sun_direction)
{
	const bool sun_changed = !sky_pass.has_baked
		|| HMM_LenSqrV3(in_sun_direction - sky_pass.last_baked_sun_dir) > 1.0e-8f;
	if (!sun_changed)
	{
		return;
	}

	sky_pass.bake_render_pass.execute(ctx, [&](i32)
	{
		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pass.bake_pipeline);

		HMM_Vec4 sun_dir = HMM_V4V(in_sun_direction, 0.0f);
		vkCmdPushConstants(
			command_buffer,
			sky_pass.bake_pipeline_layout,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			0, sizeof(HMM_Vec4), &sun_dir
		);
		vulkan_cmd_draw(ctx, 3, 1, 0, 0);
	});

	// Geometry pass (sky composite) samples it this same frame
	gpu_image_transition(
		ctx->command_buffers[ctx->frame_index],
		sky_pass.bake_render_pass.get_color_output(0),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	);

	sky_pass.last_baked_sun_dir = in_sun_direction;
	sky_pass.has_baked = true;
}

// Draws the sky composite quad (inside the geometry pass callback, after
// mesh draws)
void sky_pass_draw_composite(VulkanContext* ctx)
{
	if (!sky_pass.has_baked)
	{
		return;
	}

	VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pass.composite_pipeline);

	VkDescriptorSet sets[] = {
		frame_data.per_frame_sets[ctx->frame_index],
		sky_pass.composite_input_sets[ctx->frame_index],
	};
	vkCmdBindDescriptorSets(
		command_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		sky_pass.composite_pipeline_layout,
		0, 2, sets,
		0, nullptr
	);

	vulkan_cmd_draw(ctx, 3, 1, 0, 0);
}

void sky_pass_shutdown(VulkanContext* ctx)
{
	vkDestroyPipeline(ctx->device, sky_pass.composite_pipeline, nullptr);
	vkDestroyPipelineLayout(ctx->device, sky_pass.composite_pipeline_layout, nullptr);
	vkDestroyPipeline(ctx->device, sky_pass.bake_pipeline, nullptr);
	vkDestroyPipelineLayout(ctx->device, sky_pass.bake_pipeline_layout, nullptr);
	sky_pass.bake_render_pass.cleanup();
}
