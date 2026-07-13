#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/render_pass.h"
#include "render/shader_module.h"
#include "render/frame_data.h"
#include "render/geometry_pass.h"
#include "render/lighting_pass.h"
#include "render/sky_pass.h"
#include "render/culling.h"
#include "state/state.h"

// Probe lighting capture (port of game/src/render/lighting_capture.h):
// renders the scene's G-buffer for 6 cube faces (Multi pass), lights each
// face into a cubemap, renders radial-depth moments into a second cubemap,
// projects both into one entry of the padded octahedral atlas, and
// optionally projects SH9/SG9 radiance. Runs during command recording (a
// few probes per frame) before the main pass chain.
//
// Vulkan adaptations: capture geometry/sky use push-constant cameras (the
// per-frame UBO belongs to the main view); radial depth + cube_to_oct params
// fit in push constants; capture lighting reuses layout C + lighting.frag
// with an RGBA32F pipeline variant and per-frame fs_params slot rings.

struct AtlasViewport
{
	int x, y, w, h;
};

inline AtlasViewport get_atlas_viewport(int atlas_size, int render_size, int idx)
{
	assert(atlas_size > 0 && render_size > 0);
	assert(atlas_size >= render_size);
	assert(atlas_size % render_size == 0);

	const int slots_per_dim = atlas_size / render_size;
	const int total_slots = slots_per_dim * slots_per_dim;
	assert(idx >= 0 && idx < total_slots);

	const int grid_x = idx % slots_per_dim;
	const int grid_y = idx / slots_per_dim;

	return { grid_x * render_size, grid_y * render_size, render_size, render_size };
}

// Metal/D3D-convention cube face bases (game/ render_types.h:57-64)
inline const HMM_Vec3 CUBE_FORWARD_AND_UP[NUM_CUBE_FACES][2] = {
	{ {  1.0f,  0.0f,  0.0f }, {  0.0f, -1.0f,  0.0f } },	// +X
	{ { -1.0f,  0.0f,  0.0f }, {  0.0f, -1.0f,  0.0f } },	// -X
	{ {  0.0f,  1.0f,  0.0f }, {  0.0f,  0.0f,  1.0f } },	// +Y
	{ {  0.0f, -1.0f,  0.0f }, {  0.0f,  0.0f, -1.0f } },	// -Y
	{ {  0.0f,  0.0f,  1.0f }, {  0.0f, -1.0f,  0.0f } },	// +Z
	{ {  0.0f,  0.0f, -1.0f }, {  0.0f, -1.0f,  0.0f } },	// -Z
};

struct LightingCaptureDesc
{
	i32 cubemap_render_size = 256;
	i32 octahedral_total_size = 1024;
	i32 octahedral_entry_size = 16;
};

// Mirrors geometry_capture.vert's push constants
struct CaptureGeometryPushConstants
{
	HMM_Mat4 view_projection;
	i32 object_index;
	i32 skin_matrix_offset;
};

// Mirrors sky_capture.frag's push constants
struct CaptureSkyPushConstants
{
	HMM_Mat4 inv_view_projection;
	HMM_Vec4 capture_position;
};

// Mirrors radial_depth.frag's fs_params (used as push constants here)
struct RadialDepthPushConstants
{
	HMM_Mat4 inverse_view_projection;
	HMM_Vec3 capture_location;
	i32 probe_occlusion_mode;
	i32 force_fully_visible;
	f32 max_radial_depth;
	f32 _pad0[2];
};
static_assert(sizeof(RadialDepthPushConstants) == 96, "Must match radial_depth.frag fs_params layout");

// Mirrors cubemap_to_octahedral.frag's fs_params (used as push constants)
struct CubeToOctPushConstants
{
	i32 cubemap_render_size;
	i32 atlas_entry_size;
	i32 compute_irradiance;
	i32 use_importance_sampling;
};

// Mirrors probe_radiance_projection.comp's push constants
struct ProbeProjectionPushConstants
{
	i32 probe_index;
	i32 radiance_mode;
	i32 sample_count;
	i32 _padding0;
};

struct LightingCapture
{
	LightingCaptureDesc desc;

	RenderPass geometry_pass;		// Multi: 6 face G-buffers
	RenderPass lighting_pass;		// Cubemap: lit faces
	RenderPass radial_depth_pass;	// Cubemap: distance moments
	RenderPass cube_to_oct_pass;	// Single: padded octahedral atlas (2 MRT)

	// Capture geometry (push-constant camera, layout A)
	VkPipelineLayout capture_geometry_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline capture_geometry_pipeline = VK_NULL_HANDLE;
	VkPipeline capture_geometry_skinned_pipeline = VK_NULL_HANDLE;

	// Capture sky (layout B set + push constants)
	VkPipelineLayout capture_sky_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline capture_sky_pipeline = VK_NULL_HANDLE;
	VkDescriptorSet sky_input_sets[MAX_FRAMES_IN_FLIGHT] = {};

	// Capture lighting: layout C reused; RGBA32F pipeline variant + slot ring
	VkPipeline capture_lighting_pipeline = VK_NULL_HANDLE;
	static constexpr i32 LIGHTING_SLOTS_PER_FRAME = NUM_CUBE_FACES * 4;	// probes_to_update_per_frame
	GpuBuffer<LightingFsParams> lighting_slot_ubos[MAX_FRAMES_IN_FLIGHT][LIGHTING_SLOTS_PER_FRAME];
	VkDescriptorSet lighting_slot_sets[MAX_FRAMES_IN_FLIGHT][LIGHTING_SLOTS_PER_FRAME] = {};
	i32 lighting_slot_cursor = 0;

	// Radial depth (push constants + per-frame input set)
	VkDescriptorSetLayout radial_depth_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout radial_depth_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline radial_depth_pipeline = VK_NULL_HANDLE;
	static constexpr i32 RADIAL_SLOTS_PER_FRAME = NUM_CUBE_FACES * 4;
	VkDescriptorSet radial_slot_sets[MAX_FRAMES_IN_FLIGHT][RADIAL_SLOTS_PER_FRAME] = {};
	i32 radial_slot_cursor = 0;

	// Cube -> octahedral (static inputs: the capture cubemaps never resize)
	VkDescriptorSetLayout cube_to_oct_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout cube_to_oct_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline cube_to_oct_pipeline = VK_NULL_HANDLE;
	VkDescriptorSet cube_to_oct_set = VK_NULL_HANDLE;

	// Probe radiance projection compute (per-frame sets: sh9/sg9 buffers
	// change on layout rebuilds)
	VkDescriptorSetLayout projection_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout projection_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline projection_pipeline = VK_NULL_HANDLE;
	static constexpr i32 PROJECTION_SLOTS_PER_FRAME = 8;	// <=2 modes * 4 probes
	VkDescriptorSet projection_slot_sets[MAX_FRAMES_IN_FLIGHT][PROJECTION_SLOTS_PER_FRAME] = {};
	i32 projection_slot_cursor = 0;

	VkDescriptorPool pool = VK_NULL_HANDLE;

	// 1x1 white fallback for unused lighting bindings (ssao/contact shadows)
	GpuImage default_image = {};
	GpuImage default_array_image = {};

	bool is_initialized = false;

	static constexpr VkFormat color_format = VK_FORMAT_R32G32B32A32_SFLOAT;

	VkPipeline create_fullscreen_pipeline(
		VulkanContext* ctx,
		VkPipelineLayout in_layout,
		const char* in_vert_path,
		const char* in_frag_path,
		const VkFormat* in_color_formats,
		u32 in_color_count,
		VkFormat in_depth_format,
		bool in_depth_write
	)
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
			.depthTestEnable = in_depth_format != VK_FORMAT_UNDEFINED ? VK_TRUE : VK_FALSE,
			.depthWriteEnable = in_depth_write ? VK_TRUE : VK_FALSE,
			.depthCompareOp = Render::DEPTH_COMPARE_OP,
		};
		VkPipelineColorBlendAttachmentState blend_attachments[RENDER_PASS_MAX_COLOR_OUTPUTS] = {};
		for (u32 attachment_idx = 0; attachment_idx < in_color_count; ++attachment_idx)
		{
			blend_attachments[attachment_idx] = (VkPipelineColorBlendAttachmentState) {
				.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
								| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
			};
		}
		VkPipelineColorBlendStateCreateInfo color_blending = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = in_color_count,
			.pAttachments = blend_attachments,
		};

		VkPipelineRenderingCreateInfo rendering_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = in_color_count,
			.pColorAttachmentFormats = in_color_formats,
			.depthAttachmentFormat = in_depth_format,
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
	}

	VkPipeline create_capture_geometry_pipeline(VulkanContext* ctx, const char* in_vert_path, bool in_skinned)
	{
		VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, in_vert_path);
		VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/geometry_capture.frag.spv");

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

		VkVertexInputBindingDescription vertex_bindings[] = {
			{ .binding = 0, .stride = sizeof(Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
			{ .binding = 1, .stride = sizeof(SkinnedVertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
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
		VkPipelineColorBlendAttachmentState blend_attachments[4] = {};
		for (u32 attachment_idx = 0; attachment_idx < 4; ++attachment_idx)
		{
			blend_attachments[attachment_idx] = (VkPipelineColorBlendAttachmentState) {
				.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
								| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
			};
		}
		VkPipelineColorBlendStateCreateInfo color_blending = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 4,
			.pAttachments = blend_attachments,
		};

		VkFormat gbuffer_formats[4] = {
			Render::GBUFFER_FORMAT, Render::GBUFFER_FORMAT,
			Render::GBUFFER_FORMAT, Render::GBUFFER_FORMAT,
		};
		VkPipelineRenderingCreateInfo rendering_create_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = 4,
			.pColorAttachmentFormats = gbuffer_formats,
			.depthAttachmentFormat = Render::SCENE_DEPTH_FORMAT,
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
			.layout = capture_geometry_pipeline_layout,
			.renderPass = VK_NULL_HANDLE,
		};
		VkPipeline out_pipeline = VK_NULL_HANDLE;
		VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_create_info, &out_pipeline));

		vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
		vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
		return out_pipeline;
	}

	void init(VulkanContext* ctx, const LightingCaptureDesc& in_desc)
	{
		assert(!is_initialized);
		desc = in_desc;

		// ---- Passes ----
		geometry_pass.init((RenderPassDesc) {
			.initial_width = desc.cubemap_render_size,
			.initial_height = desc.cubemap_render_size,
			.pass_count = NUM_CUBE_FACES,
			.num_outputs = Render::GBUFFER_OUTPUT_COUNT,
			.outputs = {
				{ .format = Render::GBUFFER_FORMAT, .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR, .store_op = VK_ATTACHMENT_STORE_OP_STORE, .clear_value = {{{ 0.0f, 0.0f, 0.0f, 1.0f }}} },
				{ .format = Render::GBUFFER_FORMAT, .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR, .store_op = VK_ATTACHMENT_STORE_OP_STORE, .clear_value = {{{ 0.0f, 0.0f, 0.0f, 0.0f }}} },
				{ .format = Render::GBUFFER_FORMAT, .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR, .store_op = VK_ATTACHMENT_STORE_OP_STORE, .clear_value = {{{ 0.0f, 0.0f, 0.0f, 0.0f }}} },
				{ .format = Render::GBUFFER_FORMAT, .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR, .store_op = VK_ATTACHMENT_STORE_OP_STORE, .clear_value = {{{ 0.0f, 0.0f, 0.0f, 0.0f }}} },
			},
			.depth_output = {
				.format = Render::SCENE_DEPTH_FORMAT,
				.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
				.store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.clear_value = { .depthStencil = { .depth = Render::DEPTH_CLEAR_VALUE } },
			},
			.resize_with_window = false,
			.type = ERenderPassType::Multi,
			.debug_label = "GI Capture Geometry",
		});

		lighting_pass.init((RenderPassDesc) {
			.initial_width = desc.cubemap_render_size,
			.initial_height = desc.cubemap_render_size,
			.num_outputs = 1,
			.outputs = {
				{ .format = color_format, .load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
			},
			.resize_with_window = false,
			.type = ERenderPassType::Cubemap,
			.debug_label = "GI Capture Lighting",
		});

		radial_depth_pass.init((RenderPassDesc) {
			.initial_width = desc.cubemap_render_size,
			.initial_height = desc.cubemap_render_size,
			.num_outputs = 1,
			.outputs = {
				{ .format = color_format, .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR, .store_op = VK_ATTACHMENT_STORE_OP_STORE, .clear_value = {{{ 1.0f, 1.0f, 1.0f, 1.0f }}} },
			},
			.resize_with_window = false,
			.type = ERenderPassType::Cubemap,
			.debug_label = "GI Radial Depth",
		});

		cube_to_oct_pass.init((RenderPassDesc) {
			.initial_width = desc.octahedral_total_size,
			.initial_height = desc.octahedral_total_size,
			.num_outputs = 2,
			.outputs = {
				{ .format = color_format, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
				{ .format = color_format, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
			},
			.resize_with_window = false,
			.type = ERenderPassType::Single,
			.debug_label = "GI Cube To Octahedral",
		});

		// ---- Set layouts ----
		{
			VkDescriptorSetLayoutBinding bindings[3] = {};
			for (u32 binding_idx = 0; binding_idx < 2; ++binding_idx)
			{
				bindings[binding_idx] = (VkDescriptorSetLayoutBinding) {
					.binding = binding_idx,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				};
			}
			// cube_to_oct: b0 fs push constants live outside; b1 lighting cube,
			// b2 depth cube -> shift: b0/b1 here are bindings 1 and 2 in the
			// shader. Rebuild with explicit bindings 1 and 2:
			bindings[0].binding = 1;
			bindings[1].binding = 2;
			VkDescriptorSetLayoutCreateInfo layout_create_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.bindingCount = 2,
				.pBindings = bindings,
			};
			VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &layout_create_info, nullptr, &cube_to_oct_set_layout));
		}
		{
			VkDescriptorSetLayoutBinding bindings[3] = {
				{
					.binding = 0,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
				},
				{
					.binding = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
				},
				{
					.binding = 2,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
				},
			};
			VkDescriptorSetLayoutCreateInfo layout_create_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.bindingCount = 3,
				.pBindings = bindings,
			};
			VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &layout_create_info, nullptr, &projection_set_layout));
		}

		// ---- Pool + sets ----
		{
			VkDescriptorPoolSize pool_sizes[] = {
				{ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 128 },
				{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 512 },
				{ .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 512 },
			};
			VkDescriptorPoolCreateInfo pool_create_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.maxSets = 192,
				.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]),
				.pPoolSizes = pool_sizes,
			};
			VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_create_info, nullptr, &pool));
		}

		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			VkDescriptorSetAllocateInfo allocate_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = pool,
				.descriptorSetCount = 1,
			};

			allocate_info.pSetLayouts = &frame_data.sampled_input_layout;
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &sky_input_sets[frame_idx]));

			for (i32 slot_idx = 0; slot_idx < LIGHTING_SLOTS_PER_FRAME; ++slot_idx)
			{
				allocate_info.pSetLayouts = &::lighting_pass.set_layout;
				VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &lighting_slot_sets[frame_idx][slot_idx]));

				lighting_slot_ubos[frame_idx][slot_idx] = GpuBuffer((GpuBufferDesc<LightingFsParams>){
					.data = nullptr,
					.size = sizeof(LightingFsParams),
					.usage = { .uniform_buffer = true, .stream_update = true },
					.label = "LightingCapture::lighting_fs_params",
				});
			}
			for (i32 slot_idx = 0; slot_idx < RADIAL_SLOTS_PER_FRAME; ++slot_idx)
			{
				allocate_info.pSetLayouts = &frame_data.sampled_input_layout;
				VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &radial_slot_sets[frame_idx][slot_idx]));
			}
			for (i32 slot_idx = 0; slot_idx < PROJECTION_SLOTS_PER_FRAME; ++slot_idx)
			{
				allocate_info.pSetLayouts = &projection_set_layout;
				VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &projection_slot_sets[frame_idx][slot_idx]));
			}
		}

		{
			VkDescriptorSetAllocateInfo allocate_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &cube_to_oct_set_layout,
			};
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &cube_to_oct_set));
		}

		// ---- Pipeline layouts ----
		{
			VkPushConstantRange push_range = {
				.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
				.offset = 0,
				.size = sizeof(CaptureGeometryPushConstants),
			};
			VkPipelineLayoutCreateInfo layout_create_info = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 1,
				.pSetLayouts = &frame_data.per_frame_layout,
				.pushConstantRangeCount = 1,
				.pPushConstantRanges = &push_range,
			};
			VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &capture_geometry_pipeline_layout));
		}
		{
			VkPushConstantRange push_range = {
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				.offset = 0,
				.size = sizeof(CaptureSkyPushConstants),
			};
			VkPipelineLayoutCreateInfo layout_create_info = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 1,
				.pSetLayouts = &frame_data.sampled_input_layout,
				.pushConstantRangeCount = 1,
				.pPushConstantRanges = &push_range,
			};
			VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &capture_sky_pipeline_layout));
		}
		{
			VkPushConstantRange push_range = {
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				.offset = 0,
				.size = sizeof(RadialDepthPushConstants),
			};
			VkPipelineLayoutCreateInfo layout_create_info = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 1,
				.pSetLayouts = &frame_data.sampled_input_layout,
				.pushConstantRangeCount = 1,
				.pPushConstantRanges = &push_range,
			};
			VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &radial_depth_pipeline_layout));
		}
		{
			VkPushConstantRange push_range = {
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				.offset = 0,
				.size = sizeof(CubeToOctPushConstants),
			};
			VkPipelineLayoutCreateInfo layout_create_info = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 1,
				.pSetLayouts = &cube_to_oct_set_layout,
				.pushConstantRangeCount = 1,
				.pPushConstantRanges = &push_range,
			};
			VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &cube_to_oct_pipeline_layout));
		}
		{
			VkPushConstantRange push_range = {
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
				.offset = 0,
				.size = sizeof(ProbeProjectionPushConstants),
			};
			VkPipelineLayoutCreateInfo layout_create_info = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.setLayoutCount = 1,
				.pSetLayouts = &projection_set_layout,
				.pushConstantRangeCount = 1,
				.pPushConstantRanges = &push_range,
			};
			VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_create_info, nullptr, &projection_pipeline_layout));
		}

		// ---- Pipelines ----
		capture_geometry_pipeline = create_capture_geometry_pipeline(ctx, "bin/shaders/geometry_capture.vert.spv", false);
		capture_geometry_skinned_pipeline = create_capture_geometry_pipeline(ctx, "bin/shaders/geometry_capture_skinned.vert.spv", true);

		{
			VkFormat sky_formats[4] = {
				Render::GBUFFER_FORMAT, Render::GBUFFER_FORMAT,
				Render::GBUFFER_FORMAT, Render::GBUFFER_FORMAT,
			};
			capture_sky_pipeline = create_fullscreen_pipeline(
				ctx, capture_sky_pipeline_layout,
				"bin/shaders/sky_capture.vert.spv", "bin/shaders/sky_capture.frag.spv",
				sky_formats, 4, Render::SCENE_DEPTH_FORMAT, /*depth write*/ true
			);
		}
		{
			VkFormat lighting_formats[1] = { color_format };
			capture_lighting_pipeline = create_fullscreen_pipeline(
				ctx, ::lighting_pass.pipeline_layout,
				"bin/shaders/lighting.vert.spv", "bin/shaders/lighting.frag.spv",
				lighting_formats, 1, VK_FORMAT_UNDEFINED, false
			);
		}
		{
			VkFormat radial_formats[1] = { color_format };
			radial_depth_pipeline = create_fullscreen_pipeline(
				ctx, radial_depth_pipeline_layout,
				"bin/shaders/radial_depth.vert.spv", "bin/shaders/radial_depth.frag.spv",
				radial_formats, 1, VK_FORMAT_UNDEFINED, false
			);
		}
		{
			VkFormat oct_formats[2] = { color_format, color_format };
			cube_to_oct_pipeline = create_fullscreen_pipeline(
				ctx, cube_to_oct_pipeline_layout,
				"bin/shaders/cubemap_to_octahedral.vert.spv", "bin/shaders/cubemap_to_octahedral.frag.spv",
				oct_formats, 2, VK_FORMAT_UNDEFINED, false
			);
		}
		{
			VkShaderModule compute_module = create_shader_module_from_file(ctx->device, "bin/shaders/probe_radiance_projection.comp.spv");
			VkComputePipelineCreateInfo pipeline_create_info = {
				.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
				.stage = {
					.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
					.stage = VK_SHADER_STAGE_COMPUTE_BIT,
					.module = compute_module,
					.pName = "main",
				},
				.layout = projection_pipeline_layout,
			};
			VK_CHECK(vulkan_create_compute_pipelines(ctx, 1, &pipeline_create_info, &projection_pipeline));
			vkDestroyShaderModule(ctx->device, compute_module, nullptr);
		}

		// ---- Static resources ----
		const u32 white_pixel = 0xFFFFFFFFu;
		default_image = gpu_image_create_from_data(ctx, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, &white_pixel, sizeof(white_pixel));
		default_array_image = gpu_image_create(ctx->allocator, ctx->device, (GpuImageDesc) {
			.width = 1,
			.height = 1,
			.format = VK_FORMAT_R8G8B8A8_UNORM,
			.usage = VK_IMAGE_USAGE_SAMPLED_BIT,
			.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
			.array_layers = 2,
		});
		vulkan_context_immediate_submit(ctx, [&](VkCommandBuffer command_buffer)
		{
			gpu_image_transition(command_buffer, default_array_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true);
		});

		// Sky input sets: the baked octahedral sky image is fixed-size
		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			VkDescriptorImageInfo sky_info = {
				.sampler = frame_data.linear_sampler,
				.imageView = sky_pass.bake_render_pass.get_color_output(0).view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
			VkWriteDescriptorSet write = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = sky_input_sets[frame_idx],
				.dstBinding = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &sky_info,
			};
			vulkan_update_descriptor_sets(ctx, 1, &write);
		}

		// cube_to_oct inputs: the capture cubemaps never resize
		{
			VkDescriptorImageInfo image_infos[] = {
				{
					.sampler = frame_data.linear_sampler,
					.imageView = lighting_pass.get_color_output(0).view,
					.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				},
				{
					.sampler = frame_data.linear_sampler,
					.imageView = radial_depth_pass.get_color_output(0).view,
					.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				},
			};
			VkWriteDescriptorSet writes[2] = {};
			for (u32 write_idx = 0; write_idx < 2; ++write_idx)
			{
				writes[write_idx] = (VkWriteDescriptorSet) {
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = cube_to_oct_set,
					.dstBinding = 1 + write_idx,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.pImageInfo = &image_infos[write_idx],
				};
			}
			vulkan_update_descriptor_sets(ctx, 2, writes);
		}

		// Zero the octahedral atlas once so we never read garbage data
		vulkan_context_immediate_submit(ctx, [&](VkCommandBuffer in_command_buffer)
		{
			const VkClearColorValue clear_value = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
			const VkImageSubresourceRange clear_range = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = VK_REMAINING_ARRAY_LAYERS,
			};
			for (i32 output_idx = 0; output_idx < 2; ++output_idx)
			{
				GpuImage& atlas_image = cube_to_oct_pass.get_color_output(output_idx);
				gpu_image_transition(in_command_buffer, atlas_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				vkCmdClearColorImage(in_command_buffer, atlas_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &clear_range);
				gpu_image_transition(in_command_buffer, atlas_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			}
		});

		is_initialized = true;
	}

	// Resets the per-frame slot cursors (call once per frame after the fence)
	void begin_frame(VulkanContext* ctx)
	{
		(void) ctx;
		lighting_slot_cursor = 0;
		radial_slot_cursor = 0;
		projection_slot_cursor = 0;
	}

	void render(
		VulkanContext* ctx,
		State& in_state,
		HMM_Vec3 in_location,
		i32 in_atlas_idx,
		bool in_should_render_geometry,
		i32 in_probe_idx,
		f32 in_max_radial_depth,
		VkBuffer in_sh9_buffer,
		VkBuffer in_sg9_buffer
	)
	{
		assert(is_initialized);
		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];
		const u32 frame_index = ctx->frame_index;

		// Face cameras. game's uniform negative viewport provides the single
		// Y flip (game/ flips the projection instead — same net orientation).
		const f32 fov = HMM_AngleDeg(90.0f);
		HMM_Mat4 projection_matrix = mat4_perspective(fov, 1.0f);

		HMM_Mat4 view_projection_matrices[NUM_CUBE_FACES];
		for (i32 face_idx = 0; face_idx < NUM_CUBE_FACES; ++face_idx)
		{
			const HMM_Vec3 forward = CUBE_FORWARD_AND_UP[face_idx][0];
			const HMM_Vec3 up = CUBE_FORWARD_AND_UP[face_idx][1];
			const HMM_Mat4 view = HMM_LookAt_RH(in_location, in_location + forward * 10.0f, up);
			view_projection_matrices[face_idx] = HMM_MulM4(projection_matrix, view);
		}

		// ---- Face G-buffers ----
		geometry_pass.execute(ctx, [&](i32 face_idx)
		{
			const HMM_Mat4& view_projection = view_projection_matrices[face_idx];

			if (in_should_render_geometry && in_state.render_objects.valid)
			{
				vkCmdBindDescriptorSets(
					command_buffer,
					VK_PIPELINE_BIND_POINT_GRAPHICS,
					capture_geometry_pipeline_layout,
					0, 1, &frame_data.per_frame_sets[frame_index],
					0, nullptr
				);

				CullResult cull_result = cull_objects(in_state, view_projection,
					in_state.tessellation.enabled ? in_state.tessellation.bounds_padding : 0.0f);
				VkPipeline bound_pipeline = VK_NULL_HANDLE;
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
					const bool skinned = mesh.has_skinned_vertices && !render_view.is_tessellated;
					if (skinned && mesh.skin_matrix_arena_offset < 0)
					{
						continue;
					}

					VkPipeline wanted_pipeline = skinned ? capture_geometry_skinned_pipeline : capture_geometry_pipeline;
					if (bound_pipeline != wanted_pipeline)
					{
						vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted_pipeline);
						bound_pipeline = wanted_pipeline;
					}

					CaptureGeometryPushConstants push_constants = {
						.view_projection = view_projection,
						.object_index = object.render_object_index,
						.skin_matrix_offset = skinned ? mesh.skin_matrix_arena_offset : -1,
					};
					vkCmdPushConstants(command_buffer, capture_geometry_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push_constants), &push_constants);

					VkBuffer vertex_buffer = render_view.vertex_buffer;
					VkDeviceSize vertex_offset = 0;
					vkCmdBindVertexBuffers(command_buffer, 0, 1, &vertex_buffer, &vertex_offset);
					if (skinned)
					{
						VkBuffer skinned_vertex_buffer = mesh.skinned_vertex_buffer.get_gpu_buffer();
						VkDeviceSize skinned_offset = 0;
						vkCmdBindVertexBuffers(command_buffer, 1, 1, &skinned_vertex_buffer, &skinned_offset);
					}
					vkCmdBindIndexBuffer(command_buffer, render_view.index_buffer, 0, VK_INDEX_TYPE_UINT32);
					vulkan_cmd_draw_indexed(ctx, render_view.index_count, 1, 0, 0, 0);
				}
			}

			if (in_state.gi.render_sky_to_probes)
			{
				vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, capture_sky_pipeline);
				vkCmdBindDescriptorSets(
					command_buffer,
					VK_PIPELINE_BIND_POINT_GRAPHICS,
					capture_sky_pipeline_layout,
					0, 1, &sky_input_sets[frame_index],
					0, nullptr
				);
				CaptureSkyPushConstants sky_push = {
					.inv_view_projection = HMM_InvGeneralM4(view_projection),
					.capture_position = HMM_V4V(in_location, 1.0f),
				};
				vkCmdPushConstants(command_buffer, capture_sky_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(sky_push), &sky_push);
				vulkan_cmd_draw(ctx, 3, 1, 0, 0);
			}
		});

		// Lighting + radial depth sample the face G-buffers
		for (i32 face_idx = 0; face_idx < NUM_CUBE_FACES; ++face_idx)
		{
			for (i32 output_idx = 0; output_idx < Render::GBUFFER_OUTPUT_COUNT; ++output_idx)
			{
				gpu_image_transition(command_buffer, geometry_pass.get_color_output(output_idx, face_idx), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			}
		}

		// ---- Lit cubemap ----
		lighting_pass.execute(ctx, [&](i32 face_idx)
		{
			assert(lighting_slot_cursor < LIGHTING_SLOTS_PER_FRAME);
			const i32 slot = lighting_slot_cursor++;

			LightingFsParams fs_params = {};
			fs_params.view_position = in_location;
			fs_params.view_forward = CUBE_FORWARD_AND_UP[face_idx][0];
			fs_params.num_point_lights = (i32) in_state.lighting.point_lights.length();
			fs_params.num_spot_lights = (i32) in_state.lighting.spot_lights.length();
			fs_params.num_sun_lights = (i32) in_state.lighting.sun_lights.length();
			fs_params.direct_lighting_enable = 1;
			fs_params.shadow_bias = 0.001f;
			fs_params.shadow_map_texel_size = HMM_V2(1.0f, 1.0f);
			lighting_slot_ubos[frame_index][slot].update_gpu_buffer(&fs_params, sizeof(fs_params));

			VkDescriptorSet slot_set = lighting_slot_sets[frame_index][slot];
			VkDescriptorBufferInfo ubo_info = {
				.buffer = lighting_slot_ubos[frame_index][slot].get_gpu_buffer(),
				.offset = 0,
				.range = sizeof(LightingFsParams),
			};
			VkDescriptorImageInfo gbuffer_infos[4];
			for (i32 output_idx = 0; output_idx < 4; ++output_idx)
			{
				gbuffer_infos[output_idx] = (VkDescriptorImageInfo) {
					.sampler = ::lighting_pass.linear_sampler,
					.imageView = geometry_pass.get_color_output(output_idx, face_idx).view,
					.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				};
			}
			VkDescriptorImageInfo default_info = {
				.sampler = ::lighting_pass.linear_sampler,
				.imageView = default_image.view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
			VkDescriptorImageInfo default_array_info = {
				.sampler = ::lighting_pass.linear_sampler,
				.imageView = default_array_image.view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
			VkDescriptorBufferInfo light_infos[3] = {
				{ .buffer = in_state.lighting.point_buffers[in_state.lighting.buffer_index].get_gpu_buffer(), .offset = 0, .range = VK_WHOLE_SIZE },
				{ .buffer = in_state.lighting.spot_buffers[in_state.lighting.buffer_index].get_gpu_buffer(), .offset = 0, .range = VK_WHOLE_SIZE },
				{ .buffer = in_state.lighting.sun_buffers[in_state.lighting.buffer_index].get_gpu_buffer(), .offset = 0, .range = VK_WHOLE_SIZE },
			};

			VkWriteDescriptorSet writes[LIGHTING_DESCRIPTOR_BINDING_COUNT] = {};
			u32 write_count = 0;
			writes[write_count++] = (VkWriteDescriptorSet) {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = slot_set,
				.dstBinding = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo = &ubo_info,
			};
			for (u32 binding_idx = 1; binding_idx <= 4; ++binding_idx)
			{
				writes[write_count++] = (VkWriteDescriptorSet) {
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = slot_set,
					.dstBinding = binding_idx,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.pImageInfo = &gbuffer_infos[binding_idx - 1],
				};
			}
			for (u32 binding_idx = 6; binding_idx <= 8; ++binding_idx)
			{
				writes[write_count++] = (VkWriteDescriptorSet) {
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = slot_set,
					.dstBinding = binding_idx,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.pBufferInfo = &light_infos[binding_idx - 6],
				};
			}
			const u32 fallback_image_bindings[] = { 5, 9, 10, 13, 14 };
			for (u32 binding_idx : fallback_image_bindings)
			{
				writes[write_count++] = (VkWriteDescriptorSet) {
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = slot_set,
					.dstBinding = binding_idx,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.pImageInfo = binding_idx == 5 ? &default_array_info : &default_info,
				};
			}
			// GI is disabled for probe-capture lighting, but layout C still
			// requires valid descriptors for its statically declared SSBOs.
			// Any light buffer is a safe dummy because those bindings are never
			// read while fs_params.gi_enable is zero.
			const u32 fallback_buffer_bindings[] = { 11, 12, 15, 16, 17 };
			for (u32 binding_idx : fallback_buffer_bindings)
			{
				writes[write_count++] = (VkWriteDescriptorSet) {
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = slot_set,
					.dstBinding = binding_idx,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.pBufferInfo = &light_infos[0],
				};
			}
			vulkan_update_descriptor_sets(ctx, write_count, writes);

			vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, capture_lighting_pipeline);
			vkCmdBindDescriptorSets(
				command_buffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				::lighting_pass.pipeline_layout,
				0, 1, &slot_set,
				0, nullptr
			);
			vulkan_cmd_draw(ctx, 3, 1, 0, 0);
		});
		gpu_image_transition(command_buffer, lighting_pass.get_color_output(0), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		// ---- Radial depth cubemap ----
		radial_depth_pass.execute(ctx, [&](i32 face_idx)
		{
			assert(radial_slot_cursor < RADIAL_SLOTS_PER_FRAME);
			const i32 slot = radial_slot_cursor++;
			VkDescriptorSet slot_set = radial_slot_sets[frame_index][slot];

			VkDescriptorImageInfo position_info = {
				.sampler = ::lighting_pass.linear_sampler,
				.imageView = geometry_pass.get_color_output(1, face_idx).view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
			VkWriteDescriptorSet write = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = slot_set,
				.dstBinding = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &position_info,
			};
			vulkan_update_descriptor_sets(ctx, 1, &write);

			RadialDepthPushConstants push_constants = {
				.inverse_view_projection = HMM_InvGeneralM4(view_projection_matrices[face_idx]),
				.capture_location = in_location,
				.probe_occlusion_mode = (i32) in_state.gi.probe_occlusion_mode,
				.force_fully_visible = in_state.gi.debug_constant_white_probes ? 1 : 0,
				.max_radial_depth = in_max_radial_depth,
			};

			vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, radial_depth_pipeline);
			vkCmdBindDescriptorSets(
				command_buffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				radial_depth_pipeline_layout,
				0, 1, &slot_set,
				0, nullptr
			);
			vkCmdPushConstants(command_buffer, radial_depth_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants), &push_constants);
			vulkan_cmd_draw(ctx, 3, 1, 0, 0);
		});
		gpu_image_transition(command_buffer, radial_depth_pass.get_color_output(0), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		// ---- Octahedral atlas entry ----
		cube_to_oct_pass.execute(ctx, [&](i32)
		{
			const AtlasViewport atlas_viewport = get_atlas_viewport(
				desc.octahedral_total_size,
				desc.octahedral_entry_size,
				in_atlas_idx
			);

			// Slot viewport, keeping the framework's negative-height Y flip
			VkViewport slot_viewport = {
				.x = (f32) atlas_viewport.x,
				.y = (f32) (atlas_viewport.y + atlas_viewport.h),
				.width = (f32) atlas_viewport.w,
				.height = -(f32) atlas_viewport.h,
				.minDepth = 0.0f,
				.maxDepth = 1.0f,
			};
			vkCmdSetViewport(command_buffer, 0, 1, &slot_viewport);
			VkRect2D slot_scissor = {
				.offset = { atlas_viewport.x, atlas_viewport.y },
				.extent = { (u32) atlas_viewport.w, (u32) atlas_viewport.h },
			};
			vkCmdSetScissor(command_buffer, 0, 1, &slot_scissor);

			CubeToOctPushConstants push_constants = {
				.cubemap_render_size = desc.cubemap_render_size,
				.atlas_entry_size = desc.octahedral_entry_size,
				.compute_irradiance = in_state.gi.compute_irradiance ? 1 : 0,
				.use_importance_sampling = 1,
			};

			vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, cube_to_oct_pipeline);
			vkCmdBindDescriptorSets(
				command_buffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				cube_to_oct_pipeline_layout,
				0, 1, &cube_to_oct_set,
				0, nullptr
			);
			vkCmdPushConstants(command_buffer, cube_to_oct_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants), &push_constants);
			vulkan_cmd_draw(ctx, 3, 1, 0, 0);
		});
		gpu_image_transition(command_buffer, cube_to_oct_pass.get_color_output(0), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		gpu_image_transition(command_buffer, cube_to_oct_pass.get_color_output(1), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		// ---- SH9 / SG9 projection ----
		const bool should_project_sh9 =
			in_state.gi.probe_radiance_mode == EProbeRadianceMode::SH9 ||
			in_state.gi.probe_vis_mode == EProbeVisMode::SH9Irradiance;
		const bool should_project_sg9 =
			in_state.gi.probe_radiance_mode == EProbeRadianceMode::SG9 ||
			in_state.gi.probe_vis_mode == EProbeVisMode::SG9Irradiance;

		const auto project_probe_radiance = [&](EProbeRadianceMode in_radiance_mode)
		{
			assert(projection_slot_cursor < PROJECTION_SLOTS_PER_FRAME);
			const i32 slot = projection_slot_cursor++;
			VkDescriptorSet slot_set = projection_slot_sets[frame_index][slot];

			VkDescriptorBufferInfo buffer_infos[] = {
				{ .buffer = in_sh9_buffer, .offset = 0, .range = VK_WHOLE_SIZE },
				{ .buffer = in_sg9_buffer, .offset = 0, .range = VK_WHOLE_SIZE },
			};
			VkDescriptorImageInfo cube_info = {
				.sampler = ::lighting_pass.linear_sampler,
				.imageView = lighting_pass.get_color_output(0).view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
			VkWriteDescriptorSet writes[3] = {};
			for (u32 buffer_idx = 0; buffer_idx < 2; ++buffer_idx)
			{
				writes[buffer_idx] = (VkWriteDescriptorSet) {
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = slot_set,
					.dstBinding = buffer_idx,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.pBufferInfo = &buffer_infos[buffer_idx],
				};
			}
			writes[2] = (VkWriteDescriptorSet) {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = slot_set,
				.dstBinding = 2,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &cube_info,
			};
			vulkan_update_descriptor_sets(ctx, 3, writes);

			ProbeProjectionPushConstants push_constants = {
				.probe_index = in_probe_idx,
				.radiance_mode = (i32) in_radiance_mode,
				.sample_count = 1024,
			};
			vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, projection_pipeline);
			vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, projection_pipeline_layout, 0, 1, &slot_set, 0, nullptr);
			vkCmdPushConstants(command_buffer, projection_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);
			vulkan_cmd_dispatch(ctx, 1, 1, 1);
		};

		bool dispatched_projection = false;
		if (should_project_sh9)
		{
			project_probe_radiance(EProbeRadianceMode::SH9);
			dispatched_projection = true;
		}
		if (should_project_sg9)
		{
			project_probe_radiance(EProbeRadianceMode::SG9);
			dispatched_projection = true;
		}

		if (dispatched_projection)
		{
			VkMemoryBarrier2 memory_barrier = {
				.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
				.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
				.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
			};
			VkDependencyInfo dependency_info = {
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.memoryBarrierCount = 1,
				.pMemoryBarriers = &memory_barrier,
			};
			vkCmdPipelineBarrier2(command_buffer, &dependency_info);
		}
	}

	void cleanup(VulkanContext* ctx)
	{
		if (!is_initialized)
		{
			return;
		}

		vkDestroyPipeline(ctx->device, projection_pipeline, nullptr);
		vkDestroyPipeline(ctx->device, cube_to_oct_pipeline, nullptr);
		vkDestroyPipeline(ctx->device, radial_depth_pipeline, nullptr);
		vkDestroyPipeline(ctx->device, capture_lighting_pipeline, nullptr);
		vkDestroyPipeline(ctx->device, capture_sky_pipeline, nullptr);
		vkDestroyPipeline(ctx->device, capture_geometry_skinned_pipeline, nullptr);
		vkDestroyPipeline(ctx->device, capture_geometry_pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, projection_pipeline_layout, nullptr);
		vkDestroyPipelineLayout(ctx->device, cube_to_oct_pipeline_layout, nullptr);
		vkDestroyPipelineLayout(ctx->device, radial_depth_pipeline_layout, nullptr);
		vkDestroyPipelineLayout(ctx->device, capture_sky_pipeline_layout, nullptr);
		vkDestroyPipelineLayout(ctx->device, capture_geometry_pipeline_layout, nullptr);
		vkDestroyDescriptorSetLayout(ctx->device, projection_set_layout, nullptr);
		vkDestroyDescriptorSetLayout(ctx->device, cube_to_oct_set_layout, nullptr);
		vkDestroyDescriptorPool(ctx->device, pool, nullptr);

		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			for (i32 slot_idx = 0; slot_idx < LIGHTING_SLOTS_PER_FRAME; ++slot_idx)
			{
				lighting_slot_ubos[frame_idx][slot_idx].destroy_gpu_buffer();
			}
		}

		gpu_image_destroy(ctx->allocator, ctx->device, default_image);
		gpu_image_destroy(ctx->allocator, ctx->device, default_array_image);
		geometry_pass.cleanup();
		lighting_pass.cleanup();
		radial_depth_pass.cleanup();
		cube_to_oct_pass.cleanup();
		is_initialized = false;
	}
};
