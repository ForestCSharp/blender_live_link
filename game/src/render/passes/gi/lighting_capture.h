#pragma once

#include <cstddef>
#include <cmath>

#include "core/types.h"
#include "render/core/fullscreen_pipeline.h"
#include "render/core/vulkan_context.h"
#include "render/core/render_types.h"
#include "render/core/render_pass.h"
#include "render/core/shader_module.h"
#include "render/core/frame_data.h"
#include "render/core/mesh_draw.h"
#include "render/passes/lighting/lighting_pass.h"
#include "render/passes/sky/sky_pass.h"
#include "render/passes/gi/gi_shadow_map.h"
#include "render/core/culling.h"
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
	i32 irradiance_entry_size = 10;	// 6^2 interior + 2-texel gutter; irradiance is low-frequency
	i32 moments_entry_size = 16;	// visibility needs the finer grid
	i32 specular_entry_size = 48;
	i32 specular_mip_count = 4;
};

// Mirrors the geometry capture vertex/fragment push constants.
// Pass-constant only: pushed once per cube face. Per-object data reaches the
// shader through the ObjectData SSBO, indexed by gl_InstanceIndex.
struct CaptureGeometryPushConstants
{
	HMM_Mat4 view_projection;
	HMM_Vec4 capture_position_and_radius;
};
static_assert(sizeof(CaptureGeometryPushConstants) == 80,
	"Geometry capture push constants must fit Vulkan's guaranteed minimum");
static_assert(offsetof(CaptureGeometryPushConstants, capture_position_and_radius) == 64,
	"Geometry capture radius must match the shader push-constant offset");


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

// Mirrors probe_cache_to_octahedral.frag's fs_params (used as push constants)
struct CubeToOctPushConstants
{
	i32 cubemap_render_size;
	i32 atlas_entry_size;
	i32 compute_irradiance;
	i32 use_importance_sampling;
	i32 output_moments;
};

// Mirrors probe_radiance_projection.comp's push constants
struct ProbeProjectionPushConstants
{
	i32 base_probe_index;
	i32 radiance_mode;
	i32 sample_count;
	i32 probe_count;
	i32 radiance_total_size;
	i32 radiance_entry_size;
};

struct SpecularPrefilterPushConstants
{
	i32 atlas_entry_size;
	f32 roughness;
	i32 sample_count;
	i32 probe_index;
	i32 radiance_total_size;
	i32 radiance_entry_size;
};
static_assert(sizeof(SpecularPrefilterPushConstants) == 24, "Specular prefilter push constants mismatch");

// Mirrors probe_irradiance_from_radiance.frag
struct IrradianceRelightPushConstants
{
	i32 irradiance_entry_size;
	i32 radiance_total_size;
	i32 radiance_entry_size;
	i32 compute_irradiance;
};

// Mirrors probe_sky_relight.frag
struct SkyRelightPushConstants
{
	HMM_Vec4 sun_direction;
	HMM_Vec4 light_color_and_sky_intensity;
	HMM_Vec4 planet_center_z;
	i32 atlas[4];	// total size, entry size
};
static_assert(sizeof(SkyRelightPushConstants) == 64, "Sky relight push constants mismatch");

// Everything one relight band needs from the GI scene (gi.h fills it).
struct ProbeRelightBand
{
	LightingFsParams fs_params;	// GI fields + probe_relight
	VkBuffer probes_buffer;
	VkBuffer cells_buffer;
	VkBuffer octree_buffer;
	VkBuffer sh9_buffer;
	VkBuffer sg9_buffer;
	i32 first_row;				// slot rows of the moments/radiance grid
	i32 row_count;
	i32 first_probe;			// probes in those rows (clamped to the probe count)
	i32 probe_count;
	const i32* specular_probes;	// probe index per entry ...
	const i32* specular_slots;	// ... and its specular atlas slot
	i32 specular_count;
};

// Probe geometry cache: what each probe sees per direction, independent of
// lighting (16^2 entries on the moments slot grid). Relighting reads it.
static constexpr VkFormat GI_CACHE_SURFACE_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;	// albedo.rgb, roughness
static constexpr VkFormat GI_CACHE_NORMAL_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;	// normal * 0.5 + 0.5, metallic
static constexpr VkFormat GI_CACHE_HIT_FORMAT = VK_FORMAT_R32G32_SFLOAT;		// hit distance (0 = sky), emission

struct LightingCapture
{
	LightingCaptureDesc desc;

	RenderPass geometry_pass;		// Multi: 6 face G-buffers
	RenderPass radial_depth_pass;	// Cubemap: distance moments + packed geometry cache
	// Padded octahedral atlases, one slot per probe (slot == probe index), both
	// sized to the probe count by resize_octahedral_atlases.
	RenderPass cube_to_oct_pass;	// Single: irradiance atlas (irradiance_entry_size entries)
	RenderPass moments_oct_pass;	// Single, 4 MRT on the moments grid: moments + cache surface/normal/hit
	RenderPass radiance_pass;		// Single, moments grid: relit radiance (sky + lighting.frag)
	i32 octahedral_slots_per_dimension = 0;
	i32 irradiance_atlas_total_size = 0;
	i32 moments_atlas_total_size = 0;
	VkFormat irradiance_format = VK_FORMAT_R16G16B16A16_SFLOAT;
	VkFormat radiance_format = VK_FORMAT_R16G16B16A16_SFLOAT;

	// Capture geometry (push-constant camera, layout A)
	VkPipelineLayout capture_geometry_pipeline_layout = VK_NULL_HANDLE;
	VkPipeline capture_geometry_pipeline = VK_NULL_HANDLE;
	VkPipeline capture_geometry_skinned_pipeline = VK_NULL_HANDLE;

	// Relight R1 sky (Bruneton LUT set + cache hit/probes set)
	DescriptorSetSchema sky_relight_descriptors;
	EffectPipelineLayout sky_relight_pipeline_layout;
	FullscreenPipeline sky_relight_pipeline;
	VkDescriptorSet sky_relight_sets[MAX_FRAMES_IN_FLIGHT] = {};

	// Relight R2: lighting.frag (layout C) over the geometry cache, one band per frame
	FullscreenPipeline capture_lighting_pipeline;
	static constexpr i32 LIGHTING_SLOTS_PER_FRAME = 1;
	GpuBuffer<LightingFsParams> lighting_slot_ubos[MAX_FRAMES_IN_FLIGHT][LIGHTING_SLOTS_PER_FRAME];
	VkDescriptorSet lighting_slot_sets[MAX_FRAMES_IN_FLIGHT][LIGHTING_SLOTS_PER_FRAME] = {};
	i32 lighting_slot_cursor = 0;

	// Radial depth (push constants + per-frame input set)
	EffectPipelineLayout radial_depth_pipeline_layout;
	FullscreenPipeline radial_depth_pipeline;
	static constexpr i32 RADIAL_SLOTS_PER_FRAME = NUM_CUBE_FACES * 4;
	VkDescriptorSet radial_slot_sets[MAX_FRAMES_IN_FLIGHT][RADIAL_SLOTS_PER_FRAME] = {};
	i32 radial_slot_cursor = 0;

	// Cube -> octahedral moments + cache (static inputs: the capture cubemaps never resize)
	DescriptorSetSchema cube_to_oct_descriptors;
	EffectPipelineLayout cube_to_oct_pipeline_layout;
	FullscreenPipeline moments_oct_pipeline;
	VkDescriptorSet cube_to_oct_set = VK_NULL_HANDLE;

	// Relight R3: irradiance from radiance (writes cube_to_oct_pass)
	DescriptorSetSchema irradiance_relight_descriptors;
	EffectPipelineLayout irradiance_relight_pipeline_layout;
	FullscreenPipeline irradiance_relight_pipeline;
	VkDescriptorSet irradiance_relight_sets[MAX_FRAMES_IN_FLIGHT] = {};

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
	VkSampler nearest_sampler = VK_NULL_HANDLE;	// geometry cache: never blend hit distances
	DescriptorSetSchema capture_pack_descriptors;	// radial/pack pass: face position, color, normal, rme
	DescriptorSetSchema specular_prefilter_descriptors;
	EffectPipelineLayout specular_prefilter_pipeline_layout;
	FullscreenPipeline specular_prefilter_pipeline;
	VkDescriptorSet specular_prefilter_sets[MAX_FRAMES_IN_FLIGHT] = {};	// radiance atlas, written per relight frame
	EffectPipelineLayout brdf_lut_pipeline_layout;
	FullscreenPipeline brdf_lut_pipeline;

	VkDescriptorPool pool = VK_NULL_HANDLE;

	// 1x1 white fallback for unused lighting bindings during probe capture
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

		radial_depth_pass.init((RenderPassDesc) {
			.num_outputs = 4,
			.outputs = {
				{ .format = color_format, .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR, .store_op = VK_ATTACHMENT_STORE_OP_STORE, .clear_value = {{{ 1.0f, 1.0f, 1.0f, 1.0f }}} },
				{ .format = GI_CACHE_SURFACE_FORMAT, .load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
				{ .format = GI_CACHE_NORMAL_FORMAT, .load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
				{ .format = GI_CACHE_HIT_FORMAT, .load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
			},
			.extent = render_target_extent_fixed(desc.cubemap_render_size, desc.cubemap_render_size),
			.type = ERenderPassType::Cubemap,
			.debug_label = "GI Radial Depth",
		});

		irradiance_format = ctx->capabilities.scene_color_format;
		radiance_format = ctx->capabilities.scene_color_format;
		// cube_to_oct_pass / moments_oct_pass: created by resize_octahedral_atlases.

		// ---- Set layouts ----
		{
			const DescriptorBindingSpec bindings[] = {
				{ .binding = 2, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
				{ .binding = 3, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
				{ .binding = 4, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
				{ .binding = 5, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
			};
			cube_to_oct_descriptors.init(ctx, bindings, 4,
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

		{
			const DescriptorBindingSpec bindings[] = {
				{ .binding = 0, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
				{ .binding = 1, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
				{ .binding = 2, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
				{ .binding = 3, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },
			};
			capture_pack_descriptors.init(ctx, bindings, 4, EDescriptorSetAllocation::Transient);
		}
		{
			const DescriptorBindingSpec bindings[] = {
				{ .binding = 0, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },	// cache hit
				{ .binding = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },			// probes
			};
			sky_relight_descriptors.init(ctx, bindings, 2, EDescriptorSetAllocation::Transient);
		}
		{
			const DescriptorBindingSpec binding = {
				.binding = 0, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,	// radiance
			};
			irradiance_relight_descriptors.init(ctx, &binding, 1, EDescriptorSetAllocation::Transient);
		}

		// ---- Pool + sets ----
		{
			VkDescriptorPoolSize pool_sizes[] = {
				{ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 128 },
				{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1536 },
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
				allocate_info.pSetLayouts = &capture_pack_descriptors.layout;
				VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &radial_slot_sets[frame_idx][slot_idx]));
			}
			for (i32 slot_idx = 0; slot_idx < PROJECTION_SLOTS_PER_FRAME; ++slot_idx)
			{
				allocate_info.pSetLayouts =
					&projection_effect.effect.descriptors.layout;
				VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &projection_slot_sets[frame_idx][slot_idx]));
			}
			// Relight sets are rewritten each relight frame (the radiance atlas
			// and probe buffer change with the layout), so one per frame in flight.
			allocate_info.pSetLayouts = &sky_relight_descriptors.layout;
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &sky_relight_sets[frame_idx]));
			allocate_info.pSetLayouts = &irradiance_relight_descriptors.layout;
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &irradiance_relight_sets[frame_idx]));
			allocate_info.pSetLayouts = &specular_prefilter_descriptors.layout;
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &specular_prefilter_sets[frame_idx]));
		}

		{
			VkDescriptorSetAllocateInfo allocate_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &cube_to_oct_descriptors.layout,
			};
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &cube_to_oct_set));
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
		{
			VkDescriptorSetLayout sky_relight_layouts[2] = {
				bruneton_atmosphere_pass.descriptors.layout, sky_relight_descriptors.layout,
			};
			sky_relight_pipeline_layout.init(ctx, sky_relight_layouts, 2,
				sizeof(SkyRelightPushConstants), VK_SHADER_STAGE_FRAGMENT_BIT);
		}
		irradiance_relight_pipeline_layout.init(ctx, &irradiance_relight_descriptors.layout, 1,
			sizeof(IrradianceRelightPushConstants), VK_SHADER_STAGE_FRAGMENT_BIT);
		radial_depth_pipeline_layout.init(ctx, &capture_pack_descriptors.layout, 1,
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
			VkFormat sky_formats[1] = { radiance_format };
			sky_relight_pipeline.init(ctx, {
				.vertex_shader_path = "bin/shaders/cubemap_to_octahedral.vert.spv",
				.fragment_shader_path = "bin/shaders/probe_sky_relight.frag.spv",
				.pipeline_layout = sky_relight_pipeline_layout.layout,
				.color_formats = sky_formats,
				.color_format_count = 1,
			});
		}
		{
			VkFormat lighting_formats[1] = { radiance_format };
			capture_lighting_pipeline.init(ctx, {
				.vertex_shader_path = "bin/shaders/lighting.vert.spv",
				.fragment_shader_path = "bin/shaders/lighting.frag.spv",
				.pipeline_layout = ::lighting_pass.pipeline_layout,
				.color_formats = lighting_formats,
				.color_format_count = 1,
			});
		}
		{
			VkFormat radial_formats[4] = { color_format, GI_CACHE_SURFACE_FORMAT, GI_CACHE_NORMAL_FORMAT, GI_CACHE_HIT_FORMAT };
			radial_depth_pipeline.init(ctx, {
				.vertex_shader_path = "bin/shaders/radial_depth.vert.spv",
				.fragment_shader_path = "bin/shaders/radial_depth.frag.spv",
				.pipeline_layout = radial_depth_pipeline_layout.layout,
				.color_formats = radial_formats,
				.color_format_count = 4,
			});
		}
		{
			VkFormat irradiance_formats[1] = { irradiance_format };
			irradiance_relight_pipeline.init(ctx, {
				.vertex_shader_path = "bin/shaders/cubemap_to_octahedral.vert.spv",
				.fragment_shader_path = "bin/shaders/probe_irradiance_from_radiance.frag.spv",
				.pipeline_layout = irradiance_relight_pipeline_layout.layout,
				.color_formats = irradiance_formats,
				.color_format_count = 1,
			});
			VkFormat moments_formats[4] = { color_format, GI_CACHE_SURFACE_FORMAT, GI_CACHE_NORMAL_FORMAT, GI_CACHE_HIT_FORMAT };
			moments_oct_pipeline.init(ctx, {
				.vertex_shader_path = "bin/shaders/cubemap_to_octahedral.vert.spv",
				.fragment_shader_path = "bin/shaders/probe_cache_to_octahedral.frag.spv",
				.pipeline_layout = cube_to_oct_pipeline_layout.layout,
				.color_formats = moments_formats,
				.color_format_count = 4,
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
			sampler_create_info.magFilter = VK_FILTER_NEAREST;
			sampler_create_info.minFilter = VK_FILTER_NEAREST;
			sampler_create_info.maxLod = 0.0f;
			VK_CHECK(vkCreateSampler(ctx->device, &sampler_create_info, nullptr, &nearest_sampler));
			vulkan_set_object_name(ctx, VK_OBJECT_TYPE_SAMPLER, (u64)nearest_sampler, "GI Nearest Sampler");
			vulkan_set_object_name(ctx, VK_OBJECT_TYPE_SAMPLER, (u64)specular_sampler, "GI Specular Atlas Sampler");
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
			writer.sampled(2, frame_data.linear_sampler,
					radial_depth_pass.get_color_output(0).view)
				.sampled(3, nearest_sampler, radial_depth_pass.get_color_output(1).view)
				.sampled(4, nearest_sampler, radial_depth_pass.get_color_output(2).view)
				.sampled(5, nearest_sampler, radial_depth_pass.get_color_output(3).view);
			writer.commit();
		}

		GIShadowMap::init();
		is_initialized = true;
		resize_octahedral_atlases(ctx, 1);
	}

	// Sizes the octahedral atlases to hold in_probe_count slots on a shared
	// slots-per-dimension grid, and always zeroes them: a new layout must not
	// inherit an old one's geometry cache (a zeroed cache entry means "all
	// sky", which is exactly what the sky fallback probe needs). Leaves them
	// SHADER_READ_ONLY, since the lighting pass binds them before any capture.
	void resize_octahedral_atlases(VulkanContext* ctx, i32 in_probe_count)
	{
		assert(is_initialized);
		const i32 slots_per_dimension = (i32)std::ceil(std::sqrt((f64)std::max(in_probe_count, 1)));
		if (slots_per_dimension != octahedral_slots_per_dimension)
		{
			recreate_octahedral_atlases(ctx, slots_per_dimension);
		}
		clear_octahedral_atlases(ctx);
	}

	void recreate_octahedral_atlases(VulkanContext* ctx, const i32 slots_per_dimension)
	{
		octahedral_slots_per_dimension = slots_per_dimension;
		irradiance_atlas_total_size = slots_per_dimension * desc.irradiance_entry_size;
		moments_atlas_total_size = slots_per_dimension * desc.moments_entry_size;
		assert(moments_atlas_total_size <= (i32)ctx->capabilities.properties.limits.maxImageDimension2D);

		// init() retires the previous targets against the current frame fence.
		cube_to_oct_pass.init((RenderPassDesc) {
			.num_outputs = 1,
			.outputs = {
				{ .format = irradiance_format, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
			},
			.extent = render_target_extent_fixed(irradiance_atlas_total_size, irradiance_atlas_total_size),
			.type = ERenderPassType::Single,
			.debug_label = "GI Irradiance Atlas",
		});
		radiance_pass.init((RenderPassDesc) {
			.num_outputs = 1,
			.outputs = {
				{ .format = radiance_format, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
			},
			.extent = render_target_extent_fixed(moments_atlas_total_size, moments_atlas_total_size),
			.type = ERenderPassType::Single,
			.debug_label = "GI Radiance Atlas",
		});
		moments_oct_pass.init((RenderPassDesc) {
			.num_outputs = 4,
			.outputs = {
				{ .format = color_format, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
				{ .format = GI_CACHE_SURFACE_FORMAT, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
				{ .format = GI_CACHE_NORMAL_FORMAT, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
				{ .format = GI_CACHE_HIT_FORMAT, .load_op = VK_ATTACHMENT_LOAD_OP_LOAD, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
			},
			.extent = render_target_extent_fixed(moments_atlas_total_size, moments_atlas_total_size),
			.type = ERenderPassType::Single,
			.debug_label = "GI Moments Atlas",
		});
	}

	void clear_octahedral_atlases(VulkanContext* ctx)
	{
		auto clear_atlases = [&](VkCommandBuffer in_command_buffer)
		{
			const VkClearColorValue clear_value = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
			const VkImageSubresourceRange clear_range = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = VK_REMAINING_ARRAY_LAYERS,
			};
			for (RenderPass* pass : { &cube_to_oct_pass, &moments_oct_pass, &radiance_pass })
			{
				for (i32 output_idx = 0; output_idx < pass->desc.num_outputs; ++output_idx)
				{
					GpuImage& atlas_image = pass->get_color_output(output_idx);
					gpu_image_transition(in_command_buffer, atlas_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true);
					vkCmdClearColorImage(in_command_buffer, atlas_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &clear_range);
					gpu_image_transition(in_command_buffer, atlas_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				}
			}
		};
		if (vulkan_current_frame(ctx).recording)
		{
			clear_atlases(vulkan_current_command_buffer(ctx));
		}
		else
		{
			vulkan_context_immediate_submit(ctx, clear_atlases);
		}
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
					| VK_IMAGE_USAGE_TRANSFER_SRC_BIT	// GI bake readback
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

	// R4: GGX-prefilters the listed probes' radiance into their specular slots.
	// One render pass per mip with every probe drawn inside it, instead of one
	// per probe per mip.
	void render_specular_prefilter_batch(FrameRenderGraph& graph, VulkanContext* ctx,
		const i32* in_probes, const i32* in_slots, const i32 in_count)
	{
		assert(specular_atlas.image != VK_NULL_HANDLE);
		if (in_count <= 0)
		{
			return;
		}
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		VkDescriptorSet set = specular_prefilter_sets[ctx->frame_index];
		DescriptorWriter writer = specular_prefilter_descriptors.writer(ctx, set, true);
		writer.sampled(0, frame_data.linear_sampler, radiance_pass.get_color_output(0).view);
		writer.commit();

		for (i32 mip = 0; mip < desc.specular_mip_count; ++mip)
		{
			const i32 mip_total_size = specular_atlas_total_size >> mip;
			const i32 mip_entry_size = desc.specular_entry_size >> mip;
			graph.sampled(frame_graph_color(radiance_pass));
			FrameGraphColorAttachment attachment = {
				.image = frame_graph_mip(specular_atlas, (u32)mip),
				.load_op = VK_ATTACHMENT_LOAD_OP_LOAD,
			};
			graph.render(&attachment, 1, [&]() {
				specular_prefilter_pipeline.bind(ctx);
				vkCmdBindDescriptorSets(
					command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
					specular_prefilter_pipeline_layout.layout,
					0, 1, &set, 0, nullptr);
				for (i32 i = 0; i < in_count; ++i)
				{
					assert(in_slots[i] >= 0 && in_slots[i] < specular_atlas_capacity);
					const AtlasViewport atlas_viewport = get_atlas_viewport(
						mip_total_size, mip_entry_size, in_slots[i]);
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
					const SpecularPrefilterPushConstants push_constants = {
						.atlas_entry_size = mip_entry_size,
						.roughness = (f32)mip / (f32)(desc.specular_mip_count - 1),
						.sample_count = mip == 0 ? 1 : 256,
						.probe_index = in_probes[i],
						.radiance_total_size = moments_atlas_total_size,
						.radiance_entry_size = desc.moments_entry_size,
					};
					specular_prefilter_pipeline_layout.push(ctx, push_constants);
					vulkan_cmd_draw(ctx, 3, 1, 0, 0);
				}
			});
			graph.sampled(frame_graph_mip(specular_atlas, (u32)mip));
		}
	}

	// Sets a viewport covering the whole atlas (with the framework's
	// negative-height Y flip) and a scissor limited to slot rows.
	void set_band_viewport(VkCommandBuffer in_command_buffer, const i32 in_total_size,
		const i32 in_entry_size, const i32 in_first_row, const i32 in_row_count)
	{
		const VkViewport viewport = {
			.x = 0.0f,
			.y = (f32) in_total_size,
			.width = (f32) in_total_size,
			.height = -(f32) in_total_size,
			.minDepth = 0.0f,
			.maxDepth = 1.0f,
		};
		vkCmdSetViewport(in_command_buffer, 0, 1, &viewport);
		const VkRect2D scissor = {
			.offset = { 0, in_first_row * in_entry_size },
			.extent = { (u32) in_total_size, (u32) (in_row_count * in_entry_size) },
		};
		vkCmdSetScissor(in_command_buffer, 0, 1, &scissor);
	}

	// Relights one band of slot rows: R1 sky + R2 lighting into radiance, R3
	// irradiance, R4 specular for the listed specular probes, R5 SH9/SG9.
	void relight_band(FrameRenderGraph& graph, VulkanContext* ctx, State& in_state, const ProbeRelightBand& in_band)
	{
		assert(is_initialized);
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		const u32 frame_index = ctx->frame_index;

		// ---- R2 inputs: layout C, with the geometry cache in the G-buffer slots ----
		assert(lighting_slot_cursor < LIGHTING_SLOTS_PER_FRAME);
		const i32 slot = lighting_slot_cursor++;
		lighting_slot_ubos[frame_index][slot].update_gpu_buffer(&in_band.fs_params, sizeof(LightingFsParams));
		// lighting.frag's G-buffer bindings: color = cache surface, position =
		// cache hit, normal = cache normal (rme unused in relight mode).
		const GpuImage cache_as_gbuffer[4] = {
			moments_oct_pass.get_color_output(1),
			moments_oct_pass.get_color_output(3),
			moments_oct_pass.get_color_output(2),
			moments_oct_pass.get_color_output(1),
		};
		lighting_pass_write_set(ctx,
			lighting_slot_sets[frame_index][slot],
			lighting_slot_ubos[frame_index][slot].get_gpu_buffer(),
			cache_as_gbuffer,
			GIShadowMap::valid ? GIShadowMap::pass.get_color_output(0).view : default_array_image.view,
			default_image.view,
			in_state.lighting.point_buffers[in_state.lighting.buffer_index].get_gpu_buffer(),
			in_state.lighting.spot_buffers[in_state.lighting.buffer_index].get_gpu_buffer(),
			in_state.lighting.sun_buffers[in_state.lighting.buffer_index].get_gpu_buffer(),
			in_band.probes_buffer,
			in_band.cells_buffer,
			cube_to_oct_pass.get_color_output(0).view,
			moments_oct_pass.get_color_output(0).view,
			in_band.sh9_buffer,
			in_band.sg9_buffer,
			in_band.octree_buffer,
			specular_sampler,
			specular_atlas.view,
			brdf_lut.view,
			bruneton_atmosphere_pass.parameter_buffers[frame_index].get_gpu_buffer(),
			bruneton_atmosphere_pass.has_precomputed
				? bruneton_atmosphere_pass.transmittance_pass.get_color_output(0).view
				: default_image.view,
			default_image.view);

		// ---- R1 inputs ----
		DescriptorWriter sky_writer = sky_relight_descriptors.writer(ctx, sky_relight_sets[frame_index], true);
		sky_writer.sampled(0, nearest_sampler, moments_oct_pass.get_color_output(3).view)
			.buffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, in_band.probes_buffer)
			.commit();
		const bool sky_enabled = in_state.gi.render_sky_to_probes
			&& sky_pass.has_active_atmosphere
			&& bruneton_atmosphere_pass.has_precomputed;
		const SkyRelightPushConstants sky_push = {
			.sun_direction = HMM_V4V(sky_pass.active_sun_direction, 0.0f),
			.light_color_and_sky_intensity = HMM_V4V(sky_pass.active_sun_color,
				sky_enabled ? sky_pass.active_parameters.sky_intensity : 0.0f),
			.planet_center_z = HMM_V4(sky_pass.active_parameters.planet_center_z_m, 0.0f, 0.0f, 0.0f),
			.atlas = { moments_atlas_total_size, desc.moments_entry_size, 0, 0 },
		};

		// ---- R1 + R2: radiance band ----
		graph.execute(radiance_pass, [&](i32)
		{
			set_band_viewport(command_buffer, moments_atlas_total_size, desc.moments_entry_size,
				in_band.first_row, in_band.row_count);
			// Sky texels get sky radiance (or 0 when there is no sky: intensity 0).
			sky_relight_pipeline.bind(ctx);
			const VkDescriptorSet sky_sets[2] = {
				bruneton_atmosphere_pass.descriptors.persistent_sets.sets[frame_index],
				sky_relight_sets[frame_index],
			};
			vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
				sky_relight_pipeline_layout.layout, 0, 2, sky_sets, 0, nullptr);
			sky_relight_pipeline_layout.push(ctx, sky_push);
			vulkan_cmd_draw(ctx, 3, 1, 0, 0);

			capture_lighting_pipeline.bind(ctx);
			vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
				::lighting_pass.pipeline_layout, 0, 1, &lighting_slot_sets[frame_index][slot], 0, nullptr);
			vulkan_cmd_draw(ctx, 3, 1, 0, 0);
		});
		graph.make_sampled(radiance_pass);

		// ---- R3: irradiance band ----
		DescriptorWriter irradiance_writer = irradiance_relight_descriptors.writer(
			ctx, irradiance_relight_sets[frame_index], true);
		irradiance_writer.sampled(0, nearest_sampler, radiance_pass.get_color_output(0).view).commit();
		graph.execute(cube_to_oct_pass, [&](i32)
		{
			set_band_viewport(command_buffer, irradiance_atlas_total_size, desc.irradiance_entry_size,
				in_band.first_row, in_band.row_count);
			irradiance_relight_pipeline.bind(ctx);
			vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
				irradiance_relight_pipeline_layout.layout, 0, 1, &irradiance_relight_sets[frame_index], 0, nullptr);
			const IrradianceRelightPushConstants irradiance_push = {
				.irradiance_entry_size = desc.irradiance_entry_size,
				.radiance_total_size = moments_atlas_total_size,
				.radiance_entry_size = desc.moments_entry_size,
				.compute_irradiance = in_state.gi.compute_irradiance ? 1 : 0,
			};
			irradiance_relight_pipeline_layout.push(ctx, irradiance_push);
			vulkan_cmd_draw(ctx, 3, 1, 0, 0);
		});
		graph.make_sampled(cube_to_oct_pass);

		// ---- R4: specular ----
		render_specular_prefilter_batch(graph, ctx,
			in_band.specular_probes, in_band.specular_slots, in_band.specular_count);

		// ---- R5: SH9 / SG9 for the band's probes, one dispatch each ----
		if (in_band.probe_count > 0)
		{
			for (EProbeRadianceMode radiance_mode : { EProbeRadianceMode::SH9, EProbeRadianceMode::SG9 })
			{
				assert(projection_slot_cursor < PROJECTION_SLOTS_PER_FRAME);
				VkDescriptorSet projection_set = projection_slot_sets[frame_index][projection_slot_cursor++];
				DescriptorWriter writer = projection_effect.effect.descriptors.writer(ctx, projection_set, true);
				writer.buffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, in_band.sh9_buffer)
					.buffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, in_band.sg9_buffer)
					.sampled(2, frame_data.linear_sampler, radiance_pass.get_color_output(0).view)
					.commit();
				const ProbeProjectionPushConstants push_constants = {
					.base_probe_index = in_band.first_probe,
					.radiance_mode = (i32) radiance_mode,
					.sample_count = 256,
					.probe_count = in_band.probe_count,
					.radiance_total_size = moments_atlas_total_size,
					.radiance_entry_size = desc.moments_entry_size,
				};
				graph.sampled(frame_graph_color(radiance_pass), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
				graph.storage_read_write(frame_graph_buffer(in_band.sh9_buffer));
				graph.storage_read_write(frame_graph_buffer(in_band.sg9_buffer));
				graph.compute([&]() {
					projection_effect.bind_and_dispatch(
						ctx, projection_set, push_constants, (u32) in_band.probe_count, 1, 1);
				});
			}
			const VkPipelineStageFlags2 shader_stages =
				VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			graph.storage_read(frame_graph_buffer(in_band.sh9_buffer), shader_stages);
			graph.storage_read(frame_graph_buffer(in_band.sg9_buffer), shader_stages);
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
		i32 in_atlas_idx,			// slot on the moments/cache grid (== probe index)
		bool in_should_render_geometry,
		f32 in_max_radial_depth
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
				// Constant for the face; pushed once instead of once per draw.
				const CaptureGeometryPushConstants push_constants = {
					.view_projection = view_projection,
					.capture_position_and_radius = HMM_V4V(
						in_location,
						cull_to_probe_influence ? in_max_radial_depth : 0.0f),
				};
				vulkan_cmd_push_constants(
					ctx,
					capture_geometry_pipeline_layout,
					VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
					0, sizeof(push_constants), &push_constants);

				MeshDrawBindings bindings;
				for (i32 render_object_index : visible)
				{
					auto found = in_state.scene.objects.find(in_state.cull_entries[render_object_index].object_id);
					if (found == in_state.scene.objects.end())
					{
						continue;
					}

					draw_indexed_mesh(ctx, found->second.mesh, render_object_index,
						capture_geometry_pipeline, capture_geometry_skinned_pipeline, bindings);
				}
			}
		});

		// The pack (radial depth) pass samples the face G-buffers
		graph.make_sampled(geometry_pass);

		// ---- Radial depth cubemap ----
		graph.execute(radial_depth_pass, [&](i32 face_idx)
		{
			assert(radial_slot_cursor < RADIAL_SLOTS_PER_FRAME);
			const i32 slot = radial_slot_cursor++;
			VkDescriptorSet slot_set = radial_slot_sets[frame_index][slot];

			// G-buffer outputs: 0 color, 1 position, 2 normal, 3 roughness/metallic/emissive.
			const i32 gbuffer_output_for_binding[4] = { 1, 0, 2, 3 };
			VkDescriptorImageInfo input_infos[4];
			VkWriteDescriptorSet writes[4];
			for (i32 binding = 0; binding < 4; ++binding)
			{
				input_infos[binding] = (VkDescriptorImageInfo) {
					.sampler = ::lighting_pass.linear_sampler,
					.imageView = geometry_pass.get_color_output(gbuffer_output_for_binding[binding], face_idx).view,
					.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				};
				writes[binding] = (VkWriteDescriptorSet) {
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = slot_set,
					.dstBinding = (u32) binding,
					.descriptorCount = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.pImageInfo = &input_infos[binding],
				};
			}
			vulkan_update_descriptor_sets(ctx, 4, writes);

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

		// ---- Octahedral atlas entries (irradiance, then moments) ----
		const auto draw_octahedral_entry = [&](RenderPass& in_pass, FullscreenPipeline& in_pipeline,
			const i32 in_total_size, const i32 in_entry_size, const bool in_moments)
		{
			graph.execute(in_pass, [&](i32)
			{
				const AtlasViewport atlas_viewport = get_atlas_viewport(in_total_size, in_entry_size, in_atlas_idx);

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
					.atlas_entry_size = in_entry_size,
					.compute_irradiance = in_state.gi.compute_irradiance ? 1 : 0,
					.use_importance_sampling = 1,
					.output_moments = in_moments ? 1 : 0,
				};

				in_pipeline.bind(ctx);
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
			graph.make_sampled(in_pass);
		};
		draw_octahedral_entry(moments_oct_pass, moments_oct_pipeline,
			moments_atlas_total_size, desc.moments_entry_size, true);

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
		irradiance_relight_pipeline.shutdown(ctx);
		irradiance_relight_pipeline_layout.shutdown(ctx);
		irradiance_relight_descriptors.shutdown(ctx);
		sky_relight_descriptors.shutdown(ctx);
		moments_oct_pipeline.shutdown(ctx);
		radial_depth_pipeline.shutdown(ctx);
		capture_lighting_pipeline.shutdown(ctx);
		sky_relight_pipeline.shutdown(ctx);
		vkDestroyPipeline(ctx->device, capture_geometry_skinned_pipeline, nullptr);
		vkDestroyPipeline(ctx->device, capture_geometry_pipeline, nullptr);
		brdf_lut_pipeline_layout.shutdown(ctx);
		specular_prefilter_pipeline_layout.shutdown(ctx);
		cube_to_oct_pipeline_layout.shutdown(ctx);
		radial_depth_pipeline_layout.shutdown(ctx);
		sky_relight_pipeline_layout.shutdown(ctx);
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
		vkDestroySampler(ctx->device, nearest_sampler, nullptr);
		capture_pack_descriptors.shutdown(ctx);
		geometry_pass.cleanup();
		radiance_pass.cleanup();
		radial_depth_pass.cleanup();
		cube_to_oct_pass.cleanup();
		moments_oct_pass.cleanup();
		GIShadowMap::cleanup();
		is_initialized = false;
	}
};
