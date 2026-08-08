#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/shader_module.h"
#include "render/frame_data.h"

// First upscales scene color into a full-resolution float composite target,
// then performs the sole display encoding step into the swapchain.

struct CopyToSwapchainPass
{
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline presentation_pipeline = VK_NULL_HANDLE;
	VkPipeline swapchain_pipeline = VK_NULL_HANDLE;
	PerFrameDescriptorSets presentation_input_sets;
};

struct CopyToSwapchainPushConstants
{
	i32 output_mode; // DISPLAY_OUTPUT_MODE_* / EDisplayOutputMode.
	i32 sdr_attachment_is_srgb;
};
static_assert(sizeof(CopyToSwapchainPushConstants) == sizeof(i32) * 2);

static CopyToSwapchainPass copy_to_swapchain_pass;

void copy_to_swapchain_pass_init(VulkanContext* ctx)
{
	VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, "bin/shaders/copy_to_swapchain.vert.spv");
	VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/copy_to_swapchain.frag.spv");

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

	VkDynamicState dynamic_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};

	VkPipelineDynamicStateCreateInfo dynamic_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = sizeof(dynamic_states) / sizeof(dynamic_states[0]),
		.pDynamicStates = dynamic_states,
	};

	// Fullscreen triangle: no vertex input
	VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};

	VkPipelineInputAssemblyStateCreateInfo input_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		.primitiveRestartEnable = VK_FALSE,
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

	// No depth attachment, no depth test
	VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_FALSE,
		.depthWriteEnable = VK_FALSE,
	};

	VkPipelineColorBlendAttachmentState color_blend_attachment = {
		.blendEnable = VK_FALSE,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT
						| VK_COLOR_COMPONENT_G_BIT
						| VK_COLOR_COMPONENT_B_BIT
						| VK_COLOR_COMPONENT_A_BIT,
	};

	VkPipelineColorBlendStateCreateInfo color_blending = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &color_blend_attachment,
	};

	VkPushConstantRange push_constant_range = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(CopyToSwapchainPushConstants),
	};
	VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &frame_data.sampled_input_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range,
	};

	VK_CHECK(vkCreatePipelineLayout(ctx->device, &pipeline_layout_create_info, nullptr, &copy_to_swapchain_pass.pipeline_layout));
	copy_to_swapchain_pass.presentation_input_sets.init_persistent(ctx, frame_data.sampled_input_layout);

	VkFormat output_format = Render::SCENE_COLOR_FORMAT;
	VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &output_format,
	};

	VkGraphicsPipelineCreateInfo pipeline_create_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &pipeline_rendering_create_info,
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
		.layout = copy_to_swapchain_pass.pipeline_layout,
		.renderPass = VK_NULL_HANDLE,
	};

	VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_create_info, &copy_to_swapchain_pass.presentation_pipeline));
	output_format = ctx->surface_format.format;
	VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_create_info, &copy_to_swapchain_pass.swapchain_pipeline));

	vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
	vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
}

void copy_to_swapchain_pass_update_presentation_input(VulkanContext* ctx, VkImageView in_view)
{
	VkDescriptorImageInfo image_info = {
		.sampler = frame_data.linear_sampler,
		.imageView = in_view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = copy_to_swapchain_pass.presentation_input_sets.current(ctx),
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &image_info,
	};
	vulkan_update_descriptor_sets(ctx, 1, &write);
}

void copy_to_swapchain_pass_draw_presentation(VulkanContext* ctx)
{
	VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, copy_to_swapchain_pass.presentation_pipeline);
	vkCmdBindDescriptorSets(
		command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
		copy_to_swapchain_pass.pipeline_layout,
		0, 1, &copy_to_swapchain_pass.presentation_input_sets.current(ctx),
		0, nullptr);
	const CopyToSwapchainPushConstants constants = {
		.output_mode = DISPLAY_OUTPUT_MODE_PRESENTATION,
		.sdr_attachment_is_srgb = 0,
	};
	vkCmdPushConstants(
		command_buffer, copy_to_swapchain_pass.pipeline_layout,
		VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
	vulkan_cmd_draw(ctx, 3, 1, 0, 0);
}

void copy_to_swapchain_pass_draw(VulkanContext* ctx)
{
	VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, copy_to_swapchain_pass.swapchain_pipeline);

	vkCmdBindDescriptorSets(
		command_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		copy_to_swapchain_pass.pipeline_layout,
		0, 1, &frame_data.copy_input_sets[ctx->frame_index],
		0, nullptr
	);
	const CopyToSwapchainPushConstants constants = {
		.output_mode = (i32)ctx->active_output_mode,
		.sdr_attachment_is_srgb = (
			ctx->surface_format.format == VK_FORMAT_B8G8R8A8_SRGB
			|| ctx->surface_format.format == VK_FORMAT_R8G8B8A8_SRGB) ? 1 : 0,
	};
	vkCmdPushConstants(
		command_buffer, copy_to_swapchain_pass.pipeline_layout,
		VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);

	vulkan_cmd_draw(ctx, 3, 1, 0, 0);
}

void copy_to_swapchain_pass_shutdown(VulkanContext* ctx)
{
	vkDestroyPipeline(ctx->device, copy_to_swapchain_pass.presentation_pipeline, nullptr);
	vkDestroyPipeline(ctx->device, copy_to_swapchain_pass.swapchain_pipeline, nullptr);
	vkDestroyPipelineLayout(ctx->device, copy_to_swapchain_pass.pipeline_layout, nullptr);
}
