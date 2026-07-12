#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/shader_module.h"
#include "render/gpu_buffer.h"

// Temporal AA (port of game/'s temporal_aa_pass.h): Decima-style 2-phase
// jitter, previous-frame reprojection with neighborhood clamp/rejection, and
// a jitter-axis sharpen. game ping-pongs two target sets through the
// RenderPassEntry's intermediate (set 0) and final (set 1) passes — each has
// MRT [resolved, history]; the shader reads the other set's history.

// Mirrors temporal_aa.frag's fs_params (std140)
struct TemporalAaFsParams
{
	HMM_Mat4 previous_view_projection;
	HMM_Vec2 screen_size;
	HMM_Vec2 sharpen_axis;
	f32 blend_alpha;
	f32 sharpen_strength;
	f32 rejection_threshold;
	i32 history_valid;
	i32 debug_mode;
	f32 _pad0[3];
};
static_assert(sizeof(TemporalAaFsParams) == 112, "Must match game/'s temporal_aa_fs_params_t std140 layout");

namespace TemporalAAPass
{
	inline VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	inline VkDescriptorPool pool = VK_NULL_HANDLE;
	inline VkDescriptorSet sets[MAX_FRAMES_IN_FLIGHT] = {};

	inline VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	inline VkPipeline pipeline = VK_NULL_HANDLE;

	inline GpuBuffer<TemporalAaFsParams> fs_params_ubos[MAX_FRAMES_IN_FLIGHT];

	inline VkSampler linear_sampler = VK_NULL_HANDLE;	// borrowed from frame_data
	inline VkSampler nearest_sampler = VK_NULL_HANDLE;	// owned (position reads)

	inline void invalidate_history(State& in_state)
	{
		in_state.temporal_aa.history_valid = false;
	}

	inline HMM_Vec2 get_decima_jitter_pixels(i32 jitter_phase)
	{
		return (jitter_phase & 1) == 1
			? HMM_V2(0.5f, 0.0f)
			: HMM_V2(0.0f, 0.5f);
	}

	inline HMM_Mat4 apply_projection_jitter(HMM_Mat4 in_projection, HMM_Vec2 jitter_pixels, HMM_Vec2 screen_size)
	{
		if (screen_size.X <= 0.0f || screen_size.Y <= 0.0f)
		{
			return in_projection;
		}

		const HMM_Vec2 jitter_ndc = HMM_V2(
			(2.0f * jitter_pixels.X) / screen_size.X,
			(-2.0f * jitter_pixels.Y) / screen_size.Y
		);
		for (i32 column = 0; column < 4; ++column)
		{
			in_projection.Elements[column][0] += jitter_ndc.X * in_projection.Elements[column][3];
			in_projection.Elements[column][1] += jitter_ndc.Y * in_projection.Elements[column][3];
		}
		return in_projection;
	}

	inline void init(VulkanContext* ctx, VkSampler in_linear_sampler)
	{
		linear_sampler = in_linear_sampler;

		VkSamplerCreateInfo nearest_create_info = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_NEAREST,
			.minFilter = VK_FILTER_NEAREST,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};
		VK_CHECK(vkCreateSampler(ctx->device, &nearest_create_info, nullptr, &nearest_sampler));

		// Set layout: b0 fs UBO, b1 current color CIS, b2 position CIS,
		// b3 history CIS
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
			VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &layout_create_info, nullptr, &set_layout));
		}

		// Pool + per-frame sets + UBOs
		{
			VkDescriptorPoolSize pool_sizes[] = {
				{ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1 * MAX_FRAMES_IN_FLIGHT },
				{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 3 * MAX_FRAMES_IN_FLIGHT },
			};
			VkDescriptorPoolCreateInfo pool_create_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.maxSets = MAX_FRAMES_IN_FLIGHT,
				.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]),
				.pPoolSizes = pool_sizes,
			};
			VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_create_info, nullptr, &pool));

			for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
			{
				VkDescriptorSetAllocateInfo allocate_info = {
					.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
					.descriptorPool = pool,
					.descriptorSetCount = 1,
					.pSetLayouts = &set_layout,
				};
				VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &sets[frame_idx]));

				fs_params_ubos[frame_idx] = GpuBuffer((GpuBufferDesc<TemporalAaFsParams>){
					.data = nullptr,
					.size = sizeof(TemporalAaFsParams),
					.usage = { .uniform_buffer = true, .stream_update = true },
					.label = "TemporalAAPass::fs_params",
				});
			}
		}

		// Pipeline: MRT [resolved, history], both scene-color format
		{
			VkPipelineLayoutCreateInfo layout_create_info = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 1,
				.pSetLayouts = &set_layout,
			};
			VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &pipeline_layout));

			VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, "bin/shaders/temporal_aa.vert.spv");
			VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/temporal_aa.frag.spv");

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
			VkPipelineColorBlendAttachmentState blend_attachments[2] = {};
			for (u32 attachment_idx = 0; attachment_idx < 2; ++attachment_idx)
			{
				blend_attachments[attachment_idx] = (VkPipelineColorBlendAttachmentState) {
					.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
									| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
				};
			}
			VkPipelineColorBlendStateCreateInfo color_blending = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
				.attachmentCount = 2,
				.pAttachments = blend_attachments,
			};

			VkFormat color_formats[2] = { Render::SCENE_COLOR_FORMAT, Render::SCENE_COLOR_FORMAT };
			VkPipelineRenderingCreateInfo rendering_create_info = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
				.colorAttachmentCount = 2,
				.pColorAttachmentFormats = color_formats,
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
	}

	// After the fence wait, before any binds record
	inline void update(
		VulkanContext* ctx,
		const TemporalAaFsParams& in_fs_params,
		VkImageView in_current_color_view,
		VkImageView in_gbuffer_position_view,
		VkImageView in_previous_history_view
	)
	{
		const u32 frame_index = ctx->frame_index;

		fs_params_ubos[frame_index].update_gpu_buffer(&in_fs_params, sizeof(in_fs_params));

		VkDescriptorBufferInfo ubo_info = {
			.buffer = fs_params_ubos[frame_index].get_gpu_buffer(),
			.offset = 0,
			.range = sizeof(TemporalAaFsParams),
		};
		VkDescriptorImageInfo image_infos[] = {
			{
				.sampler = linear_sampler,
				.imageView = in_current_color_view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			},
			{
				.sampler = nearest_sampler,
				.imageView = in_gbuffer_position_view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			},
			{
				.sampler = linear_sampler,
				.imageView = in_previous_history_view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			},
		};

		VkWriteDescriptorSet writes[4] = {};
		writes[0] = (VkWriteDescriptorSet) {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = sets[frame_index],
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &ubo_info,
		};
		for (u32 image_idx = 0; image_idx < 3; ++image_idx)
		{
			writes[1 + image_idx] = (VkWriteDescriptorSet) {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = sets[frame_index],
				.dstBinding = 1 + image_idx,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image_infos[image_idx],
			};
		}
		vkUpdateDescriptorSets(ctx->device, 4, writes, 0, nullptr);
	}

	inline void draw(VulkanContext* ctx)
	{
		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdBindDescriptorSets(
			command_buffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipeline_layout,
			0, 1, &sets[ctx->frame_index],
			0, nullptr
		);
		vkCmdDraw(command_buffer, 3, 1, 0, 0);
	}

	inline void shutdown(VulkanContext* ctx)
	{
		vkDestroyPipeline(ctx->device, pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, pipeline_layout, nullptr);
		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			fs_params_ubos[frame_idx].destroy_gpu_buffer();
		}
		vkDestroyDescriptorPool(ctx->device, pool, nullptr);
		vkDestroyDescriptorSetLayout(ctx->device, set_layout, nullptr);
		vkDestroySampler(ctx->device, nearest_sampler, nullptr);
	}
}
