#pragma once

#include "render/core/shader_module.h"
#include "render/passes/gi/gi.h"

namespace GIDebugPass
{
	struct PushConstants
	{
		HMM_Mat4 view_projection;
		i32 octree_depth = 0;
		f32 probe_debug_radius = 0.1f;
		i32 atlas_total_size = 0;
		i32 atlas_entry_size = 0;
		i32 probe_vis_mode = 0;
		i32 isolated_probe_index = -1;
		i32 specular_atlas_total_size = 0;
		i32 specular_atlas_entry_size = 0;
		i32 specular_mip_count = 0;
		f32 specular_debug_roughness = 0.0f;
		i32 probe_level_filter_enable = 0;
		i32 probe_level_filter_selection = 0;
	};
	static_assert(sizeof(PushConstants) == 112, "GI debug push constants mismatch");

	inline VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	inline VkDescriptorPool pool = VK_NULL_HANDLE;
	inline VkDescriptorSet sets[MAX_FRAMES_IN_FLIGHT] = {};
	inline VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	inline VkPipeline pipeline = VK_NULL_HANDLE;
	inline Mesh sphere_mesh;

	inline void init(VulkanContext* ctx)
	{
		// Binding layout must match gi_debug.vert/frag.
		VkDescriptorSetLayoutBinding bindings[] = {
			{
				.binding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
			},
			{
				.binding = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			},
			{
				.binding = 2,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			},
			{
				.binding = 3,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			},
			{
				.binding = 4,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			},
			{
				.binding = 5,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			},
			{
				.binding = 6,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			},
		};
		VkDescriptorSetLayoutCreateInfo set_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = sizeof(bindings) / sizeof(bindings[0]),
			.pBindings = bindings,
		};
		VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &set_info, nullptr, &set_layout));

		VkDescriptorPoolSize pool_sizes[] = {
			{ .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 4 * MAX_FRAMES_IN_FLIGHT },
			{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 3 * MAX_FRAMES_IN_FLIGHT },
		};
		VkDescriptorPoolCreateInfo pool_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = MAX_FRAMES_IN_FLIGHT,
			.poolSizeCount = 2,
			.pPoolSizes = pool_sizes,
		};
		VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_info, nullptr, &pool));
		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			VkDescriptorSetAllocateInfo allocate_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &set_layout,
			};
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &sets[frame_idx]));
		}

		VkPushConstantRange push_range = {
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			.offset = 0,
			.size = sizeof(PushConstants),
		};
		VkPipelineLayoutCreateInfo layout_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &set_layout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_range,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_info, nullptr, &pipeline_layout));

		VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, "bin/shaders/gi_debug.vert.spv");
		VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/gi_debug.frag.spv");
		VkPipelineShaderStageCreateInfo stages[] = {
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
		VkVertexInputBindingDescription vertex_binding = {
			.binding = 0,
			.stride = sizeof(Vertex),
			.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		};
		VkVertexInputAttributeDescription attributes[] = {
			{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, position) },
			{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, normal) },
		};
		VkPipelineVertexInputStateCreateInfo vertex_input = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = 1,
			.pVertexBindingDescriptions = &vertex_binding,
			.vertexAttributeDescriptionCount = 2,
			.pVertexAttributeDescriptions = attributes,
		};
		VkPipelineInputAssemblyStateCreateInfo input_assembly = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
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
			.depthTestEnable = VK_TRUE,
			.depthWriteEnable = VK_TRUE,
			.depthCompareOp = Render::DEPTH_COMPARE_OP,
		};

		// Probe spheres write straight into the four G-buffer targets.
		constexpr u32 gbuffer_count = 4;
		VkPipelineColorBlendAttachmentState blend_attachments[gbuffer_count] = {};
		VkFormat color_formats[gbuffer_count] = {};
		for (u32 attachment_idx = 0; attachment_idx < gbuffer_count; ++attachment_idx)
		{
			blend_attachments[attachment_idx].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
				| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			color_formats[attachment_idx] = Render::GBUFFER_FORMAT;
		}
		VkPipelineColorBlendStateCreateInfo color_blending = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = gbuffer_count,
			.pAttachments = blend_attachments,
		};
		VkPipelineRenderingCreateInfo rendering = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = gbuffer_count,
			.pColorAttachmentFormats = color_formats,
			.depthAttachmentFormat = Render::SCENE_DEPTH_FORMAT,
		};
		VkGraphicsPipelineCreateInfo pipeline_info = {
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.pNext = &rendering,
			.stageCount = 2,
			.pStages = stages,
			.pVertexInputState = &vertex_input,
			.pInputAssemblyState = &input_assembly,
			.pViewportState = &viewport,
			.pRasterizationState = &rasterization,
			.pMultisampleState = &multisampling,
			.pDepthStencilState = &depth_stencil,
			.pColorBlendState = &color_blending,
			.pDynamicState = &dynamic_state,
			.layout = pipeline_layout,
		};
		VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_info, &pipeline));
		vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
		vkDestroyShaderModule(ctx->device, fragment_module, nullptr);

		sphere_mesh = make_mesh(mesh_init_data_uv_sphere(1.0f, 12, 16));
		sphere_mesh.vertex_buffer.get_gpu_buffer();
		sphere_mesh.index_buffer.get_gpu_buffer();
	}

	inline void draw(VulkanContext* ctx, GI_Scene& gi_scene, State& state, const HMM_Mat4& view_projection)
	{
		if (!state.gi.show_probes || gi_scene.probes.empty())
		{
			return;
		}

		const VkBuffer probes_buffer = gi_scene.probes_buffer.get_gpu_buffer();
		VkDescriptorBufferInfo probes_info = { .buffer = probes_buffer, .range = VK_WHOLE_SIZE };
		VkDescriptorBufferInfo sh9_info = { .buffer = gi_scene.sh9_coefficients_buffer.get_gpu_buffer(), .range = VK_WHOLE_SIZE };
		VkDescriptorBufferInfo sg9_info = { .buffer = gi_scene.sg9_lobes_buffer.get_gpu_buffer(), .range = VK_WHOLE_SIZE };
		VkDescriptorImageInfo lighting_info = {
			.sampler = frame_data.linear_sampler,
			.imageView = gi_scene_get_octahedral_lighting_view(gi_scene),
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		VkDescriptorImageInfo depth_info = {
			.sampler = frame_data.linear_sampler,
			.imageView = gi_scene_get_octahedral_depth_view(gi_scene),
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		VkDescriptorImageInfo specular_info = {
			.sampler = gi_scene.lighting_capture.specular_sampler,
			.imageView = gi_scene_get_specular_lighting_view(gi_scene),
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};

		const VkDescriptorSet set = sets[ctx->frame_index];
		auto buffer_write = [&](u32 in_binding, const VkDescriptorBufferInfo* in_info) {
			return VkWriteDescriptorSet {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = set,
				.dstBinding = in_binding,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = in_info,
			};
		};
		auto image_write = [&](u32 in_binding, const VkDescriptorImageInfo* in_info) {
			return VkWriteDescriptorSet {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = set,
				.dstBinding = in_binding,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = in_info,
			};
		};
		// Bindings 0 (vertex) and 3 (fragment) both read the probe buffer.
		VkWriteDescriptorSet writes[] = {
			buffer_write(0, &probes_info),
			image_write(1, &lighting_info),
			image_write(2, &depth_info),
			buffer_write(3, &probes_info),
			buffer_write(4, &sh9_info),
			buffer_write(5, &sg9_info),
			image_write(6, &specular_info),
		};
		vulkan_update_descriptor_sets(ctx, sizeof(writes) / sizeof(writes[0]), writes);

		PushConstants params = {
			.view_projection = view_projection,
			.octree_depth = gi_scene.octree_depth,
			.probe_debug_radius = gi_scene_debug_probe_radius(gi_scene),
			.atlas_total_size = GI_Scene::atlas_total_size,
			.atlas_entry_size = GI_Scene::atlas_entry_size,
			.probe_vis_mode = (i32) state.gi.probe_vis_mode,
			.isolated_probe_index = state.gi.probe_isolation_enable ? state.gi.isolated_probe_index : -1,
			.specular_atlas_total_size = gi_scene.lighting_capture.specular_atlas_total_size,
			.specular_atlas_entry_size = gi_scene.lighting_capture.desc.specular_entry_size,
			.specular_mip_count = gi_scene.lighting_capture.desc.specular_mip_count,
			.specular_debug_roughness = state.gi.specular_debug_roughness,
			.probe_level_filter_enable = state.gi.probe_level_filter_enable ? 1 : 0,
			.probe_level_filter_selection = std::clamp(state.gi.probe_level_filter_selection, 0, gi_scene.octree_depth + 1),
		};

		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vkCmdBindDescriptorSets(
			command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
			0, 1, &set, 0, nullptr);
		vkCmdPushConstants(
			command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			0, sizeof(params), &params);
		VkBuffer vertex_buffer = sphere_mesh.vertex_buffer.get_gpu_buffer();
		VkDeviceSize vertex_offset = 0;
		vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer, &vertex_offset);
		vkCmdBindIndexBuffer(command_buffer, sphere_mesh.index_buffer.get_gpu_buffer(), 0, VK_INDEX_TYPE_UINT32);
		vulkan_cmd_draw_indexed(ctx, sphere_mesh.index_count, (u32) gi_scene.probes.length(), 0, 0, 0);
	}

	inline void shutdown(VulkanContext* ctx)
	{
		free(sphere_mesh.vertices);
		free(sphere_mesh.indices);
		free(sphere_mesh.wire_indices);
		free(sphere_mesh.material_indices);
		sphere_mesh.vertex_buffer.destroy_gpu_buffer();
		sphere_mesh.index_buffer.destroy_gpu_buffer();
		sphere_mesh.wire_index_buffer.destroy_gpu_buffer();

		vkDestroyPipeline(ctx->device, pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, pipeline_layout, nullptr);
		vkDestroyDescriptorPool(ctx->device, pool, nullptr);
		vkDestroyDescriptorSetLayout(ctx->device, set_layout, nullptr);
	}
}
