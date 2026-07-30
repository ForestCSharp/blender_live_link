#pragma once

#include "core/types.h"
#include "render/shader_module.h"
#include "render/vulkan_context.h"

#include <cassert>
#include <vector>

struct FullscreenPipelineDesc
{
	const char* vertex_shader_path = nullptr;
	const char* fragment_shader_path = nullptr;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	const VkFormat* color_formats = nullptr;
	u32 color_format_count = 0;
};

inline VkPipeline vulkan_create_fullscreen_pipeline(
	VulkanContext* ctx,
	const FullscreenPipelineDesc& in_desc)
{
	assert(in_desc.vertex_shader_path);
	assert(in_desc.fragment_shader_path);
	assert(in_desc.pipeline_layout != VK_NULL_HANDLE);
	assert(in_desc.color_formats);
	assert(in_desc.color_format_count > 0);

	VkShaderModule vertex_module =
		create_shader_module_from_file(ctx->device, in_desc.vertex_shader_path);
	VkShaderModule fragment_module =
		create_shader_module_from_file(ctx->device, in_desc.fragment_shader_path);
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
	};
	std::vector<VkPipelineColorBlendAttachmentState> blend_attachments(
		in_desc.color_format_count,
		{
			.blendEnable = VK_FALSE,
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
							| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
		}
	);
	VkPipelineColorBlendStateCreateInfo color_blending = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = in_desc.color_format_count,
		.pAttachments = blend_attachments.data(),
	};
	VkPipelineRenderingCreateInfo rendering = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = in_desc.color_format_count,
		.pColorAttachmentFormats = in_desc.color_formats,
	};
	VkGraphicsPipelineCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &rendering,
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
		.layout = in_desc.pipeline_layout,
		.renderPass = VK_NULL_HANDLE,
	};

	VkPipeline pipeline = VK_NULL_HANDLE;
	VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_info, &pipeline));
	vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
	vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
	return pipeline;
}
