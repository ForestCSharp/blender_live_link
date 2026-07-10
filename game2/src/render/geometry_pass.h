#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/shader_module.h"
#include "render/frame_data.h"
#include "game_object/mesh.h"

// Deferred geometry pass: writes the 4-attachment G-buffer (see
// geometry.frag for the layout). Same descriptor set 0 (layout A) and push
// constants as the old forward pass; materials/bindless textures are baked
// into the G-buffer here and consumed by the lighting pass.

struct GeometryPassPushConstants
{
	i32 object_index;
	i32 skin_matrix_offset;	// arena offset for skinned draws; ignored otherwise
};
static_assert(sizeof(GeometryPassPushConstants) == 8, "Push constants: object index + skin matrix arena offset");

struct GeometryPass
{
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkPipeline skinned_pipeline = VK_NULL_HANDLE;

	// Bind-on-change tracker, reset each pass begin
	VkPipeline bound_pipeline = VK_NULL_HANDLE;
};

static GeometryPass geometry_pass;

// Builds one geometry pipeline variant (static or skinned vertex input)
static VkPipeline geometry_pass_create_pipeline(VulkanContext* ctx, const char* in_vertex_shader_path, bool in_skinned)
{
	VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, in_vertex_shader_path);
	VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/geometry.frag.spv");

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

	VkVertexInputBindingDescription vertex_bindings[] = {
		{
			.binding = 0,
			.stride = sizeof(Vertex),
			.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		},
		{
			.binding = 1,
			.stride = sizeof(SkinnedVertex),
			.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		},
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
		.primitiveRestartEnable = VK_FALSE,
	};

	VkPipelineViewportStateCreateInfo viewport = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};

	// CULL_NONE: game/ parity (geometry_pass.h uses SG_CULLMODE_NONE)
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

	VkPipelineColorBlendAttachmentState color_blend_attachments[Render::GBUFFER_OUTPUT_COUNT];
	for (i32 attachment_idx = 0; attachment_idx < Render::GBUFFER_OUTPUT_COUNT; ++attachment_idx)
	{
		color_blend_attachments[attachment_idx] = (VkPipelineColorBlendAttachmentState) {
			.blendEnable = VK_FALSE,
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT
							| VK_COLOR_COMPONENT_G_BIT
							| VK_COLOR_COMPONENT_B_BIT
							| VK_COLOR_COMPONENT_A_BIT,
		};
	}

	VkPipelineColorBlendStateCreateInfo color_blending = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = Render::GBUFFER_OUTPUT_COUNT,
		.pAttachments = color_blend_attachments,
	};

	VkFormat color_formats[Render::GBUFFER_OUTPUT_COUNT];
	for (i32 format_idx = 0; format_idx < Render::GBUFFER_OUTPUT_COUNT; ++format_idx)
	{
		color_formats[format_idx] = Render::GBUFFER_FORMAT;
	}

	VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = Render::GBUFFER_OUTPUT_COUNT,
		.pColorAttachmentFormats = color_formats,
		.depthAttachmentFormat = Render::SCENE_DEPTH_FORMAT,
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
		.layout = geometry_pass.pipeline_layout,
		.renderPass = VK_NULL_HANDLE,
	};

	VkPipeline pipeline = VK_NULL_HANDLE;
	VK_CHECK(vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &pipeline));

	vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
	vkDestroyShaderModule(ctx->device, fragment_module, nullptr);

	return pipeline;
}

void geometry_pass_init(VulkanContext* ctx)
{
	VkPushConstantRange push_constant_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		.offset = 0,
		.size = sizeof(GeometryPassPushConstants),
	};

	VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &frame_data.per_frame_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range,
	};

	VK_CHECK(vkCreatePipelineLayout(ctx->device, &pipeline_layout_create_info, nullptr, &geometry_pass.pipeline_layout));

	geometry_pass.pipeline = geometry_pass_create_pipeline(ctx, "bin/shaders/geometry.vert.spv", /*in_skinned*/ false);
	geometry_pass.skinned_pipeline = geometry_pass_create_pipeline(ctx, "bin/shaders/geometry_skinned.vert.spv", /*in_skinned*/ true);
}

void geometry_pass_bind(VulkanContext* ctx)
{
	VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];

	geometry_pass.bound_pipeline = VK_NULL_HANDLE;

	vkCmdBindDescriptorSets(
		command_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		geometry_pass.pipeline_layout,
		0, 1, &frame_data.per_frame_sets[ctx->frame_index],
		0, nullptr
	);
}

// Lazy GPU buffer creation happens here, on the main thread
void geometry_pass_draw_mesh(VulkanContext* ctx, Mesh& in_mesh, i32 in_object_index)
{
	VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];

	const bool skinned = in_mesh.has_skinned_vertices;
	if (skinned && in_mesh.skin_matrix_arena_offset < 0)
	{
		return;
	}

	VkPipeline wanted_pipeline = skinned ? geometry_pass.skinned_pipeline : geometry_pass.pipeline;
	if (geometry_pass.bound_pipeline != wanted_pipeline)
	{
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted_pipeline);
		geometry_pass.bound_pipeline = wanted_pipeline;
	}

	GeometryPassPushConstants push_constants = {
		.object_index = in_object_index,
		.skin_matrix_offset = skinned ? in_mesh.skin_matrix_arena_offset : -1,
	};

	vkCmdPushConstants(
		command_buffer,
		geometry_pass.pipeline_layout,
		VK_SHADER_STAGE_VERTEX_BIT,
		0,
		sizeof(push_constants),
		&push_constants
	);

	VkBuffer vertex_buffer = in_mesh.vertex_buffer.get_gpu_buffer();
	VkDeviceSize vertex_buffer_offset = 0;
	vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer, &vertex_buffer_offset);

	if (skinned)
	{
		VkBuffer skinned_vertex_buffer = in_mesh.skinned_vertex_buffer.get_gpu_buffer();
		VkDeviceSize skinned_offset = 0;
		vkCmdBindVertexBuffers(command_buffer, 1, 1, &skinned_vertex_buffer, &skinned_offset);
	}

	vkCmdBindIndexBuffer(command_buffer, in_mesh.index_buffer.get_gpu_buffer(), 0, VK_INDEX_TYPE_UINT32);

	vkCmdDrawIndexed(command_buffer, in_mesh.index_count, 1, 0, 0, 0);
}

void geometry_pass_shutdown(VulkanContext* ctx)
{
	vkDestroyPipeline(ctx->device, geometry_pass.skinned_pipeline, nullptr);
	vkDestroyPipeline(ctx->device, geometry_pass.pipeline, nullptr);
	vkDestroyPipelineLayout(ctx->device, geometry_pass.pipeline_layout, nullptr);
}
