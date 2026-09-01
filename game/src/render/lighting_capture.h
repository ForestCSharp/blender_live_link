#pragma once

#include <cstddef>
#include <cmath>

#include "core/types.h"
#include "render/fullscreen_pipeline.h"
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

// Probe lighting capture:
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

// Metal/D3D-convention cube face bases.
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
	i32 specular_entry_size = 48;
	i32 specular_mip_count = 4;
};

// Mirrors the geometry capture vertex/fragment push constants.
struct CaptureGeometryPushConstants
{
	HMM_Mat4 view_projection;
	i32 object_index;
	i32 skin_matrix_offset;
	i32 _padding0[2];
	HMM_Vec4 capture_position_and_radius;
};
static_assert(sizeof(CaptureGeometryPushConstants) == 96,
	"Geometry capture push constants must fit Vulkan's guaranteed minimum");
static_assert(offsetof(CaptureGeometryPushConstants, capture_position_and_radius) == 80,
	"Geometry capture radius must match the shader push-constant offset");

// Mirrors sky_capture.frag's push constants
struct CaptureSkyPushConstants
{
	HMM_Mat4 inv_view_projection;
	HMM_Vec4 capture_position;
	HMM_Vec4 sun_direction;
	HMM_Vec4 light_color_and_sky_intensity;
	HMM_Vec4 planet_center_z;
};
static_assert(sizeof(CaptureSkyPushConstants) == 128,
	"Sky capture push constants must fit Vulkan's guaranteed minimum");

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

struct SpecularPrefilterPushConstants
{
	i32 atlas_entry_size;
	f32 roughness;
	i32 sample_count;
	i32 padding0;
};
static_assert(sizeof(SpecularPrefilterPushConstants) == 16, "Specular prefilter push constants mismatch");

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

	// Capture sky (Bruneton LUT set + per-probe position push constants)
	EffectPipelineLayout capture_sky_pipeline_layout;
	FullscreenPipeline capture_sky_pipeline;

	// Capture lighting: layout C reused; RGBA32F pipeline variant + slot ring
	FullscreenPipeline capture_lighting_pipeline;
	static constexpr i32 LIGHTING_SLOTS_PER_FRAME = NUM_CUBE_FACES * 4;	// probes_to_update_per_frame
	GpuBuffer<LightingFsParams> lighting_slot_ubos[MAX_FRAMES_IN_FLIGHT][LIGHTING_SLOTS_PER_FRAME];
	VkDescriptorSet lighting_slot_sets[MAX_FRAMES_IN_FLIGHT][LIGHTING_SLOTS_PER_FRAME] = {};
	i32 lighting_slot_cursor = 0;

	// Radial depth (push constants + per-frame input set)
	EffectPipelineLayout radial_depth_pipeline_layout;
	FullscreenPipeline radial_depth_pipeline;
	static constexpr i32 RADIAL_SLOTS_PER_FRAME = NUM_CUBE_FACES * 4;
	VkDescriptorSet radial_slot_sets[MAX_FRAMES_IN_FLIGHT][RADIAL_SLOTS_PER_FRAME] = {};
	i32 radial_slot_cursor = 0;

	// Cube -> octahedral (static inputs: the capture cubemaps never resize)
	DescriptorSetSchema cube_to_oct_descriptors;
	EffectPipelineLayout cube_to_oct_pipeline_layout;
	FullscreenPipeline cube_to_oct_pipeline;
	VkDescriptorSet cube_to_oct_set = VK_NULL_HANDLE;

	// Probe radiance projection compute (per-frame sets: sh9/sg9 buffers
	// change on layout rebuilds)
	TypedComputeEffect<ProbeProjectionPushConstants> projection_effect;
	static constexpr i32 PROJECTION_SLOTS_PER_FRAME = 8;	// <=2 modes * 4 probes
	VkDescriptorSet projection_slot_sets[MAX_FRAMES_IN_FLIGHT][PROJECTION_SLOTS_PER_FRAME] = {};
	i32 projection_slot_cursor = 0;

	// GGX specular atlas + split-sum BRDF LUT.
	GpuImage specular_atlas = {};
	GpuImage brdf_lut = {};
	i32 specular_atlas_total_size = 0;
	i32 specular_atlas_capacity = 0;
	VkSampler specular_sampler = VK_NULL_HANDLE;
	DescriptorSetSchema specular_prefilter_descriptors;
	EffectPipelineLayout specular_prefilter_pipeline_layout;
	FullscreenPipeline specular_prefilter_pipeline;
	VkDescriptorSet specular_prefilter_set = VK_NULL_HANDLE;
	EffectPipelineLayout brdf_lut_pipeline_layout;
	FullscreenPipeline brdf_lut_pipeline;

	VkDescriptorPool pool = VK_NULL_HANDLE;

	// 1x1 white fallback for unused lighting bindings (ssao/contact shadows)
	GpuImage default_image = {};
	GpuImage default_array_image = {};

	bool is_initialized = false;

	VkFormat color_format = VK_FORMAT_R32G32B32A32_SFLOAT;

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
		color_format = ctx->capabilities.gbuffer_format;
		assert(!is_initialized);
		desc = in_desc;

		// ---- Passes ----
		geometry_pass.init((RenderPassDesc) {
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
			.extent = render_target_extent_fixed(desc.cubemap_render_size, desc.cubemap_render_size),
			.type = ERenderPassType::Multi,
			.debug_label = "GI Capture Geometry",
		});

		lighting_pass.init((RenderPassDesc) {
			.num_outputs = 1,
			.outputs = {
				{ .format = color_format, .load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
			},
			.extent = render_target_extent_fixed(desc.cubemap_render_size, desc.cubemap_render_size),
			.type = ERenderPassType::Cubemap,
			.debug_label = "GI Capture Lighting",
		});

		radial_depth_pass.init((RenderPassDesc) {
			.num_outputs = 1,
			.outputs = {
				{ .format = color_format, .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR, .store_op = VK_ATTACHMENT_STORE_OP_STORE, .clear_value = {{{ 1.0f, 1.0f, 1.0f, 1.0f }}} },
			},
			.extent = render_target_extent_fixed(desc.cubemap_render_size, desc.cubemap_render_size),
			.type = ERenderPassType::Cubemap,
			.debug_label = "GI Radial Depth",
		});

		cube_to_oct_pass.init((RenderPassDesc) {
			.num_outputs = 2,
			.outputs = {
				{ .format = color_format, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
				{ .format = color_format, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
			},
			.extent = render_target_extent_fixed(desc.octahedral_total_size, desc.octahedral_total_size),
			.type = ERenderPassType::Single,
			.debug_label = "GI Cube To Octahedral",
		});

		// ---- Set layouts ----
		{
			const DescriptorBindingSpec bindings[] = {
				{ .binding = 1, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
				{ .binding = 2, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
			};
			cube_to_oct_descriptors.init(ctx, bindings, 2,
				EDescriptorSetAllocation::Transient);
		}
		{
			const DescriptorBindingSpec bindings[] = {
				{ .binding = 0, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.stages = VK_SHADER_STAGE_COMPUTE_BIT },
				{ .binding = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.stages = VK_SHADER_STAGE_COMPUTE_BIT },
				{ .binding = 2, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.stages = VK_SHADER_STAGE_COMPUTE_BIT },
			};
			projection_effect.init(ctx, {
				.shader_path = "bin/shaders/probe_radiance_projection.comp.spv",
				.bindings = bindings,
				.binding_count = 3,
			});
		}
		{
			const DescriptorBindingSpec binding = {
				.binding = 0, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			};
			specular_prefilter_descriptors.init(ctx, &binding, 1,
				EDescriptorSetAllocation::Transient);
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
				allocate_info.pSetLayouts =
					&projection_effect.effect.descriptors.layout;
				VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &projection_slot_sets[frame_idx][slot_idx]));
			}
		}

		{
			VkDescriptorSetAllocateInfo allocate_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &cube_to_oct_descriptors.layout,
			};
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &cube_to_oct_set));
			allocate_info.pSetLayouts = &specular_prefilter_descriptors.layout;
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &specular_prefilter_set));
		}

		// ---- Pipeline layouts ----
		{
			VkPushConstantRange push_range = {
				.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
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
		capture_sky_pipeline_layout.init(ctx,
			&bruneton_atmosphere_pass.descriptors.layout, 1,
			sizeof(CaptureSkyPushConstants), VK_SHADER_STAGE_FRAGMENT_BIT);
		radial_depth_pipeline_layout.init(ctx, &frame_data.sampled_input_layout, 1,
			sizeof(RadialDepthPushConstants), VK_SHADER_STAGE_FRAGMENT_BIT);
		cube_to_oct_pipeline_layout.init(ctx, &cube_to_oct_descriptors.layout, 1,
			sizeof(CubeToOctPushConstants), VK_SHADER_STAGE_FRAGMENT_BIT);
		specular_prefilter_pipeline_layout.init(ctx,
			&specular_prefilter_descriptors.layout, 1,
			sizeof(SpecularPrefilterPushConstants), VK_SHADER_STAGE_FRAGMENT_BIT);
		brdf_lut_pipeline_layout.init(ctx, nullptr, 0, 0, 0);

		// ---- Pipelines ----
		capture_geometry_pipeline = create_capture_geometry_pipeline(ctx, "bin/shaders/geometry_capture.vert.spv", false);
		capture_geometry_skinned_pipeline = create_capture_geometry_pipeline(ctx, "bin/shaders/geometry_capture_skinned.vert.spv", true);

		{
			VkFormat sky_formats[4] = {
				Render::GBUFFER_FORMAT, Render::GBUFFER_FORMAT,
				Render::GBUFFER_FORMAT, Render::GBUFFER_FORMAT,
			};
			capture_sky_pipeline.init(ctx, {
				.vertex_shader_path = "bin/shaders/sky_capture.vert.spv",
				.fragment_shader_path = "bin/shaders/sky_capture.frag.spv",
				.pipeline_layout = capture_sky_pipeline_layout.layout,
				.color_formats = sky_formats,
				.color_format_count = 4,
				.depth_format = Render::SCENE_DEPTH_FORMAT,
				.depth_test_enable = true,
				.depth_write_enable = true,
				.depth_compare_op = Render::DEPTH_COMPARE_OP,
			});
		}
		{
			VkFormat lighting_formats[1] = { color_format };
			capture_lighting_pipeline.init(ctx, {
				.vertex_shader_path = "bin/shaders/lighting.vert.spv",
				.fragment_shader_path = "bin/shaders/lighting.frag.spv",
				.pipeline_layout = ::lighting_pass.pipeline_layout,
				.color_formats = lighting_formats,
				.color_format_count = 1,
			});
		}
		{
			VkFormat radial_formats[1] = { color_format };
			radial_depth_pipeline.init(ctx, {
				.vertex_shader_path = "bin/shaders/radial_depth.vert.spv",
				.fragment_shader_path = "bin/shaders/radial_depth.frag.spv",
				.pipeline_layout = radial_depth_pipeline_layout.layout,
				.color_formats = radial_formats,
				.color_format_count = 1,
			});
		}
		{
			VkFormat oct_formats[2] = { color_format, color_format };
			cube_to_oct_pipeline.init(ctx, {
				.vertex_shader_path = "bin/shaders/cubemap_to_octahedral.vert.spv",
				.fragment_shader_path = "bin/shaders/cubemap_to_octahedral.frag.spv",
				.pipeline_layout = cube_to_oct_pipeline_layout.layout,
				.color_formats = oct_formats,
				.color_format_count = 2,
			});
		}
		{
			VkFormat prefilter_formats[1] = { ctx->capabilities.scene_color_format };
			specular_prefilter_pipeline.init(ctx, {
				.vertex_shader_path = "bin/shaders/cubemap_to_octahedral.vert.spv",
				.fragment_shader_path = "bin/shaders/probe_specular_prefilter.frag.spv",
				.pipeline_layout = specular_prefilter_pipeline_layout.layout,
				.color_formats = prefilter_formats,
				.color_format_count = 1,
			});
			brdf_lut_pipeline.init(ctx, {
				.vertex_shader_path = "bin/shaders/cubemap_to_octahedral.vert.spv",
				.fragment_shader_path = "bin/shaders/brdf_integration.frag.spv",
				.pipeline_layout = brdf_lut_pipeline_layout.layout,
				.color_formats = prefilter_formats,
				.color_format_count = 1,
			});
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

		{
			VkSamplerCreateInfo sampler_create_info = {
				.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
				.magFilter = VK_FILTER_LINEAR,
				.minFilter = VK_FILTER_LINEAR,
				.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
				.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				.minLod = 0.0f,
				.maxLod = (f32)(desc.specular_mip_count - 1),
			};
			VK_CHECK(vkCreateSampler(ctx->device, &sampler_create_info, nullptr, &specular_sampler));
			vulkan_set_object_name(ctx, VK_OBJECT_TYPE_SAMPLER, (u64)specular_sampler, "GI Specular Atlas Sampler");

			DescriptorWriter writer = specular_prefilter_descriptors.writer(
				ctx, specular_prefilter_set, true);
			writer.sampled(0, frame_data.linear_sampler,
				lighting_pass.get_color_output(0).view);
			writer.commit();
		}

		brdf_lut = gpu_image_create(ctx->allocator, ctx->device, (GpuImageDesc) {
			.width = 256,
			.height = 256,
			.format = ctx->capabilities.scene_color_format,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
			.label = "GI Split-Sum BRDF LUT",
		});
		vulkan_context_immediate_submit(ctx, [&](VkCommandBuffer command_buffer)
		{
			gpu_image_transition(command_buffer, brdf_lut, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
			VkRenderingAttachmentInfo attachment = {
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = brdf_lut.view,
				.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			};
			VkRenderingInfo rendering_info = {
				.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
				.renderArea = { .offset = {0, 0}, .extent = brdf_lut.extent },
				.layerCount = 1,
				.colorAttachmentCount = 1,
				.pColorAttachments = &attachment,
			};
			vkCmdBeginRendering(command_buffer, &rendering_info);
			VkViewport viewport = {
				.x = 0.0f, .y = (f32)brdf_lut.extent.height,
				.width = (f32)brdf_lut.extent.width, .height = -(f32)brdf_lut.extent.height,
				.minDepth = 0.0f, .maxDepth = 1.0f,
			};
			VkRect2D scissor = { .offset = {0, 0}, .extent = brdf_lut.extent };
			vkCmdSetViewport(command_buffer, 0, 1, &viewport);
			vkCmdSetScissor(command_buffer, 0, 1, &scissor);
			brdf_lut_pipeline.bind(command_buffer);
			vkCmdDraw(command_buffer, 3, 1, 0, 0);
			vkCmdEndRendering(command_buffer);
			gpu_image_transition(command_buffer, brdf_lut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		});

		// cube_to_oct inputs: the capture cubemaps never resize
		{
			DescriptorWriter writer = cube_to_oct_descriptors.writer(
				ctx, cube_to_oct_set, true);
			writer.sampled(1, frame_data.linear_sampler,
				lighting_pass.get_color_output(0).view)
				.sampled(2, frame_data.linear_sampler,
					radial_depth_pass.get_color_output(0).view);
			writer.commit();
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

	void resize_specular_atlas(VulkanContext* ctx, i32 in_probe_count)
	{
		assert(is_initialized);
		const i32 slots_per_dimension = (i32)std::ceil(std::sqrt((f64)std::max(in_probe_count, 1)));
		const i32 required_size = slots_per_dimension * desc.specular_entry_size;
		assert(required_size <= (i32)ctx->capabilities.properties.limits.maxImageDimension2D);

		if (specular_atlas.image == VK_NULL_HANDLE || specular_atlas_total_size != required_size)
		{
			vulkan_context_retire_image(ctx, specular_atlas);
			specular_atlas = gpu_image_create(ctx->allocator, ctx->device, (GpuImageDesc) {
				.width = (u32)required_size,
				.height = (u32)required_size,
				.format = ctx->capabilities.scene_color_format,
				.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
					| VK_IMAGE_USAGE_SAMPLED_BIT
					| VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
				.mip_levels = (u32)desc.specular_mip_count,
				.label = "GI Specular Prefilter Atlas",
			});
			specular_atlas_total_size = required_size;
			specular_atlas_capacity = slots_per_dimension * slots_per_dimension;
		}

		auto clear_atlas = [&](VkCommandBuffer command_buffer)
		{
			const VkClearColorValue clear_value = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
			const VkImageSubresourceRange clear_range = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = (u32)desc.specular_mip_count,
				.baseArrayLayer = 0,
				.layerCount = 1,
			};
			gpu_image_transition(command_buffer, specular_atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true);
			vkCmdClearColorImage(
				command_buffer, specular_atlas.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				&clear_value, 1, &clear_range
			);
			gpu_image_transition(command_buffer, specular_atlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		};

		if (vulkan_current_frame(ctx).recording)
		{
			clear_atlas(vulkan_current_command_buffer(ctx));
		}
		else
		{
			vulkan_context_immediate_submit(ctx, clear_atlas);
		}
	}

	void render_specular_prefilter(
		FrameRenderGraph& graph, VulkanContext* ctx, i32 in_atlas_idx)
	{
		assert(specular_atlas.image != VK_NULL_HANDLE);
		assert(in_atlas_idx >= 0 && in_atlas_idx < specular_atlas_capacity);
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);

		for (i32 mip = 0; mip < desc.specular_mip_count; ++mip)
		{
			const i32 mip_total_size = specular_atlas_total_size >> mip;
			const i32 mip_entry_size = desc.specular_entry_size >> mip;
			const AtlasViewport atlas_viewport = get_atlas_viewport(
				mip_total_size, mip_entry_size, in_atlas_idx
			);
			SpecularPrefilterPushConstants push_constants = {
				.atlas_entry_size = mip_entry_size,
				.roughness = (f32)mip / (f32)(desc.specular_mip_count - 1),
				.sample_count = mip == 0 ? 1 : 1024,
			};
			graph.sampled(frame_graph_color(lighting_pass));
			FrameGraphColorAttachment attachment = {
				.image = frame_graph_mip(specular_atlas, (u32)mip),
				.load_op = VK_ATTACHMENT_LOAD_OP_LOAD,
			};
			graph.render(&attachment, 1, [&]() {
				VkViewport viewport = {
					.x = (f32)atlas_viewport.x,
					.y = (f32)(atlas_viewport.y + atlas_viewport.h),
					.width = (f32)atlas_viewport.w,
					.height = -(f32)atlas_viewport.h,
					.minDepth = 0.0f,
					.maxDepth = 1.0f,
				};
				VkRect2D scissor = {
					.offset = {atlas_viewport.x, atlas_viewport.y},
					.extent = {(u32)atlas_viewport.w, (u32)atlas_viewport.h},
				};
				vkCmdSetViewport(command_buffer, 0, 1, &viewport);
				vkCmdSetScissor(command_buffer, 0, 1, &scissor);
				specular_prefilter_pipeline.bind(ctx);
				vkCmdBindDescriptorSets(
					command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
					specular_prefilter_pipeline_layout.layout,
					0, 1, &specular_prefilter_set, 0, nullptr);
				specular_prefilter_pipeline_layout.push(ctx, push_constants);
				vulkan_cmd_draw(ctx, 3, 1, 0, 0);
			});
			graph.sampled(frame_graph_mip(specular_atlas, (u32)mip));
		}
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
		FrameRenderGraph& graph,
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
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		const u32 frame_index = ctx->frame_index;

		// Face cameras use a negative viewport height for the required Y flip,
		// keeping all six faces consistently oriented.
		const bool cull_to_probe_influence = in_should_render_geometry
			&& in_state.gi.probe_influence_culling;
		const BoundingSphere influence_sphere = {
			.center = in_location,
			.radius = in_max_radial_depth,
		};
		const f32 fov = HMM_AngleDeg(90.0f);
		HMM_Mat4 projection_matrix = cull_to_probe_influence
			? mat4_perspective(fov, 1.0f, in_max_radial_depth)
			: mat4_perspective(fov, 1.0f);

		HMM_Mat4 view_projection_matrices[NUM_CUBE_FACES];
		for (i32 face_idx = 0; face_idx < NUM_CUBE_FACES; ++face_idx)
		{
			const HMM_Vec3 forward = CUBE_FORWARD_AND_UP[face_idx][0];
			const HMM_Vec3 up = CUBE_FORWARD_AND_UP[face_idx][1];
			const HMM_Mat4 view = HMM_LookAt_RH(in_location, in_location + forward * 10.0f, up);
			view_projection_matrices[face_idx] = HMM_MulM4(projection_matrix, view);
		}

		// ---- Face G-buffers ----
		graph.execute(geometry_pass, [&](i32 face_idx)
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

				DynamicArray<i32>& visible = in_state.cull_scratch.capture;
				cull_objects(
					in_state,
					view_projection,
					in_state.tessellation.enabled
						? in_state.tessellation.bounds_padding : 0.0f,
					visible,
					cull_to_probe_influence ? &influence_sphere : nullptr);
				VkPipeline bound_pipeline = VK_NULL_HANDLE;
				for (i32 render_object_index : visible)
				{
					auto found = in_state.scene.objects.find(in_state.cull_entries[render_object_index].object_id);
					if (found == in_state.scene.objects.end())
					{
						continue;
					}

					Object& object = found->second;
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
						.object_index = render_object_index,
						.skin_matrix_offset = skinned ? mesh.skin_matrix_arena_offset : -1,
						.capture_position_and_radius = HMM_V4V(
							in_location,
							cull_to_probe_influence ? in_max_radial_depth : 0.0f),
					};
					vulkan_cmd_push_constants(
						ctx,
						capture_geometry_pipeline_layout,
						VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
						0,
						sizeof(push_constants),
						&push_constants);

					VkBuffer vertex_buffer = render_view.vertex_buffer;
					VkDeviceSize vertex_offset = 0;
					vulkan_cmd_bind_vertex_buffers(ctx, 0, 1, &vertex_buffer, &vertex_offset);
					if (skinned)
					{
						VkBuffer skinned_vertex_buffer = mesh.skinned_vertex_buffer.get_gpu_buffer();
						VkDeviceSize skinned_offset = 0;
						vulkan_cmd_bind_vertex_buffers(ctx, 1, 1, &skinned_vertex_buffer, &skinned_offset);
					}
					vulkan_cmd_bind_index_buffer(ctx, render_view.index_buffer, 0, VK_INDEX_TYPE_UINT32);
					vulkan_cmd_draw_indexed(ctx, render_view.index_count, 1, 0, 0, 0);
				}
			}

			if (in_state.gi.render_sky_to_probes
				&& sky_pass.has_active_atmosphere
				&& bruneton_atmosphere_pass.has_precomputed)
			{
				capture_sky_pipeline.bind(ctx);
				vkCmdBindDescriptorSets(
					command_buffer,
					VK_PIPELINE_BIND_POINT_GRAPHICS,
					capture_sky_pipeline_layout.layout,
					0, 1, &bruneton_atmosphere_pass.descriptors.persistent_sets.sets[frame_index],
					0, nullptr
				);
				CaptureSkyPushConstants sky_push = {
					.inv_view_projection = HMM_InvGeneralM4(view_projection),
					.capture_position = HMM_V4V(in_location, 1.0f),
					.sun_direction = HMM_V4V(sky_pass.active_sun_direction, 0.0f),
					.light_color_and_sky_intensity = HMM_V4V(
						sky_pass.active_sun_color, sky_pass.active_parameters.sky_intensity),
					.planet_center_z = HMM_V4(
						sky_pass.active_parameters.planet_center_z_m, 0.0f, 0.0f, 0.0f),
				};
				capture_sky_pipeline_layout.push(ctx, sky_push);
				vulkan_cmd_draw(ctx, 3, 1, 0, 0);
			}
		});

		// Lighting + radial depth sample the face G-buffers
		graph.make_sampled(geometry_pass);

		// ---- Lit cubemap ----
		graph.execute(lighting_pass, [&](i32 face_idx)
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
			fs_params.atmosphere_sun_index =
				in_state.lighting.active_atmosphere_sun_index;
			fs_params.atmosphere_enabled = sky_pass.has_active_atmosphere
				&& bruneton_atmosphere_pass.has_precomputed
				&& in_state.lighting.active_atmosphere_sun_index >= 0 ? 1 : 0;
			fs_params.atmosphere_planet_center_z =
				sky_pass.active_parameters.planet_center_z_m;
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
			const u32 fallback_image_bindings[] = { 5, 9, 10, 13, 14, 18, 19, 22 };
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
			VkDescriptorBufferInfo atmosphere_buffer_info = descriptor_buffer(
				bruneton_atmosphere_pass.parameter_buffers[frame_index].get_gpu_buffer(),
				sizeof(BrunetonAtmosphereGpu));
			writes[write_count++] = descriptor_write_buffer(
				slot_set, 20, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				&atmosphere_buffer_info);
			VkDescriptorImageInfo atmosphere_image_info = descriptor_sampled(
				::lighting_pass.linear_sampler,
				bruneton_atmosphere_pass.has_precomputed
					? bruneton_atmosphere_pass.transmittance_pass.get_color_output(0).view
					: default_image.view);
			writes[write_count++] = descriptor_write_image(
				slot_set, 21, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				&atmosphere_image_info);
			vulkan_update_descriptor_sets(ctx, write_count, writes);

			capture_lighting_pipeline.bind(ctx);
			vkCmdBindDescriptorSets(
				command_buffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				::lighting_pass.pipeline_layout,
				0, 1, &slot_set,
				0, nullptr
			);
			vulkan_cmd_draw(ctx, 3, 1, 0, 0);
		});
		graph.make_sampled(lighting_pass);
		render_specular_prefilter(graph, ctx, in_atlas_idx);

		// ---- Radial depth cubemap ----
		graph.execute(radial_depth_pass, [&](i32 face_idx)
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

			radial_depth_pipeline.bind(ctx);
			vkCmdBindDescriptorSets(
				command_buffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				radial_depth_pipeline_layout.layout,
				0, 1, &slot_set,
				0, nullptr
			);
			radial_depth_pipeline_layout.push(ctx, push_constants);
			vulkan_cmd_draw(ctx, 3, 1, 0, 0);
		});
		graph.make_sampled(radial_depth_pass);

		// ---- Octahedral atlas entry ----
		graph.execute(cube_to_oct_pass, [&](i32)
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

			cube_to_oct_pipeline.bind(ctx);
			vkCmdBindDescriptorSets(
				command_buffer,
				VK_PIPELINE_BIND_POINT_GRAPHICS,
				cube_to_oct_pipeline_layout.layout,
				0, 1, &cube_to_oct_set,
				0, nullptr
			);
			cube_to_oct_pipeline_layout.push(ctx, push_constants);
			vulkan_cmd_draw(ctx, 3, 1, 0, 0);
		});
		graph.make_sampled(cube_to_oct_pass);

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

			DescriptorWriter writer = projection_effect.effect.descriptors.writer(
				ctx, slot_set, true);
			writer.buffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, in_sh9_buffer)
				.buffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, in_sg9_buffer)
				.sampled(2, ::lighting_pass.linear_sampler,
					lighting_pass.get_color_output(0).view)
				.commit();

			ProbeProjectionPushConstants push_constants = {
				.probe_index = in_probe_idx,
				.radiance_mode = (i32) in_radiance_mode,
				.sample_count = 1024,
			};
			graph.sampled(frame_graph_color(lighting_pass),
				VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
			graph.storage_read_write(frame_graph_buffer(in_sh9_buffer));
			graph.storage_read_write(frame_graph_buffer(in_sg9_buffer));
			graph.compute([&]() {
				projection_effect.bind_and_dispatch(
					ctx, slot_set, push_constants, 1, 1, 1);
			});
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
			const VkPipelineStageFlags2 shader_stages =
				VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
				| VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			graph.storage_read(frame_graph_buffer(in_sh9_buffer), shader_stages);
			graph.storage_read(frame_graph_buffer(in_sg9_buffer), shader_stages);
		}
	}

	void cleanup(VulkanContext* ctx)
	{
		if (!is_initialized)
		{
			return;
		}

		brdf_lut_pipeline.shutdown(ctx);
		specular_prefilter_pipeline.shutdown(ctx);
		projection_effect.shutdown(ctx);
		cube_to_oct_pipeline.shutdown(ctx);
		radial_depth_pipeline.shutdown(ctx);
		capture_lighting_pipeline.shutdown(ctx);
		capture_sky_pipeline.shutdown(ctx);
		vkDestroyPipeline(ctx->device, capture_geometry_skinned_pipeline, nullptr);
		vkDestroyPipeline(ctx->device, capture_geometry_pipeline, nullptr);
		brdf_lut_pipeline_layout.shutdown(ctx);
		specular_prefilter_pipeline_layout.shutdown(ctx);
		cube_to_oct_pipeline_layout.shutdown(ctx);
		radial_depth_pipeline_layout.shutdown(ctx);
		capture_sky_pipeline_layout.shutdown(ctx);
		vkDestroyPipelineLayout(ctx->device, capture_geometry_pipeline_layout, nullptr);
		specular_prefilter_descriptors.shutdown(ctx);
		cube_to_oct_descriptors.shutdown(ctx);
		vkDestroyDescriptorPool(ctx->device, pool, nullptr);

		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			for (i32 slot_idx = 0; slot_idx < LIGHTING_SLOTS_PER_FRAME; ++slot_idx)
			{
				lighting_slot_ubos[frame_idx][slot_idx].destroy_gpu_buffer();
			}
		}

		vulkan_context_retire_image(ctx, default_image);
		vulkan_context_retire_image(ctx, default_array_image);
		vulkan_context_retire_image(ctx, brdf_lut);
		vulkan_context_retire_image(ctx, specular_atlas);
		vkDestroySampler(ctx->device, specular_sampler, nullptr);
		geometry_pass.cleanup();
		lighting_pass.cleanup();
		radial_depth_pass.cleanup();
		cube_to_oct_pass.cleanup();
		is_initialized = false;
	}
};
