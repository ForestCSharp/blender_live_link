#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/render_pass.h"
#include "render/shader_module.h"
#include "render/frame_data.h"
#include "render/shadow_depth_pass.h"

// Separable gaussian blur over the EVSM moments array (port of
// game/src/render/shadow_blur_pass.h). Horizontal (intermediate pass) then
// vertical (final pass); lighting samples the final output when
// state.shadow.blur_enable is set. The blur is what makes EVSM shadows soft.

namespace ShadowBlurPass
{
	constexpr i32 BlurSize = 21;

	struct PushConstants
	{
		HMM_Vec2 screen_size;
		HMM_Vec2 direction;
		i32 blur_size;
		i32 array_layer;
	};
	static_assert(sizeof(PushConstants) == 24, "Must match shadow_blur.frag's push constant block");

	inline VkDescriptorPool pool = VK_NULL_HANDLE;
	inline VkDescriptorSet horizontal_input_set = VK_NULL_HANDLE;
	inline VkDescriptorSet vertical_input_set = VK_NULL_HANDLE;
	inline VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	inline VkPipeline pipeline = VK_NULL_HANDLE;

	inline void init(VulkanContext* ctx)
	{
		// Dedicated pool: two static sets (inputs are fixed-size images that
		// are never recreated, so the sets are written once and never touched
		// while frames are in flight)
		VkDescriptorPoolSize pool_size = {
			.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = 2,
		};
		VkDescriptorPoolCreateInfo pool_create_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = 2,
			.poolSizeCount = 1,
			.pPoolSizes = &pool_size,
		};
		VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_create_info, nullptr, &pool));

		VkDescriptorSetAllocateInfo allocate_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &frame_data.sampled_input_layout,
		};
		VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &horizontal_input_set));
		VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &vertical_input_set));

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

		VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, "bin/shaders/shadow_blur.vert.spv");
		VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/shadow_blur.frag.spv");

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

		VkFormat moments_format = Render::SHADOW_MOMENTS_FORMAT;
		VkPipelineRenderingCreateInfo rendering_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = 1,
			.pColorAttachmentFormats = &moments_format,
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
		VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_create_info, &pipeline));

		vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
		vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
	}

	// Writes the two static input sets. Called once after the ShadowDepth +
	// ShadowBlur passes are registered (their fixed-size images exist and are
	// never recreated).
	inline void init_sets(VulkanContext* ctx, VkImageView in_moments_view, VkImageView in_horizontal_view, VkSampler in_sampler)
	{
		VkDescriptorImageInfo image_infos[] = {
			{
				.sampler = in_sampler,
				.imageView = in_moments_view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			},
			{
				.sampler = in_sampler,
				.imageView = in_horizontal_view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			},
		};
		VkWriteDescriptorSet writes[] = {
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = horizontal_input_set,
				.dstBinding = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image_infos[0],
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = vertical_input_set,
				.dstBinding = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image_infos[1],
			},
		};
		vulkan_update_descriptor_sets(ctx, 2, writes);
	}

	inline void draw_blur_slice(VulkanContext* ctx, VkDescriptorSet in_input_set, HMM_Vec2 in_direction, i32 in_cascade_idx)
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
			.screen_size = HMM_V2((f32) ShadowDepthPass::ShadowMapResolution, (f32) ShadowDepthPass::ShadowMapResolution),
			.direction = in_direction,
			.blur_size = BlurSize,
			.array_layer = in_cascade_idx,
		};
		vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants), &push_constants);
		vulkan_cmd_draw(ctx, 3, 1, 0, 0);
	}

	// Horizontal into the intermediate target, vertical into the final one.
	// Expects the raw moments image already in SHADER_READ_ONLY; leaves both
	// blur targets in SHADER_READ_ONLY.
	inline void execute_separable(VulkanContext* ctx, RenderPassEntry& in_entry, i32 in_active_cascade_count)
	{
		RenderPass& horizontal_pass = in_entry.intermediate_pass();
		RenderPass& vertical_pass = in_entry.final_pass();
		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];

		horizontal_pass.set_pass_count_override(in_active_cascade_count);
		horizontal_pass.execute(ctx, [&](i32 in_cascade_idx)
		{
			draw_blur_slice(ctx, horizontal_input_set, HMM_V2(1.0f, 0.0f), in_cascade_idx);
		});
		horizontal_pass.set_pass_count_override(-1);

		gpu_image_transition(command_buffer, horizontal_pass.get_color_output(0), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		vertical_pass.set_pass_count_override(in_active_cascade_count);
		vertical_pass.execute(ctx, [&](i32 in_cascade_idx)
		{
			draw_blur_slice(ctx, vertical_input_set, HMM_V2(0.0f, 1.0f), in_cascade_idx);
		});
		vertical_pass.set_pass_count_override(-1);
	}

	inline void shutdown(VulkanContext* ctx)
	{
		vkDestroyPipeline(ctx->device, pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, pipeline_layout, nullptr);
		vkDestroyDescriptorPool(ctx->device, pool, nullptr);
	}
}
