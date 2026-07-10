#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/shader_module.h"
#include "render/gpu_buffer.h"

// Deferred lighting pass: fullscreen, reads the G-buffer + light SSBOs,
// writes HDR scene color. Owns descriptor set layout C (pass-local set 0):
//   0 = fs_params UBO                     (FS)
//   1-4 = G-buffer attachments 0-3        (FS, combined image samplers)
//   5 = shadow moments texture array      (FS; written once shadows exist)
//   6-8 = point/spot/sun light SSBOs      (FS)

// C++ mirror of the lighting fs_params UBO. Byte-identical to game/'s
// lighting_fs_params_t (lighting.compiled.h:136-166) — GI/SSAO/SSS fields
// are carried (zeroed) for later phases.
struct LightingFsParams
{
	HMM_Vec3 view_position;
	f32 _pad0;
	HMM_Vec3 view_forward;
	i32 num_point_lights;
	i32 num_spot_lights;
	i32 num_sun_lights;
	i32 ssao_enable;
	i32 direct_lighting_enable;
	i32 gi_enable;
	i32 gi_probe_occlusion;
	i32 probe_occlusion_mode;
	i32 probe_radiance_mode;
	f32 gi_intensity;
	i32 atlas_total_size;
	i32 atlas_entry_size;
	i32 gi_fallback_probe_index;
	i32 gi_octree_node_count;
	i32 shadow_map_enable;
	i32 shadow_num_cascades;
	i32 shadow_cascade_placement_mode;
	i32 shadow_debug_show_cascade_selection;
	i32 isolated_probe_index;
	i32 screen_space_shadows_enable;
	f32 shadow_bias;
	f32 screen_space_shadow_intensity;
	f32 _pad1;
	HMM_Vec2 shadow_map_texel_size;
	HMM_Vec4 shadow_cascade_distances;
	HMM_Mat4 shadow_view_projections[4];
};
static_assert(sizeof(LightingFsParams) == 400, "Must match game/'s lighting_fs_params_t std140 layout");

struct LightingPass
{
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet sets[MAX_FRAMES_IN_FLIGHT] = {};

	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;

	GpuBuffer<LightingFsParams> fs_params_ubos[MAX_FRAMES_IN_FLIGHT];

	VkSampler linear_sampler = VK_NULL_HANDLE;	// borrowed from frame_data at init
};

static LightingPass lighting_pass;

void lighting_pass_init(VulkanContext* ctx, VkSampler in_linear_sampler)
{
	lighting_pass.linear_sampler = in_linear_sampler;

	// Layout C
	{
		VkDescriptorSetLayoutBinding bindings[9] = {};
		bindings[0] = (VkDescriptorSetLayoutBinding) {
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		};
		for (u32 binding_idx = 1; binding_idx <= 5; ++binding_idx)
		{
			bindings[binding_idx] = (VkDescriptorSetLayoutBinding) {
				.binding = binding_idx,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			};
		}
		for (u32 binding_idx = 6; binding_idx <= 8; ++binding_idx)
		{
			bindings[binding_idx] = (VkDescriptorSetLayoutBinding) {
				.binding = binding_idx,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			};
		}

		// Binding 5 (shadow moments) is unwritten until the shadow pass
		// exists — legal while the shader doesn't statically use it
		VkDescriptorBindingFlags binding_flags[9] = {};
		binding_flags[5] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
		VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_create_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
			.bindingCount = 9,
			.pBindingFlags = binding_flags,
		};

		VkDescriptorSetLayoutCreateInfo layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.pNext = &binding_flags_create_info,
			.bindingCount = 9,
			.pBindings = bindings,
		};
		VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &layout_create_info, nullptr, &lighting_pass.set_layout));
	}

	// Pool + sets
	{
		VkDescriptorPoolSize pool_sizes[] = {
			{ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1 * MAX_FRAMES_IN_FLIGHT },
			{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 5 * MAX_FRAMES_IN_FLIGHT },
			{ .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 3 * MAX_FRAMES_IN_FLIGHT },
		};
		VkDescriptorPoolCreateInfo pool_create_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = MAX_FRAMES_IN_FLIGHT,
			.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]),
			.pPoolSizes = pool_sizes,
		};
		VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_create_info, nullptr, &lighting_pass.pool));

		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			VkDescriptorSetAllocateInfo allocate_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = lighting_pass.pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &lighting_pass.set_layout,
			};
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &lighting_pass.sets[frame_idx]));
		}
	}

	// Per-frame fs_params UBOs
	for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
	{
		lighting_pass.fs_params_ubos[frame_idx] = GpuBuffer((GpuBufferDesc<LightingFsParams>){
			.data = nullptr,
			.size = sizeof(LightingFsParams),
			.usage = {
				.uniform_buffer = true,
				.stream_update = true,
			},
			.label = "LightingPass::fs_params",
		});
	}

	// Pipeline
	{
		VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &lighting_pass.set_layout,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &pipeline_layout_create_info, nullptr, &lighting_pass.pipeline_layout));

		VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, "bin/shaders/lighting.vert.spv");
		VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/lighting.frag.spv");

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
			.depthTestEnable = VK_FALSE,
			.depthWriteEnable = VK_FALSE,
		};
		VkPipelineColorBlendAttachmentState color_blend_attachment = {
			.blendEnable = VK_FALSE,
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
							| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
		};
		VkPipelineColorBlendStateCreateInfo color_blending = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = &color_blend_attachment,
		};
		VkPipelineRenderingCreateInfo pipeline_rendering_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = 1,
			.pColorAttachmentFormats = &Render::SCENE_COLOR_FORMAT,
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
			.layout = lighting_pass.pipeline_layout,
			.renderPass = VK_NULL_HANDLE,
		};
		VK_CHECK(vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &lighting_pass.pipeline));

		vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
		vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
	}
}

// Uploads fs_params + rewrites this frame's descriptor set (call after the
// fence wait, before any binds record). in_shadow_moments_view may be
// VK_NULL_HANDLE until the shadow pass exists (binding 5 stays unwritten).
void lighting_pass_update(
	VulkanContext* ctx,
	const LightingFsParams& in_fs_params,
	const GpuImage* in_gbuffer_outputs,	// 4 attachments
	VkImageView in_shadow_moments_view,
	VkBuffer in_point_lights_buffer,
	VkBuffer in_spot_lights_buffer,
	VkBuffer in_sun_lights_buffer
)
{
	const u32 frame_index = ctx->frame_index;

	lighting_pass.fs_params_ubos[frame_index].update_gpu_buffer(&in_fs_params, sizeof(LightingFsParams));

	VkDescriptorBufferInfo ubo_info = {
		.buffer = lighting_pass.fs_params_ubos[frame_index].get_gpu_buffer(),
		.offset = 0,
		.range = sizeof(LightingFsParams),
	};

	VkDescriptorImageInfo gbuffer_infos[4];
	for (i32 attachment_idx = 0; attachment_idx < 4; ++attachment_idx)
	{
		gbuffer_infos[attachment_idx] = (VkDescriptorImageInfo) {
			.sampler = lighting_pass.linear_sampler,
			.imageView = in_gbuffer_outputs[attachment_idx].view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
	}

	VkDescriptorBufferInfo light_buffer_infos[] = {
		{ .buffer = in_point_lights_buffer, .offset = 0, .range = VK_WHOLE_SIZE },
		{ .buffer = in_spot_lights_buffer, .offset = 0, .range = VK_WHOLE_SIZE },
		{ .buffer = in_sun_lights_buffer, .offset = 0, .range = VK_WHOLE_SIZE },
	};

	VkWriteDescriptorSet writes[9] = {};
	u32 write_count = 0;

	writes[write_count++] = (VkWriteDescriptorSet) {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = lighting_pass.sets[frame_index],
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &ubo_info,
	};

	for (u32 attachment_idx = 0; attachment_idx < 4; ++attachment_idx)
	{
		writes[write_count++] = (VkWriteDescriptorSet) {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = lighting_pass.sets[frame_index],
			.dstBinding = 1 + attachment_idx,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &gbuffer_infos[attachment_idx],
		};
	}

	VkDescriptorImageInfo shadow_info = {};
	if (in_shadow_moments_view != VK_NULL_HANDLE)
	{
		shadow_info = (VkDescriptorImageInfo) {
			.sampler = lighting_pass.linear_sampler,
			.imageView = in_shadow_moments_view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		writes[write_count++] = (VkWriteDescriptorSet) {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = lighting_pass.sets[frame_index],
			.dstBinding = 5,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &shadow_info,
		};
	}

	for (u32 light_type_idx = 0; light_type_idx < 3; ++light_type_idx)
	{
		writes[write_count++] = (VkWriteDescriptorSet) {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = lighting_pass.sets[frame_index],
			.dstBinding = 6 + light_type_idx,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &light_buffer_infos[light_type_idx],
		};
	}

	vkUpdateDescriptorSets(ctx->device, write_count, writes, 0, nullptr);
}

// Bind + fullscreen draw (inside the framework's execute callback)
void lighting_pass_draw(VulkanContext* ctx)
{
	VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, lighting_pass.pipeline);
	vkCmdBindDescriptorSets(
		command_buffer,
		VK_PIPELINE_BIND_POINT_GRAPHICS,
		lighting_pass.pipeline_layout,
		0, 1, &lighting_pass.sets[ctx->frame_index],
		0, nullptr
	);
	vkCmdDraw(command_buffer, 3, 1, 0, 0);
}

void lighting_pass_shutdown(VulkanContext* ctx)
{
	for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
	{
		lighting_pass.fs_params_ubos[frame_idx].destroy_gpu_buffer();
	}
	vkDestroyPipeline(ctx->device, lighting_pass.pipeline, nullptr);
	vkDestroyPipelineLayout(ctx->device, lighting_pass.pipeline_layout, nullptr);
	vkDestroyDescriptorPool(ctx->device, lighting_pass.pool, nullptr);
	vkDestroyDescriptorSetLayout(ctx->device, lighting_pass.set_layout, nullptr);
}
