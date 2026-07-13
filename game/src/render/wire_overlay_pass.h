#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/shader_module.h"
#include "render/frame_data.h"
#include "render/gpu_buffer.h"
#include "render/culling.h"

// Shaded wireframe overlay (port of game/'s wire_overlay_pass.h +
// wire_overlay.glsl): copies the current scene color, then alpha-blends
// anti-aliased triangle edges on top. Wires only draw where the G-buffer
// says the surface is visible. Skinned meshes use the compute-baked cache;
// tessellated meshes use the generated render view.

// Mirrors wire_overlay_mesh.frag's mesh_fs_params (std140)
struct WireOverlayMeshFsParams
{
	HMM_Vec4 color;
	HMM_Vec4 camera_position;
	HMM_Vec4 camera_forward;
	HMM_Vec2 screen_size;
	f32 width;
	f32 softness;
	f32 opacity;
	f32 visibility_tolerance;
	f32 _pad0[2];
};
static_assert(sizeof(WireOverlayMeshFsParams) == 80, "Must match mesh_fs_params std140 layout");

namespace WireOverlayPass
{
	inline VkDescriptorSetLayout fs_set_layout = VK_NULL_HANDLE;	// set 1: fs UBO + position CIS
	inline VkDescriptorSetLayout mesh_set_layout = VK_NULL_HANDLE;	// set 2: vertex + index SSBOs
	inline VkDescriptorPool static_pool = VK_NULL_HANDLE;
	inline VkDescriptorPool mesh_pools[MAX_FRAMES_IN_FLIGHT] = {};	// reset each frame
	inline VkDescriptorSet copy_input_sets[MAX_FRAMES_IN_FLIGHT] = {};
	inline VkDescriptorSet fs_sets[MAX_FRAMES_IN_FLIGHT] = {};

	inline VkPipelineLayout copy_pipeline_layout = VK_NULL_HANDLE;
	inline VkPipelineLayout mesh_pipeline_layout = VK_NULL_HANDLE;
	inline VkPipeline copy_pipeline = VK_NULL_HANDLE;
	inline VkPipeline mesh_pipeline = VK_NULL_HANDLE;

	inline GpuBuffer<WireOverlayMeshFsParams> fs_params_ubos[MAX_FRAMES_IN_FLIGHT];

	inline VkSampler linear_sampler = VK_NULL_HANDLE;	// borrowed from frame_data
	inline VkSampler nearest_sampler = VK_NULL_HANDLE;	// owned (game/ samples position with nearest)

	constexpr u32 MAX_WIRE_MESHES_PER_FRAME = 1024;

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

		// Set 1 layout: b0 fs UBO, b1 position CIS
		{
			VkDescriptorSetLayoutBinding bindings[2] = {
				{
					.binding = 0,
					.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					.binding = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
			};
			VkDescriptorSetLayoutCreateInfo layout_create_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.bindingCount = 2,
				.pBindings = bindings,
			};
			VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &layout_create_info, nullptr, &fs_set_layout));
		}

		// Set 2 layout: b0 vertices SSBO, b1 indices SSBO (per mesh)
		{
			VkDescriptorSetLayoutBinding bindings[2] = {
				{
					.binding = 0,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
				},
				{
					.binding = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
				},
			};
			VkDescriptorSetLayoutCreateInfo layout_create_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.bindingCount = 2,
				.pBindings = bindings,
			};
			VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &layout_create_info, nullptr, &mesh_set_layout));
		}

		// Static pool: copy input (layout B) + fs sets, per frame in flight
		{
			VkDescriptorPoolSize pool_sizes[] = {
				{ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1 * MAX_FRAMES_IN_FLIGHT },
				{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 2 * MAX_FRAMES_IN_FLIGHT },
			};
			VkDescriptorPoolCreateInfo pool_create_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.maxSets = 2 * MAX_FRAMES_IN_FLIGHT,
				.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]),
				.pPoolSizes = pool_sizes,
			};
			VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_create_info, nullptr, &static_pool));

			for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
			{
				VkDescriptorSetAllocateInfo allocate_info = {
					.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
					.descriptorPool = static_pool,
					.descriptorSetCount = 1,
					.pSetLayouts = &frame_data.sampled_input_layout,
				};
				VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &copy_input_sets[frame_idx]));

				allocate_info.pSetLayouts = &fs_set_layout;
				VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &fs_sets[frame_idx]));

				fs_params_ubos[frame_idx] = GpuBuffer((GpuBufferDesc<WireOverlayMeshFsParams>){
					.data = nullptr,
					.size = sizeof(WireOverlayMeshFsParams),
					.usage = { .uniform_buffer = true, .stream_update = true },
					.label = "WireOverlayPass::fs_params",
				});
			}
		}

		// Per-frame mesh pools (reset each frame; sets are written during
		// recording, which is legal — they're fresh allocations no in-flight
		// command buffer references)
		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			VkDescriptorPoolSize pool_size = {
				.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 2 * MAX_WIRE_MESHES_PER_FRAME,
			};
			VkDescriptorPoolCreateInfo pool_create_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.maxSets = MAX_WIRE_MESHES_PER_FRAME,
				.poolSizeCount = 1,
				.pPoolSizes = &pool_size,
			};
			VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_create_info, nullptr, &mesh_pools[frame_idx]));
		}

		// Copy pipeline: layout B, fullscreen passthrough into the wire target
		{
			VkPipelineLayoutCreateInfo layout_create_info = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 1,
				.pSetLayouts = &frame_data.sampled_input_layout,
			};
			VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &copy_pipeline_layout));
		}

		// Mesh pipeline: [set0 = layout A, set1 = fs, set2 = mesh SSBOs]
		{
			VkDescriptorSetLayout set_layouts[] = {
				frame_data.per_frame_layout,
				fs_set_layout,
				mesh_set_layout,
			};
			VkPushConstantRange push_constant_range = {
				.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
				.offset = 0,
				.size = sizeof(i32),
			};
			VkPipelineLayoutCreateInfo layout_create_info = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 3,
				.pSetLayouts = set_layouts,
				.pushConstantRangeCount = 1,
				.pPushConstantRanges = &push_constant_range,
			};
			VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &mesh_pipeline_layout));
		}

		const auto create_wire_pipeline = [ctx](
			VkPipelineLayout in_layout,
			const char* in_vert_path,
			const char* in_frag_path,
			bool in_blend
		) -> VkPipeline
		{
			VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, in_vert_path);
			VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, in_frag_path);

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
			// game/ blend: src_alpha/one_minus_src_alpha rgb, one/one_minus alpha
			VkPipelineColorBlendAttachmentState blend_attachment = {
				.blendEnable = in_blend ? VK_TRUE : VK_FALSE,
				.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
				.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
				.colorBlendOp = VK_BLEND_OP_ADD,
				.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
				.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
				.alphaBlendOp = VK_BLEND_OP_ADD,
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
				.layout = in_layout,
				.renderPass = VK_NULL_HANDLE,
			};
			VkPipeline out_pipeline = VK_NULL_HANDLE;
			VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_create_info, &out_pipeline));

			vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
			vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
			return out_pipeline;
		};

		copy_pipeline = create_wire_pipeline(
			copy_pipeline_layout,
			"bin/shaders/wire_overlay_copy.vert.spv",
			"bin/shaders/wire_overlay_copy.frag.spv",
			/*in_blend*/ false
		);
		mesh_pipeline = create_wire_pipeline(
			mesh_pipeline_layout,
			"bin/shaders/wire_overlay_mesh.vert.spv",
			"bin/shaders/wire_overlay_mesh.frag.spv",
			/*in_blend*/ true
		);
	}

	// After the fence wait, before any binds record: writes the copy input +
	// fs sets, uploads fs params, and resets this frame's mesh pool
	inline void update(
		VulkanContext* ctx,
		const WireOverlayMeshFsParams& in_fs_params,
		VkImageView in_source_color_view,
		VkImageView in_gbuffer_position_view
	)
	{
		const u32 frame_index = ctx->frame_index;

		VK_CHECK(vkResetDescriptorPool(ctx->device, mesh_pools[frame_index], 0));

		fs_params_ubos[frame_index].update_gpu_buffer(&in_fs_params, sizeof(in_fs_params));

		VkDescriptorBufferInfo ubo_info = {
			.buffer = fs_params_ubos[frame_index].get_gpu_buffer(),
			.offset = 0,
			.range = sizeof(WireOverlayMeshFsParams),
		};
		VkDescriptorImageInfo image_infos[] = {
			{
				.sampler = linear_sampler,
				.imageView = in_source_color_view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			},
			{
				.sampler = nearest_sampler,
				.imageView = in_gbuffer_position_view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			},
		};

		VkWriteDescriptorSet writes[] = {
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = copy_input_sets[frame_index],
				.dstBinding = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image_infos[0],
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = fs_sets[frame_index],
				.dstBinding = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo = &ubo_info,
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = fs_sets[frame_index],
				.dstBinding = 1,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image_infos[1],
			},
		};
		vulkan_update_descriptor_sets(ctx, 3, writes);
	}

	// Runs inside the WireOverlay pass execute callback: base copy, then the
	// alpha-blended wires for every visible static mesh
	inline void draw(VulkanContext* ctx, State& in_state, const HMM_Mat4& in_view_projection)
	{
		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];
		const u32 frame_index = ctx->frame_index;

		// Base copy
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, copy_pipeline);
		vkCmdBindDescriptorSets(
			command_buffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			copy_pipeline_layout,
			0, 1, &copy_input_sets[frame_index],
			0, nullptr
		);
		vulkan_cmd_draw(ctx, 3, 1, 0, 0);

		if (!in_state.render_objects.valid)
		{
			return;
		}

		// Wire meshes
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline);
		VkDescriptorSet frame_sets[] = {
			frame_data.per_frame_sets[frame_index],
			fs_sets[frame_index],
		};
		vkCmdBindDescriptorSets(
			command_buffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			mesh_pipeline_layout,
			0, 2, frame_sets,
			0, nullptr
		);

		CullResult cull_result = cull_objects(in_state, in_view_projection, 0.0f);
		u32 drawn_mesh_count = 0;
		for (i32 object_id : cull_result.object_ids)
		{
			auto found = in_state.scene.objects.find(object_id);
			if (found == in_state.scene.objects.end())
			{
				continue;
			}

			Object& object = found->second;
			if (object.render_object_index < 0)
			{
				continue;
			}

			Mesh& mesh = object.mesh;
			MeshRenderView render_view = mesh_get_render_view(mesh);
			if (render_view.index_count == 0)
			{
				continue;
			}
			// Skinned wires draw from the compute-baked vertex cache
			if (mesh.has_skinned_vertices && !mesh.skinned_vertex_cache_valid)
			{
				continue;
			}
			if (drawn_mesh_count >= MAX_WIRE_MESHES_PER_FRAME)
			{
				break;
			}

			VkDescriptorSet mesh_set = VK_NULL_HANDLE;
			VkDescriptorSetAllocateInfo allocate_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = mesh_pools[frame_index],
				.descriptorSetCount = 1,
				.pSetLayouts = &mesh_set_layout,
			};
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &mesh_set));

			VkDescriptorBufferInfo buffer_infos[] = {
				{ .buffer = render_view.vertex_buffer, .offset = 0, .range = VK_WHOLE_SIZE },
				{ .buffer = render_view.index_buffer, .offset = 0, .range = VK_WHOLE_SIZE },
			};
			VkWriteDescriptorSet writes[] = {
				{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = mesh_set,
					.dstBinding = 0,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.pBufferInfo = &buffer_infos[0],
				},
				{
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = mesh_set,
					.dstBinding = 1,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.pBufferInfo = &buffer_infos[1],
				},
			};
			vulkan_update_descriptor_sets(ctx, 2, writes);

			vkCmdBindDescriptorSets(
				command_buffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				mesh_pipeline_layout,
				2, 1, &mesh_set,
				0, nullptr
			);
			vkCmdPushConstants(
				command_buffer,
				mesh_pipeline_layout,
				VK_SHADER_STAGE_VERTEX_BIT,
				0, sizeof(i32), &object.render_object_index
			);
			vulkan_cmd_draw(ctx, render_view.index_count, 1, 0, 0);
			drawn_mesh_count += 1;
		}
	}

	inline void shutdown(VulkanContext* ctx)
	{
		vkDestroyPipeline(ctx->device, mesh_pipeline, nullptr);
		vkDestroyPipeline(ctx->device, copy_pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, mesh_pipeline_layout, nullptr);
		vkDestroyPipelineLayout(ctx->device, copy_pipeline_layout, nullptr);
		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			fs_params_ubos[frame_idx].destroy_gpu_buffer();
			vkDestroyDescriptorPool(ctx->device, mesh_pools[frame_idx], nullptr);
		}
		vkDestroyDescriptorPool(ctx->device, static_pool, nullptr);
		vkDestroyDescriptorSetLayout(ctx->device, mesh_set_layout, nullptr);
		vkDestroyDescriptorSetLayout(ctx->device, fs_set_layout, nullptr);
		vkDestroySampler(ctx->device, nearest_sampler, nullptr);
	}
}
