#pragma once

#include "render/frame_data.h"
#include "render/render_pass.h"
#include "render/shader_module.h"

namespace ShadowCascadeDebugPass
{
	struct PushConstants
	{
		i32 cascade_index;
		i32 view_mode;
	};

	inline VkDescriptorPool pool = VK_NULL_HANDLE;
	inline VkDescriptorSet sets[MAX_FRAMES_IN_FLIGHT] = {};
	inline VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	inline VkPipeline pipeline = VK_NULL_HANDLE;

	inline void init(VulkanContext* ctx)
	{
		VkDescriptorPoolSize pool_size = { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_FRAMES_IN_FLIGHT };
		VkDescriptorPoolCreateInfo pool_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = MAX_FRAMES_IN_FLIGHT,
			.poolSizeCount = 1,
			.pPoolSizes = &pool_size,
		};
		VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_info, nullptr, &pool));
		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			VkDescriptorSetAllocateInfo alloc_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &frame_data.sampled_input_layout,
			};
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &alloc_info, &sets[frame_idx]));
		}

		VkPushConstantRange push_range = {
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			.size = sizeof(PushConstants),
		};
		VkPipelineLayoutCreateInfo layout_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &frame_data.sampled_input_layout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_range,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_info, nullptr, &pipeline_layout));

		VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, "bin/shaders/shadow_cascade_debug.vert.spv");
		VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/shadow_cascade_debug.frag.spv");
		VkPipelineShaderStageCreateInfo stages[] = {
			{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex_module, .pName = "main" },
			{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment_module, .pName = "main" },
		};
		VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamic = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dynamic_states };
		VkPipelineVertexInputStateCreateInfo vertex_input = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		VkPipelineInputAssemblyStateCreateInfo assembly = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
		VkPipelineViewportStateCreateInfo viewport = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
		VkPipelineRasterizationStateCreateInfo raster = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
		VkPipelineMultisampleStateCreateInfo msaa = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
		VkPipelineDepthStencilStateCreateInfo depth = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		VkPipelineColorBlendAttachmentState blend_attachment = { .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
		VkPipelineColorBlendStateCreateInfo blend = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend_attachment };
		VkFormat output_format = VK_FORMAT_R16G16B16A16_SFLOAT;
		VkPipelineRenderingCreateInfo rendering = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &output_format };
		VkGraphicsPipelineCreateInfo pipeline_info = {
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.pNext = &rendering,
			.stageCount = 2,
			.pStages = stages,
			.pVertexInputState = &vertex_input,
			.pInputAssemblyState = &assembly,
			.pViewportState = &viewport,
			.pRasterizationState = &raster,
			.pMultisampleState = &msaa,
			.pDepthStencilState = &depth,
			.pColorBlendState = &blend,
			.pDynamicState = &dynamic,
			.layout = pipeline_layout,
		};
		VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_info, &pipeline));
		vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
		vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
	}

	inline void render(VulkanContext* ctx, VkImageView moments_view, VkSampler sampler, i32 cascade_index, i32 view_mode)
	{
		VkDescriptorImageInfo image_info = { .sampler = sampler, .imageView = moments_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[ctx->frame_index], .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image_info };
		vulkan_update_descriptor_sets(ctx, 1, &write);

		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &sets[ctx->frame_index], 0, nullptr);
		PushConstants push = { .cascade_index = cascade_index, .view_mode = view_mode };
		vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		vulkan_cmd_draw(ctx, 3, 1, 0, 0);
	}

	inline void shutdown(VulkanContext* ctx)
	{
		vkDestroyPipeline(ctx->device, pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, pipeline_layout, nullptr);
		vkDestroyDescriptorPool(ctx->device, pool, nullptr);
	}
}
