#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/shader_module.h"
#include "render/gpu_buffer.h"

// Exponential height fog over the lit scene (port of game/'s fog_pass.h +
// fog.glsl). Runs after lighting when an enabled fog controller exists;
// downstream passes read this output instead of the lighting target.

// Mirrors fog.frag's fs_params / game/'s fog_fs_params_t (std140)
struct FogFsParams
{
	HMM_Vec3 camera_position;
	f32 fog_base_height;
	HMM_Vec3 fog_color;
	f32 density;
	f32 scale_height;
	f32 max_distance;
	i32 ceiling_enabled;
	f32 ceiling_height;
	f32 ceiling_fade;
	f32 ambient_intensity;
	f32 sun_intensity;
	f32 anisotropy;
	HMM_Vec3 sun_direction;
	f32 _pad0;
	HMM_Vec3 sun_color;
	f32 _pad1;
};
static_assert(sizeof(FogFsParams) == 96, "Must match game/'s fog_fs_params_t std140 layout");

struct FogPass
{
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet sets[MAX_FRAMES_IN_FLIGHT] = {};

	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;

	GpuBuffer<FogFsParams> fs_params_ubos[MAX_FRAMES_IN_FLIGHT];

	VkSampler linear_sampler = VK_NULL_HANDLE;	// borrowed from frame_data
};

static FogPass fog_pass;

void fog_pass_init(VulkanContext* ctx, VkSampler in_linear_sampler)
{
	fog_pass.linear_sampler = in_linear_sampler;

	// Set layout: b0 fs_params UBO, b1 lit color CIS, b2 world position CIS
	{
		VkDescriptorSetLayoutBinding bindings[3] = {};
		bindings[0] = (VkDescriptorSetLayoutBinding) {
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		};
		for (u32 binding_idx = 1; binding_idx <= 2; ++binding_idx)
		{
			bindings[binding_idx] = (VkDescriptorSetLayoutBinding) {
				.binding = binding_idx,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			};
		}

		VkDescriptorSetLayoutCreateInfo layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = 3,
			.pBindings = bindings,
		};
		VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &layout_create_info, nullptr, &fog_pass.set_layout));
	}

	// Pool + per-frame sets + UBOs
	{
		VkDescriptorPoolSize pool_sizes[] = {
			{ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1 * MAX_FRAMES_IN_FLIGHT },
			{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 2 * MAX_FRAMES_IN_FLIGHT },
		};
		VkDescriptorPoolCreateInfo pool_create_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = MAX_FRAMES_IN_FLIGHT,
			.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]),
			.pPoolSizes = pool_sizes,
		};
		VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_create_info, nullptr, &fog_pass.pool));

		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			VkDescriptorSetAllocateInfo allocate_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = fog_pass.pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &fog_pass.set_layout,
			};
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &fog_pass.sets[frame_idx]));

			fog_pass.fs_params_ubos[frame_idx] = GpuBuffer((GpuBufferDesc<FogFsParams>){
				.data = nullptr,
				.size = sizeof(FogFsParams),
				.usage = { .uniform_buffer = true, .stream_update = true },
				.label = "FogPass::fs_params",
			});
		}
	}

	// Pipeline
	{
		VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &fog_pass.set_layout,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &pipeline_layout_create_info, nullptr, &fog_pass.pipeline_layout));

		VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, "bin/shaders/fog.vert.spv");
		VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/fog.frag.spv");

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

		VkFormat color_format = Render::SCENE_COLOR_FORMAT;
		VkPipelineRenderingCreateInfo rendering_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = 1,
			.pColorAttachmentFormats = &color_format,
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
			.layout = fog_pass.pipeline_layout,
			.renderPass = VK_NULL_HANDLE,
		};
		VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_create_info, &fog_pass.pipeline));

		vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
		vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
	}
}

// Uploads fs_params + rewrites this frame's set (after the fence wait, before
// any binds record)
void fog_pass_update(
	VulkanContext* ctx,
	const FogFsParams& in_fs_params,
	VkImageView in_lit_color_view,
	VkImageView in_gbuffer_position_view
)
{
	const u32 frame_index = ctx->frame_index;

	fog_pass.fs_params_ubos[frame_index].update_gpu_buffer(&in_fs_params, sizeof(FogFsParams));

	VkDescriptorBufferInfo ubo_info = {
		.buffer = fog_pass.fs_params_ubos[frame_index].get_gpu_buffer(),
		.offset = 0,
		.range = sizeof(FogFsParams),
	};
	VkDescriptorImageInfo image_infos[] = {
		{
			.sampler = fog_pass.linear_sampler,
			.imageView = in_lit_color_view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		},
		{
			.sampler = fog_pass.linear_sampler,
			.imageView = in_gbuffer_position_view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		},
	};

	VkWriteDescriptorSet writes[3] = {};
	writes[0] = (VkWriteDescriptorSet) {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = fog_pass.sets[frame_index],
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &ubo_info,
	};
	for (u32 image_idx = 0; image_idx < 2; ++image_idx)
	{
		writes[image_idx + 1] = (VkWriteDescriptorSet) {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = fog_pass.sets[frame_index],
			.dstBinding = image_idx + 1,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image_infos[image_idx],
		};
	}
	vulkan_update_descriptor_sets(ctx, 3, writes);
}

void fog_pass_draw(VulkanContext* ctx)
{
	VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, fog_pass.pipeline);
	vkCmdBindDescriptorSets(
		command_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		fog_pass.pipeline_layout,
		0, 1, &fog_pass.sets[ctx->frame_index],
		0, nullptr
	);
	vulkan_cmd_draw(ctx, 3, 1, 0, 0);
}

void fog_pass_shutdown(VulkanContext* ctx)
{
	vkDestroyPipeline(ctx->device, fog_pass.pipeline, nullptr);
	vkDestroyPipelineLayout(ctx->device, fog_pass.pipeline_layout, nullptr);
	for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
	{
		fog_pass.fs_params_ubos[frame_idx].destroy_gpu_buffer();
	}
	vkDestroyDescriptorPool(ctx->device, fog_pass.pool, nullptr);
	vkDestroyDescriptorSetLayout(ctx->device, fog_pass.set_layout, nullptr);
}
