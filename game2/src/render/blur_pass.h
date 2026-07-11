#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/render_pass.h"
#include "render/shader_module.h"
#include "render/frame_data.h"

// Generic separable gaussian blur over a 2D target (port of game/'s
// blur_pass.h): horizontal (intermediate pass) then vertical (final pass).
// The 2D counterpart of ShadowBlurPass. Inputs resize with the window, so
// each consumer gets per-frame descriptor sets rewritten in its update.

namespace BlurPass
{
	struct PushConstants
	{
		HMM_Vec2 screen_size;
		HMM_Vec2 direction;
		i32 blur_size;
	};
	static_assert(sizeof(PushConstants) == 20, "Must match blur.frag's push constant block");

	// One pipeline shared by all users of a given color format
	inline VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	inline VkPipeline pipeline = VK_NULL_HANDLE;
	inline VkFormat pipeline_format = VK_FORMAT_UNDEFINED;

	inline void init(VulkanContext* ctx, VkFormat in_color_format)
	{
		assert(pipeline == VK_NULL_HANDLE && "single-format blur pipeline; extend to a per-format cache when needed");
		pipeline_format = in_color_format;

		VkPushConstantRange push_constant_range = {
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			.offset = 0,
			.size = sizeof(PushConstants),
		};
		VkPipelineLayoutCreateInfo layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &frame_data.sampled_input_layout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_constant_range,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &pipeline_layout));

		VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, "bin/shaders/blur.vert.spv");
		VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/blur.frag.spv");

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
		VkPipelineColorBlendAttachmentState blend_attachment = {
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
							| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
		};
		VkPipelineColorBlendStateCreateInfo color_blending = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = &blend_attachment,
		};

		VkPipelineRenderingCreateInfo rendering_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = 1,
			.pColorAttachmentFormats = &pipeline_format,
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
		VK_CHECK(vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &pipeline));

		vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
		vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
	}

	inline void draw_blur(VulkanContext* ctx, VkDescriptorSet in_input_set, HMM_Vec2 in_screen_size, HMM_Vec2 in_direction, i32 in_blur_size)
	{
		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];

		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdBindDescriptorSets(
			command_buffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipeline_layout,
			0, 1, &in_input_set,
			0, nullptr
		);
		PushConstants push_constants = {
			.screen_size = in_screen_size,
			.direction = in_direction,
			.blur_size = in_blur_size,
		};
		vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants), &push_constants);
		vkCmdDraw(command_buffer, 3, 1, 0, 0);
	}

	// Horizontal into the intermediate target, vertical into the final one.
	// in_horizontal_input_set samples the caller's source image (must be
	// SHADER_READ_ONLY); in_vertical_input_set samples the intermediate
	// target. Leaves both targets in SHADER_READ_ONLY.
	inline void execute_separable(
		VulkanContext* ctx,
		RenderPassEntry& in_entry,
		VkDescriptorSet in_horizontal_input_set,
		VkDescriptorSet in_vertical_input_set,
		i32 in_blur_size
	)
	{
		RenderPass& horizontal_pass = in_entry.intermediate_pass();
		RenderPass& vertical_pass = in_entry.final_pass();
		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];

		const HMM_Vec2 screen_size = HMM_V2((f32) horizontal_pass.current_width, (f32) horizontal_pass.current_height);

		horizontal_pass.execute(ctx, [&](i32)
		{
			draw_blur(ctx, in_horizontal_input_set, screen_size, HMM_V2(1.0f, 0.0f), in_blur_size);
		});
		gpu_image_transition(command_buffer, horizontal_pass.get_color_output(0), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vertical_pass.execute(ctx, [&](i32)
		{
			draw_blur(ctx, in_vertical_input_set, screen_size, HMM_V2(0.0f, 1.0f), in_blur_size);
		});
		gpu_image_transition(command_buffer, vertical_pass.get_color_output(0), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}

	inline void shutdown(VulkanContext* ctx)
	{
		vkDestroyPipeline(ctx->device, pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, pipeline_layout, nullptr);
	}
}
