#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/shader_module.h"
#include "render/gpu_buffer.h"
#include "render/frame_data.h"

#include <random>

#include "ssao_constants.h"

// Hemisphere-kernel SSAO over the G-buffer at half render resolution (port
// of game/'s ssao pass, game/src/main.cpp:1485-1557 + ssao.glsl). The raw
// output goes through the generic BlurPass before lighting samples it.

// Mirrors ssao.frag's fs_params / game/'s ssao_fs_params_t (std140)
struct SsaoFsParams
{
	HMM_Vec2 screen_size;
	f32 _pad0[2];
	HMM_Mat4 view;
	HMM_Mat4 projection;
	HMM_Vec4 kernel_samples[SSAO_KERNEL_SIZE];
	i32 ssao_enable;
	f32 _pad1[3];
};
static_assert(sizeof(SsaoFsParams) == 928, "Must match game/'s ssao_fs_params_t std140 layout");

struct SsaoPass
{
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet sets[MAX_FRAMES_IN_FLIGHT] = {};

	// Inputs for the SSAO blur (BlurPass): horizontal samples the raw SSAO
	// output, vertical samples the blur's intermediate target. Per-frame
	// because the half-res targets are recreated on resize.
	VkDescriptorSet blur_horizontal_sets[MAX_FRAMES_IN_FLIGHT] = {};
	VkDescriptorSet blur_vertical_sets[MAX_FRAMES_IN_FLIGHT] = {};

	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;

	GpuBuffer<SsaoFsParams> fs_params_ubos[MAX_FRAMES_IN_FLIGHT];
	SsaoFsParams fs_params_template = {};	// kernel filled once at init

	GpuImage noise_texture;
	VkSampler linear_sampler = VK_NULL_HANDLE;	// borrowed from frame_data
};

static SsaoPass ssao_pass;

void ssao_pass_init(VulkanContext* ctx, VkSampler in_linear_sampler)
{
	ssao_pass.linear_sampler = in_linear_sampler;

	// Noise texture + hemisphere kernel (game/src/main.cpp:1511-1557)
	{
		std::uniform_real_distribution<f32> randomf32s(0.0, 1.0);
		std::default_random_engine generator;

		HMM_Vec4 ssao_noise[SSAO_TEXTURE_SIZE];
		for (u32 i = 0; i < SSAO_TEXTURE_SIZE; ++i)
		{
			ssao_noise[i] = HMM_V4(
				randomf32s(generator) * 2.0f - 1.0f,
				randomf32s(generator) * 2.0f - 1.0f,
				0.0f,
				0.0f
			);
		}
		ssao_pass.noise_texture = gpu_image_create_from_data(
			ctx,
			SSAO_TEXTURE_WIDTH,
			SSAO_TEXTURE_WIDTH,
			VK_FORMAT_R32G32B32A32_SFLOAT,
			ssao_noise,
			sizeof(ssao_noise)
		);

		for (u32 i = 0; i < SSAO_KERNEL_SIZE; ++i)
		{
			HMM_Vec3 sample = HMM_V3(
				randomf32s(generator) * 2.0f - 1.0f,
				randomf32s(generator) * 2.0f - 1.0f,
				randomf32s(generator)
			);
			sample = HMM_NormV3(sample);
			sample *= randomf32s(generator);

			// Scale samples s.t. they're more aligned to the kernel center
			f32 scale = (f32) i / (f32) SSAO_KERNEL_SIZE;
			scale = HMM_Lerp(0.1f, scale * scale, 1.0f);
			sample *= scale;

			ssao_pass.fs_params_template.kernel_samples[i] = HMM_V4(sample.X, sample.Y, sample.Z, 0.0f);
		}
	}

	// Set layout: b0 fs_params UBO, b1 position, b2 normal, b3 noise
	{
		VkDescriptorSetLayoutBinding bindings[4] = {};
		bindings[0] = (VkDescriptorSetLayoutBinding) {
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		};
		for (u32 binding_idx = 1; binding_idx <= 3; ++binding_idx)
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
			.bindingCount = 4,
			.pBindings = bindings,
		};
		VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &layout_create_info, nullptr, &ssao_pass.set_layout));
	}

	// Pool + per-frame sets (rewritten each frame: G-buffer views change on
	// resize)
	{
		VkDescriptorPoolSize pool_sizes[] = {
			{ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1 * MAX_FRAMES_IN_FLIGHT },
			{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 5 * MAX_FRAMES_IN_FLIGHT },
		};
		VkDescriptorPoolCreateInfo pool_create_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = 3 * MAX_FRAMES_IN_FLIGHT,
			.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]),
			.pPoolSizes = pool_sizes,
		};
		VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_create_info, nullptr, &ssao_pass.pool));

		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			VkDescriptorSetAllocateInfo allocate_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = ssao_pass.pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &ssao_pass.set_layout,
			};
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &ssao_pass.sets[frame_idx]));

			allocate_info.pSetLayouts = &frame_data.sampled_input_layout;
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &ssao_pass.blur_horizontal_sets[frame_idx]));
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &ssao_pass.blur_vertical_sets[frame_idx]));
		}
	}

	// Per-frame fs_params UBOs
	for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
	{
		ssao_pass.fs_params_ubos[frame_idx] = GpuBuffer((GpuBufferDesc<SsaoFsParams>){
			.data = nullptr,
			.size = sizeof(SsaoFsParams),
			.usage = {
				.uniform_buffer = true,
				.stream_update = true,
			},
			.label = "SsaoPass::fs_params",
		});
	}

	// Pipeline
	{
		VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &ssao_pass.set_layout,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &pipeline_layout_create_info, nullptr, &ssao_pass.pipeline_layout));

		VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, "bin/shaders/ssao.vert.spv");
		VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/ssao.frag.spv");

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

		VkFormat ssao_format = Render::SSAO_FORMAT;
		VkPipelineRenderingCreateInfo rendering_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = 1,
			.pColorAttachmentFormats = &ssao_format,
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
			.layout = ssao_pass.pipeline_layout,
			.renderPass = VK_NULL_HANDLE,
		};
		VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_create_info, &ssao_pass.pipeline));

		vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
		vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
	}
}

// Uploads fs_params + rewrites this frame's set (after the fence wait, before
// any binds record). Pass the SSAO pass target size (half render res).
void ssao_pass_update(
	VulkanContext* ctx,
	HMM_Vec2 in_target_size,
	const HMM_Mat4& in_view,
	const HMM_Mat4& in_projection,
	bool in_enable,
	VkImageView in_gbuffer_position_view,
	VkImageView in_gbuffer_normal_view,
	VkImageView in_ssao_output_view,
	VkImageView in_blur_intermediate_view
)
{
	const u32 frame_index = ctx->frame_index;

	SsaoFsParams fs_params = ssao_pass.fs_params_template;
	fs_params.screen_size = in_target_size;
	fs_params.view = in_view;
	fs_params.projection = in_projection;
	fs_params.ssao_enable = in_enable ? 1 : 0;
	ssao_pass.fs_params_ubos[frame_index].update_gpu_buffer(&fs_params, sizeof(SsaoFsParams));

	VkDescriptorBufferInfo ubo_info = {
		.buffer = ssao_pass.fs_params_ubos[frame_index].get_gpu_buffer(),
		.offset = 0,
		.range = sizeof(SsaoFsParams),
	};
	VkDescriptorImageInfo image_infos[] = {
		{
			.sampler = ssao_pass.linear_sampler,
			.imageView = in_gbuffer_position_view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		},
		{
			.sampler = ssao_pass.linear_sampler,
			.imageView = in_gbuffer_normal_view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		},
		{
			.sampler = ssao_pass.linear_sampler,
			.imageView = ssao_pass.noise_texture.view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		},
	};

	VkDescriptorImageInfo blur_input_infos[] = {
		{
			.sampler = ssao_pass.linear_sampler,
			.imageView = in_ssao_output_view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		},
		{
			.sampler = ssao_pass.linear_sampler,
			.imageView = in_blur_intermediate_view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		},
	};

	VkWriteDescriptorSet writes[6] = {};
	writes[0] = (VkWriteDescriptorSet) {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = ssao_pass.sets[frame_index],
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &ubo_info,
	};
	for (u32 image_idx = 0; image_idx < 3; ++image_idx)
	{
		writes[image_idx + 1] = (VkWriteDescriptorSet) {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = ssao_pass.sets[frame_index],
			.dstBinding = image_idx + 1,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image_infos[image_idx],
		};
	}
	writes[4] = (VkWriteDescriptorSet) {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = ssao_pass.blur_horizontal_sets[frame_index],
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &blur_input_infos[0],
	};
	writes[5] = (VkWriteDescriptorSet) {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = ssao_pass.blur_vertical_sets[frame_index],
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &blur_input_infos[1],
	};
	vulkan_update_descriptor_sets(ctx, 6, writes);
}

void ssao_pass_draw(VulkanContext* ctx)
{
	VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ssao_pass.pipeline);
	vkCmdBindDescriptorSets(
		command_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		ssao_pass.pipeline_layout,
		0, 1, &ssao_pass.sets[ctx->frame_index],
		0, nullptr
	);
	vulkan_cmd_draw(ctx, 3, 1, 0, 0);
}

void ssao_pass_shutdown(VulkanContext* ctx)
{
	vkDestroyPipeline(ctx->device, ssao_pass.pipeline, nullptr);
	vkDestroyPipelineLayout(ctx->device, ssao_pass.pipeline_layout, nullptr);
	for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
	{
		ssao_pass.fs_params_ubos[frame_idx].destroy_gpu_buffer();
	}
	gpu_image_destroy(ctx->allocator, ctx->device, ssao_pass.noise_texture);
	vkDestroyDescriptorPool(ctx->device, ssao_pass.pool, nullptr);
	vkDestroyDescriptorSetLayout(ctx->device, ssao_pass.set_layout, nullptr);
}
