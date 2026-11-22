#pragma once

#include <cstring>

#include "render/core/frame_render_graph.h"
#include "render/core/fullscreen_pipeline.h"
#include "render/core/mesh_draw.h"
#include "render/passes/shadows/shadow_depth_pass.h"
#include "render/passes/sky/bruneton_atmosphere_pass.h"
#include "render/passes/sky/sky_pass.h"
#include "render/passes/sky/solar_calibration.h"
#include "render/passes/sdf_gi/sdf_gi_math.h"
#include "state/state.h"

// SDF GI: a Lumen-style alternative to the probe octree (gi.h), with no
// hardware ray tracing and no bake.
//
//   static meshes --3-axis ortho raster--> voxel albedo/normal/emission
//   occupancy --jump flood--> distance clipmap (LEVEL_COUNT x LEVEL_RES^3,
//                              camera-centred, voxel doubles per level)
//   voxels + lights + sky + previous radiance --> voxel radiance (both sides)
//   G-buffer --> screen probes (1 per 16^2 tile, 8x8 rays through the SDF)
//     --> temporal --> spatial --> SH9 --> per-pixel irradiance for lighting.frag
//
// Levels are stacked along z in one volume per quantity (sdf_common.h). At most
// one level is rebuilt and one is relit per frame.
namespace SdfGi
{
	constexpr i32 VOXELIZE_RES = 256;		// 2x LEVEL_RES: no conservative raster on MoltenVK
	constexpr i32 PROBE_TILE = 16;			// SDF_PROBE_TILE
	constexpr i32 PROBE_RES = 8;			// SDF_PROBE_RES
	constexpr i32 SKY_RES = 32;				// SDF_SKY_RES
	constexpr i32 CONVERGE_FRAMES = 48;		// automation: frames after the last rebuild before "converged"
	constexpr i32 FAST_RESPONSE_FRAMES = 16;	// lower hysteresis this long after a lighting change
	constexpr i32 PROBE_FILTER_PASSES = 3;	// odd: the last pass lands in `filtered` (Lumen also uses 3)
	static_assert(PROBE_FILTER_PASSES % 2 == 1);
	constexpr i32 JFA_PASSES = 8;			// steps 64..1 plus one extra step of 1

	// std140, mirrors sdf_common.h SdfGiParams.
	struct GpuParams
	{
		HMM_Vec4 level_origin_voxel[LEVEL_COUNT];
		HMM_Mat4 view_projection;
		HMM_Mat4 prev_view_projection;
		HMM_Vec4 camera_position;
		HMM_Vec4 sun_direction;
		i32 counts[4];
		i32 screen[4];
		HMM_Vec4 settings;
		i32 debug[4];
		HMM_Mat4 inv_view_projection;
	};
	static_assert(sizeof(GpuParams) == 352, "Must match sdf_common.h SdfGiParams");

	struct LevelPush { i32 level[4]; };
	struct ScreenTracePush { i32 offset[4]; };	// ivec4 push for the screen-trace and resolve passes; meaning per shader Push block
	struct JfaStepPush { i32 step[4]; };
	struct JfaResolvePush { i32 level[4]; HMM_Vec4 voxel_size; };
	struct VoxelizePush { HMM_Mat4 view_projection; HMM_Vec4 level_origin_voxel; i32 level[4]; };
	static_assert(sizeof(VoxelizePush) == 96, "Must match sdf_voxelize.vert/frag");
	struct SkyPush
	{
		HMM_Vec4 camera_position;
		HMM_Vec4 sun_direction;
		HMM_Vec4 sky_color_and_intensity;
		HMM_Vec4 sun_irradiance_and_atmosphere;
		i32 flags[4];
	};
	static_assert(sizeof(SkyPush) == 80, "Must match sdf_sky.comp");

	struct Level
	{
		HMM_Vec3 built_origin = {};
		f32 built_voxel_size = 0.0f;
		bool built = false;
		bool dirty = true;
	};

	struct Pass
	{
		bool initialized = false;

		// Clipmap volumes (all levels stacked along z)
		GpuImage distance = {};
		GpuImage albedo = {};
		GpuImage normal_emission = {};
		GpuImage radiance_front = {};
		GpuImage radiance_back = {};
		GpuImage jfa[2] = {};
		GpuImage voxelize_target = {};
		GpuImage sky = {};
		GpuBuffer<HMM_Vec4> sun_buffer;		// written by sdf_sky.comp (atmosphere sun)
		GpuBuffer<HMM_Vec4> sun_fallback[MAX_FRAMES_IN_FLIGHT];	// CPU-written when there is no atmosphere
		VkBuffer active_sun_buffer = VK_NULL_HANDLE;
		VkBuffer params_buffer = VK_NULL_HANDLE;	// this frame's GpuParams

		// Screen probes (sized to the render resolution)
		VkExtent2D screen_extent = {};
		i32 probe_grid_width = 0;
		i32 probe_grid_height = 0;
		GpuImage probe_position[2] = {};
		GpuImage probe_normal[2] = {};
		GpuImage trace = {};
		GpuImage history[2] = {};
		GpuImage filtered = {};
		GpuImage filtered_scratch = {};		// spatial filter ping-pong
		GpuBuffer<HMM_Vec4> probe_sh;
		GpuImage irradiance = {};
		GpuImage specular = {};
		GpuImage reflection_trace = {};			// half resolution
		GpuImage reflection_history[2] = {};	// a = view depth
		GpuImage diffuse_history[2] = {};		// per-pixel irradiance, a = view depth

		// Pipelines
		DescriptorSetSchema shared_set;		// SDF params + sampled volumes; set 1 of the probe passes
		ComputeEffect clear_pipeline;
		ComputeEffect jfa_seed_pipeline;
		ComputeEffect jfa_step_pipeline;
		ComputeEffect jfa_resolve_pipeline;
		ComputeEffect sky_pipeline;
		ComputeEffect voxel_lighting_pipeline;
		ComputeEffect probe_place_pipeline;
		ComputeEffect probe_trace_pipeline;
		ComputeEffect probe_temporal_pipeline;
		ComputeEffect probe_filter_pipeline;
		ComputeEffect probe_sh_pipeline;
		ComputeEffect resolve_pipeline;
		ComputeEffect reflection_trace_pipeline;
		ComputeEffect reflection_resolve_pipeline;
		DescriptorSetSchema voxelize_set;
		VkPipelineLayout voxelize_pipeline_layout = VK_NULL_HANDLE;
		VkPipeline voxelize_pipeline = VK_NULL_HANDLE;
		VkSampler linear_sampler = VK_NULL_HANDLE;
		VkSampler nearest_sampler = VK_NULL_HANDLE;
		PerFrameUniform<GpuParams> params;

		// Clipmap + temporal state
		Level levels[LEVEL_COUNT];
		i32 rebuild_cursor = 0;
		i32 lighting_cursor = 0;
		u64 frame_counter = 0;
		bool history_valid = false;
		bool reflection_history_valid = false;
		bool diffuse_history_valid = false;
		HMM_Mat4 prev_view_projection = HMM_M4D(1.0f);
		GeometryContentHash lighting_signature = {};
		i32 fast_response_frames = 0;
		DynamicArray<i32> visible_scratch;
	};

	inline Pass pass;

	inline GpuImage make_image(VulkanContext* ctx, u32 width, u32 height, u32 depth, VkFormat format,
		VkImageUsageFlags usage, const char* label)
	{
		return gpu_image_create(ctx->allocator, ctx->device, {
			.width = width, .height = height, .format = format,
			.usage = usage, .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
			.depth = depth, .label = label,
		});
	}

	constexpr VkImageUsageFlags STORAGE_SAMPLED = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

	inline DescriptorBindingSpec compute_binding(u32 binding, VkDescriptorType type)
	{
		return { .binding = binding, .type = type, .stages = VK_SHADER_STAGE_COMPUTE_BIT };
	}

	inline void init_compute(VulkanContext* ctx, ComputeEffect& effect, const char* shader,
		const DescriptorBindingSpec* bindings, u32 binding_count, u32 push_size, bool shared)
	{
		effect.init(ctx, {
			.shader_path = shader,
			.bindings = bindings,
			.binding_count = binding_count,
			.push_constant_size = push_size,
			.global_set_layout = shared ? pass.shared_set.layout : VK_NULL_HANDLE,
		});
	}

	inline void create_voxelize_pipeline(VulkanContext* ctx)
	{
		const DescriptorBindingSpec bindings[] = {
			{ .binding = 0, .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .stages = VK_SHADER_STAGE_FRAGMENT_BIT },
			{ .binding = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .stages = VK_SHADER_STAGE_FRAGMENT_BIT },
		};
		pass.voxelize_set.init(ctx, bindings, 2, EDescriptorSetAllocation::Transient);
		const VkDescriptorSetLayout set_layouts[2] = { frame_data.per_frame_layout, pass.voxelize_set.layout };
		const VkPushConstantRange push_range = {
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			.size = sizeof(VoxelizePush),
		};
		const VkPipelineLayoutCreateInfo layout_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 2,
			.pSetLayouts = set_layouts,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_range,
		};
		VK_CHECK(vkCreatePipelineLayout(ctx->device, &layout_info, nullptr, &pass.voxelize_pipeline_layout));

		VkShaderModule vertex_module = create_shader_module_from_file(ctx->device, "bin/shaders/sdf_voxelize.vert.spv");
		VkShaderModule fragment_module = create_shader_module_from_file(ctx->device, "bin/shaders/sdf_voxelize.frag.spv");
		const VkPipelineShaderStageCreateInfo stages[] = {
			{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex_module, .pName = "main" },
			{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment_module, .pName = "main" },
		};
		const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		const VkPipelineDynamicStateCreateInfo dynamic_state = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = 2,
			.pDynamicStates = dynamic_states,
		};
		const VkVertexInputBindingDescription vertex_binding = {
			.binding = 0, .stride = sizeof(Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		};
		const VkVertexInputAttributeDescription vertex_attributes[] = {
			{ .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, position) },
			{ .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, normal) },
			{ .location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, texcoord) },
		};
		const VkPipelineVertexInputStateCreateInfo vertex_input = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = 1,
			.pVertexBindingDescriptions = &vertex_binding,
			.vertexAttributeDescriptionCount = 3,
			.pVertexAttributeDescriptions = vertex_attributes,
		};
		const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};
		const VkPipelineViewportStateCreateInfo viewport = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.scissorCount = 1,
		};
		const VkPipelineRasterizationStateCreateInfo rasterization = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_NONE,
			.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
			.lineWidth = 1.0f,
		};
		const VkPipelineMultisampleStateCreateInfo multisampling = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		};
		const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		};
		// The target only exists because dynamic rendering needs an extent; nothing is written to it.
		const VkPipelineColorBlendAttachmentState blend_attachment = { .colorWriteMask = 0 };
		const VkPipelineColorBlendStateCreateInfo color_blending = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = &blend_attachment,
		};
		const VkFormat target_format = VK_FORMAT_R8_UNORM;
		const VkPipelineRenderingCreateInfo rendering_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = 1,
			.pColorAttachmentFormats = &target_format,
		};
		const VkGraphicsPipelineCreateInfo pipeline_info = {
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.pNext = &rendering_info,
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
			.layout = pass.voxelize_pipeline_layout,
		};
		VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_info, &pass.voxelize_pipeline));
		vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
		vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
	}

	inline void init(VulkanContext* ctx)
	{
		const u32 volume_depth = LEVEL_RES * LEVEL_COUNT;
		pass.distance = make_image(ctx, LEVEL_RES, LEVEL_RES, volume_depth, VK_FORMAT_R16_SFLOAT, STORAGE_SAMPLED, "SDF GI Distance");
		pass.albedo = make_image(ctx, LEVEL_RES, LEVEL_RES, volume_depth, VK_FORMAT_R8G8B8A8_UNORM, STORAGE_SAMPLED, "SDF GI Albedo");
		pass.normal_emission = make_image(ctx, LEVEL_RES, LEVEL_RES, volume_depth, VK_FORMAT_R8G8B8A8_UNORM, STORAGE_SAMPLED, "SDF GI Normal");
		pass.radiance_front = make_image(ctx, LEVEL_RES, LEVEL_RES, volume_depth, VK_FORMAT_R16G16B16A16_SFLOAT, STORAGE_SAMPLED, "SDF GI Radiance Front");
		pass.radiance_back = make_image(ctx, LEVEL_RES, LEVEL_RES, volume_depth, VK_FORMAT_R16G16B16A16_SFLOAT, STORAGE_SAMPLED, "SDF GI Radiance Back");
		for (i32 i = 0; i < 2; ++i)
		{
			pass.jfa[i] = make_image(ctx, LEVEL_RES, LEVEL_RES, LEVEL_RES, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT, "SDF GI Jump Flood");
		}
		pass.voxelize_target = make_image(ctx, VOXELIZE_RES, VOXELIZE_RES, 1, VK_FORMAT_R8_UNORM,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "SDF GI Voxelize Target");
		pass.sky = make_image(ctx, SKY_RES, SKY_RES, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
			STORAGE_SAMPLED | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "SDF GI Sky");
		pass.sun_buffer = GpuBuffer((GpuBufferDesc<HMM_Vec4>){
			.data = nullptr,
			.size = sizeof(HMM_Vec4),
			.usage = { .storage_buffer = true },
			.label = "SDF GI Sun",
		});
		pass.sun_buffer.get_gpu_buffer();
		for (GpuBuffer<HMM_Vec4>& buffer : pass.sun_fallback)
		{
			buffer = GpuBuffer((GpuBufferDesc<HMM_Vec4>){
				.data = nullptr,
				.size = sizeof(HMM_Vec4),
				.usage = { .storage_buffer = true, .stream_update = true },
				.label = "SDF GI Sun (no atmosphere)",
			});
			buffer.get_gpu_buffer();
		}

		const VkSamplerCreateInfo linear_info = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_LINEAR,
			.minFilter = VK_FILTER_LINEAR,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};
		VK_CHECK(vkCreateSampler(ctx->device, &linear_info, nullptr, &pass.linear_sampler));
		VkSamplerCreateInfo nearest_info = linear_info;
		nearest_info.magFilter = VK_FILTER_NEAREST;
		nearest_info.minFilter = VK_FILTER_NEAREST;
		VK_CHECK(vkCreateSampler(ctx->device, &nearest_info, nullptr, &pass.nearest_sampler));
		pass.params.init("SDF GI Params");

		const VkDescriptorType SAMPLED = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		const VkDescriptorType STORAGE = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		const VkDescriptorType SSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		const VkDescriptorType UBO = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		{
			const DescriptorBindingSpec bindings[] = {
				compute_binding(0, UBO), compute_binding(1, SAMPLED), compute_binding(2, SAMPLED),
				compute_binding(3, SAMPLED), compute_binding(4, SAMPLED), compute_binding(5, SAMPLED),
				compute_binding(6, SAMPLED),
			};
			pass.shared_set.init(ctx, bindings, 7, EDescriptorSetAllocation::Transient);
		}
		{
			const DescriptorBindingSpec bindings[] = {
				compute_binding(0, STORAGE), compute_binding(1, STORAGE), compute_binding(2, STORAGE), compute_binding(3, STORAGE),
			};
			init_compute(ctx, pass.clear_pipeline, "bin/shaders/sdf_clear.comp.spv", bindings, 4, sizeof(LevelPush), false);
		}
		{
			const DescriptorBindingSpec bindings[] = { compute_binding(0, STORAGE), compute_binding(1, STORAGE) };
			init_compute(ctx, pass.jfa_seed_pipeline, "bin/shaders/sdf_jfa_seed.comp.spv", bindings, 2, sizeof(LevelPush), false);
			init_compute(ctx, pass.jfa_step_pipeline, "bin/shaders/sdf_jfa_step.comp.spv", bindings, 2, sizeof(JfaStepPush), false);
			init_compute(ctx, pass.jfa_resolve_pipeline, "bin/shaders/sdf_jfa_resolve.comp.spv", bindings, 2, sizeof(JfaResolvePush), false);
		}
		{
			// Set 0 is Bruneton's (made compute-visible in bruneton_atmosphere_pass.h).
			const DescriptorBindingSpec bindings[] = { compute_binding(0, STORAGE), compute_binding(1, SSBO) };
			pass.sky_pipeline.descriptors.init(ctx, bindings, 2, EDescriptorSetAllocation::Transient);
			const VkDescriptorSetLayout layouts[2] = {
				bruneton_atmosphere_pass.descriptors.layout, pass.sky_pipeline.descriptors.layout,
			};
			pass.sky_pipeline.pipeline_layout.init(ctx, layouts, 2, sizeof(SkyPush), VK_SHADER_STAGE_COMPUTE_BIT);
			VkShaderModule module = create_shader_module_from_file(ctx->device, "bin/shaders/sdf_sky.comp.spv");
			const VkComputePipelineCreateInfo pipeline_info = {
				.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
				.stage = {
					.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
					.stage = VK_SHADER_STAGE_COMPUTE_BIT,
					.module = module,
					.pName = "main",
				},
				.layout = pass.sky_pipeline.pipeline_layout.layout,
			};
			VK_CHECK(vulkan_create_compute_pipelines(ctx, 1, &pipeline_info, &pass.sky_pipeline.pipeline));
			vkDestroyShaderModule(ctx->device, module, nullptr);
		}
		{
			const DescriptorBindingSpec bindings[] = {
				compute_binding(0, UBO), compute_binding(1, SAMPLED), compute_binding(2, SAMPLED),
				compute_binding(3, SAMPLED), compute_binding(4, STORAGE), compute_binding(5, STORAGE),
				compute_binding(6, SAMPLED), compute_binding(7, SSBO), compute_binding(8, SSBO), compute_binding(9, SSBO),
			};
			init_compute(ctx, pass.voxel_lighting_pipeline, "bin/shaders/sdf_voxel_lighting.comp.spv", bindings, 10, sizeof(LevelPush), false);
		}
		{
			const DescriptorBindingSpec bindings[] = {
				compute_binding(0, SAMPLED), compute_binding(1, SAMPLED), compute_binding(2, STORAGE), compute_binding(3, STORAGE),
			};
			init_compute(ctx, pass.probe_place_pipeline, "bin/shaders/sdf_probe_place.comp.spv", bindings, 4, 0, true);
		}
		{
			const DescriptorBindingSpec bindings[] = {
				compute_binding(0, STORAGE), compute_binding(1, STORAGE), compute_binding(2, STORAGE),
				compute_binding(3, SAMPLED), compute_binding(4, SAMPLED), compute_binding(5, SAMPLED),
			};
			init_compute(ctx, pass.probe_trace_pipeline, "bin/shaders/sdf_probe_trace.comp.spv", bindings, 6, sizeof(ScreenTracePush), true);
		}
		{
			const DescriptorBindingSpec bindings[] = {
				compute_binding(0, STORAGE), compute_binding(1, STORAGE), compute_binding(2, STORAGE), compute_binding(3, STORAGE),
				compute_binding(4, STORAGE), compute_binding(5, STORAGE), compute_binding(6, STORAGE),
			};
			init_compute(ctx, pass.probe_temporal_pipeline, "bin/shaders/sdf_probe_temporal.comp.spv", bindings, 7, 0, true);
		}
		{
			const DescriptorBindingSpec bindings[] = {
				compute_binding(0, STORAGE), compute_binding(1, STORAGE), compute_binding(2, STORAGE), compute_binding(3, STORAGE),
			};
			init_compute(ctx, pass.probe_filter_pipeline, "bin/shaders/sdf_probe_filter.comp.spv", bindings, 4, 0, true);
		}
		{
			const DescriptorBindingSpec bindings[] = { compute_binding(0, STORAGE), compute_binding(1, STORAGE), compute_binding(2, SSBO) };
			init_compute(ctx, pass.probe_sh_pipeline, "bin/shaders/sdf_probe_sh.comp.spv", bindings, 3, 0, true);
		}
		{
			const DescriptorBindingSpec bindings[] = {
				compute_binding(0, SAMPLED), compute_binding(1, SAMPLED), compute_binding(2, STORAGE), compute_binding(3, STORAGE),
				compute_binding(4, SSBO), compute_binding(5, STORAGE), compute_binding(6, STORAGE), compute_binding(7, STORAGE),
				compute_binding(8, SAMPLED), compute_binding(9, STORAGE),
			};
			init_compute(ctx, pass.resolve_pipeline, "bin/shaders/sdf_gi_resolve.comp.spv", bindings, 10, sizeof(ScreenTracePush), true);
		}
		{
			const DescriptorBindingSpec bindings[] = {
				compute_binding(0, SAMPLED), compute_binding(1, SAMPLED), compute_binding(2, SAMPLED),
				compute_binding(3, SAMPLED), compute_binding(4, STORAGE),
			};
			init_compute(ctx, pass.reflection_trace_pipeline, "bin/shaders/sdf_reflection_trace.comp.spv", bindings, 5, sizeof(ScreenTracePush), true);
		}
		{
			const DescriptorBindingSpec bindings[] = {
				compute_binding(0, SAMPLED), compute_binding(1, SAMPLED), compute_binding(2, SAMPLED), compute_binding(3, STORAGE),
				compute_binding(4, SAMPLED), compute_binding(5, STORAGE), compute_binding(6, STORAGE), compute_binding(7, STORAGE),
			};
			init_compute(ctx, pass.reflection_resolve_pipeline, "bin/shaders/sdf_reflection_resolve.comp.spv", bindings, 8, sizeof(ScreenTracePush), true);
		}
		create_voxelize_pipeline(ctx);
		pass.initialized = true;
	}

	inline void destroy_screen_resources(VulkanContext* ctx)
	{
		if (pass.screen_extent.width == 0) return;
		for (i32 i = 0; i < 2; ++i)
		{
			vulkan_context_retire_image(ctx, pass.probe_position[i]);
			vulkan_context_retire_image(ctx, pass.probe_normal[i]);
			vulkan_context_retire_image(ctx, pass.history[i]);
			vulkan_context_retire_image(ctx, pass.reflection_history[i]);
			vulkan_context_retire_image(ctx, pass.diffuse_history[i]);
		}
		vulkan_context_retire_image(ctx, pass.trace);
		vulkan_context_retire_image(ctx, pass.filtered);
		vulkan_context_retire_image(ctx, pass.filtered_scratch);
		vulkan_context_retire_image(ctx, pass.irradiance);
		vulkan_context_retire_image(ctx, pass.specular);
		vulkan_context_retire_image(ctx, pass.reflection_trace);
		pass.probe_sh.destroy_gpu_buffer();
		pass.screen_extent = {};
	}

	inline void ensure_screen_resources(VulkanContext* ctx, VkExtent2D extent)
	{
		if (pass.screen_extent.width == extent.width && pass.screen_extent.height == extent.height) return;
		destroy_screen_resources(ctx);
		pass.screen_extent = extent;
		pass.probe_grid_width = (i32) ((extent.width + PROBE_TILE - 1) / PROBE_TILE);
		pass.probe_grid_height = (i32) ((extent.height + PROBE_TILE - 1) / PROBE_TILE);
		const u32 grid_w = (u32) pass.probe_grid_width;
		const u32 grid_h = (u32) pass.probe_grid_height;
		for (i32 i = 0; i < 2; ++i)
		{
			pass.probe_position[i] = make_image(ctx, grid_w, grid_h, 1, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "SDF GI Probe Position");
			pass.probe_normal[i] = make_image(ctx, grid_w, grid_h, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "SDF GI Probe Normal");
			pass.history[i] = make_image(ctx, grid_w * PROBE_RES, grid_h * PROBE_RES, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "SDF GI Probe History");
			pass.reflection_history[i] = make_image(ctx, extent.width, extent.height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, STORAGE_SAMPLED, "SDF GI Reflection History");
			pass.diffuse_history[i] = make_image(ctx, extent.width, extent.height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, STORAGE_SAMPLED, "SDF GI Diffuse History");
		}
		pass.trace = make_image(ctx, grid_w * PROBE_RES, grid_h * PROBE_RES, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "SDF GI Probe Trace");
		pass.filtered = make_image(ctx, grid_w * PROBE_RES, grid_h * PROBE_RES, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "SDF GI Probe Filtered");
		pass.filtered_scratch = make_image(ctx, grid_w * PROBE_RES, grid_h * PROBE_RES, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "SDF GI Probe Filtered Scratch");
		pass.irradiance = make_image(ctx, extent.width, extent.height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, STORAGE_SAMPLED, "SDF GI Irradiance");
		pass.specular = make_image(ctx, extent.width, extent.height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, STORAGE_SAMPLED, "SDF GI Specular");
		pass.reflection_trace = make_image(ctx, (extent.width + 1) / 2, (extent.height + 1) / 2, 1, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "SDF GI Reflection Trace");
		pass.probe_sh = GpuBuffer((GpuBufferDesc<HMM_Vec4>){
			.data = nullptr,
			.size = sizeof(HMM_Vec4) * 9 * grid_w * grid_h,
			.usage = { .storage_buffer = true },
			.label = "SDF GI Probe SH9",
		});
		pass.probe_sh.get_gpu_buffer();
		pass.history_valid = false;
		pass.reflection_history_valid = false;
		pass.diffuse_history_valid = false;
	}

	inline void shutdown(VulkanContext* ctx)
	{
		if (!pass.initialized) return;
		destroy_screen_resources(ctx);
		for (GpuImage* image : { &pass.distance, &pass.albedo, &pass.normal_emission, &pass.radiance_front,
			&pass.radiance_back, &pass.jfa[0], &pass.jfa[1], &pass.voxelize_target, &pass.sky })
		{
			gpu_image_destroy(ctx->allocator, ctx->device, *image);
		}
		pass.sun_buffer.destroy_gpu_buffer();
		for (GpuBuffer<HMM_Vec4>& buffer : pass.sun_fallback) buffer.destroy_gpu_buffer();
		for (ComputeEffect* effect : { &pass.clear_pipeline, &pass.jfa_seed_pipeline, &pass.jfa_step_pipeline,
			&pass.jfa_resolve_pipeline, &pass.sky_pipeline, &pass.voxel_lighting_pipeline, &pass.probe_place_pipeline,
			&pass.probe_trace_pipeline, &pass.probe_temporal_pipeline, &pass.probe_filter_pipeline,
			&pass.probe_sh_pipeline, &pass.resolve_pipeline, &pass.reflection_trace_pipeline, &pass.reflection_resolve_pipeline })
		{
			effect->shutdown(ctx);
		}
		pass.shared_set.shutdown(ctx);
		pass.voxelize_set.shutdown(ctx);
		vkDestroyPipeline(ctx->device, pass.voxelize_pipeline, nullptr);
		vkDestroyPipelineLayout(ctx->device, pass.voxelize_pipeline_layout, nullptr);
		vkDestroySampler(ctx->device, pass.linear_sampler, nullptr);
		vkDestroySampler(ctx->device, pass.nearest_sampler, nullptr);
		pass.params.shutdown();
		pass.initialized = false;
	}

	inline f32 level_voxel_size(const State& in_state, i32 in_level)
	{
		return std::max(in_state.sdf_gi.base_voxel_size, 0.01f) * (f32) (1 << in_level);
	}

	inline i32 ready_level_mask()
	{
		i32 mask = 0;
		for (i32 level = 0; level < LEVEL_COUNT; ++level)
		{
			if (pass.levels[level].built) mask |= 1 << level;
		}
		return mask;
	}

	inline void dispatch_level_volume(VulkanContext* ctx, ComputeEffect& effect, VkDescriptorSet set, const void* push)
	{
		effect.bind_and_dispatch(ctx, set, LEVEL_RES / 4, LEVEL_RES / 4, LEVEL_RES / 4, push);
	}

	// Clear -> voxelize (3 axes) -> jump flood -> distance, for one level.
	inline void rebuild_level(FrameRenderGraph& graph, VulkanContext* ctx, State& in_state, i32 in_level,
		HMM_Vec3 in_origin, f32 in_voxel_size)
	{
		vulkan_begin_debug_label(ctx, "SDF GI Level Rebuild");
		const LevelPush level_push = { { in_level, 0, 0, 0 } };

		DescriptorWriter clear_writer = pass.clear_pipeline.writer(ctx);
		clear_writer.storage_image(0, pass.albedo.view).storage_image(1, pass.normal_emission.view)
			.storage_image(2, pass.radiance_front.view).storage_image(3, pass.radiance_back.view).commit();
		graph.storage_write(frame_graph_image(pass.albedo));
		graph.storage_write(frame_graph_image(pass.normal_emission));
		graph.storage_write(frame_graph_image(pass.radiance_front));
		graph.storage_write(frame_graph_image(pass.radiance_back));
		graph.compute([&]() { dispatch_level_volume(ctx, pass.clear_pipeline, clear_writer.set, &level_push); });

		// Voxelize: the same box seen along X, Y and Z.
		const f32 extent = in_voxel_size * (f32) LEVEL_RES;
		const f32 half = extent * 0.5f;
		const HMM_Vec3 center = in_origin + HMM_V3(half, half, half);
		const HMM_Vec3 axes[3] = { HMM_V3(1, 0, 0), HMM_V3(0, 1, 0), HMM_V3(0, 0, 1) };
		const HMM_Vec3 ups[3] = { HMM_V3(0, 0, 1), HMM_V3(0, 0, 1), HMM_V3(0, 1, 0) };
		HMM_Mat4 view_projections[3];
		for (i32 axis = 0; axis < 3; ++axis)
		{
			const HMM_Mat4 view = HMM_LookAt_RH(center - axes[axis] * (half + 1.0f), center, ups[axis]);
			view_projections[axis] = HMM_MulM4(
				ShadowDepthPass::mat4_orthographic(-half, half, -half, half, 0.5f, extent + 1.5f), view);
		}
		cull_objects(in_state, view_projections[2], 0.0f, pass.visible_scratch);

		DescriptorWriter voxel_writer = pass.voxelize_set.writer(ctx);
		voxel_writer.storage_image(0, pass.albedo.view).storage_image(1, pass.normal_emission.view).commit();
		graph.storage_write(frame_graph_image(pass.albedo), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
		graph.storage_write(frame_graph_image(pass.normal_emission), VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
		const FrameGraphColorAttachment attachment = {
			.image = frame_graph_image(pass.voxelize_target),
			.load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		};
		graph.render(&attachment, 1, [&]()
		{
			VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
			const VkDescriptorSet sets[2] = { frame_data.per_frame_sets[ctx->frame_index], voxel_writer.set };
			vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
				pass.voxelize_pipeline_layout, 0, 2, sets, 0, nullptr);
			for (i32 axis = 0; axis < 3; ++axis)
			{
				const VoxelizePush push = {
					.view_projection = view_projections[axis],
					.level_origin_voxel = HMM_V4V(in_origin, in_voxel_size),
					.level = { in_level, 0, 0, 0 },
				};
				vulkan_cmd_push_constants(ctx, pass.voxelize_pipeline_layout,
					VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
				MeshDrawBindings bindings;
				for (const i32 render_object_index : pass.visible_scratch)
				{
					auto found = in_state.scene.objects.find(in_state.cull_entries[render_object_index].object_id);
					if (found == in_state.scene.objects.end() || !object_contributes_to_gi_scene(found->second)
						|| found->second.mesh.has_skinned_vertices)
					{
						continue;
					}
					// shortcut: skinned meshes stay out of the SDF (static geometry only, by design).
					draw_indexed_mesh(ctx, found->second.mesh, render_object_index,
						pass.voxelize_pipeline, pass.voxelize_pipeline, bindings);
				}
			}
		});

		// Jump flood
		DescriptorWriter seed_writer = pass.jfa_seed_pipeline.writer(ctx);
		seed_writer.storage_image(0, pass.albedo.view).storage_image(1, pass.jfa[0].view).commit();
		graph.storage_read(frame_graph_image(pass.albedo));
		graph.storage_write(frame_graph_image(pass.jfa[0]), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);
		graph.compute([&]() { dispatch_level_volume(ctx, pass.jfa_seed_pipeline, seed_writer.set, &level_push); });
		i32 source = 0;
		for (i32 step_index = 0; step_index < JFA_PASSES; ++step_index)
		{
			const i32 step = std::max(LEVEL_RES / (2 << step_index), 1);
			const JfaStepPush step_push = { { step, 0, 0, 0 } };
			DescriptorWriter step_writer = pass.jfa_step_pipeline.writer(ctx);
			step_writer.storage_image(0, pass.jfa[source].view).storage_image(1, pass.jfa[1 - source].view).commit();
			graph.storage_read(frame_graph_image(pass.jfa[source]));
			graph.storage_write(frame_graph_image(pass.jfa[1 - source]), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);
			graph.compute([&]() { dispatch_level_volume(ctx, pass.jfa_step_pipeline, step_writer.set, &step_push); });
			source = 1 - source;
		}
		const JfaResolvePush resolve_push = { { in_level, 0, 0, 0 }, HMM_V4(in_voxel_size, 0, 0, 0) };
		DescriptorWriter resolve_writer = pass.jfa_resolve_pipeline.writer(ctx);
		resolve_writer.storage_image(0, pass.jfa[source].view).storage_image(1, pass.distance.view).commit();
		graph.storage_read(frame_graph_image(pass.jfa[source]));
		graph.storage_write(frame_graph_image(pass.distance));
		graph.compute([&]() { dispatch_level_volume(ctx, pass.jfa_resolve_pipeline, resolve_writer.set, &resolve_push); });
		vulkan_end_debug_label(ctx);
	}

	inline void record_sky(FrameRenderGraph& graph, VulkanContext* ctx, State& in_state, HMM_Vec3 in_camera,
		HMM_Vec3 in_sun_to_light, HMM_Vec3 in_sun_irradiance, bool in_has_sun)
	{
		const bool atmosphere = sky_pass.has_active_atmosphere && bruneton_atmosphere_pass.has_precomputed;
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		if (!atmosphere)
		{
			// No physical sky: black sky, sun without atmospheric transmittance.
			const HMM_Vec4 sun = HMM_V4V(in_has_sun ? in_sun_irradiance : HMM_V3(0, 0, 0), 1.0f);
			GpuBuffer<HMM_Vec4>& fallback = pass.sun_fallback[ctx->frame_index];
			fallback.update_gpu_buffer(&sun, sizeof(sun));
			pass.active_sun_buffer = fallback.get_gpu_buffer();
			graph.transfer_destination(frame_graph_image(pass.sky), true);
			graph.transfer([&]()
			{
				const VkClearColorValue black = {};
				const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				vkCmdClearColorImage(command_buffer, pass.sky.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
			});
			return;
		}
		pass.active_sun_buffer = pass.sun_buffer.get_gpu_buffer();
		DescriptorWriter writer = pass.sky_pipeline.writer(ctx);
		writer.storage_image(0, pass.sky.view)
			.buffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, pass.sun_buffer.get_gpu_buffer()).commit();
		const SkyPush push = {
			.camera_position = HMM_V4V(in_camera, 1.0f),
			.sun_direction = HMM_V4V(in_sun_to_light, in_has_sun ? 1.0f : 0.0f),
			.sky_color_and_intensity = HMM_V4V(sky_pass.active_sun_color,
				in_state.gi.render_sky_to_probes ? sky_pass.active_parameters.sky_intensity : 0.0f),
			.sun_irradiance_and_atmosphere = HMM_V4V(in_sun_irradiance, sky_pass.active_parameters.planet_center_z_m),
			.flags = { 1, 0, 0, 0 },
		};
		bruneton_atmosphere_pass.declare_sampled_resources(graph, ctx, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
		graph.storage_write(frame_graph_image(pass.sky), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);
		graph.storage_write(frame_graph_buffer(pass.sun_buffer.get_gpu_buffer()));
		graph.compute([&]()
		{
			vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pass.sky_pipeline.pipeline);
			const VkDescriptorSet sets[2] = { bruneton_atmosphere_pass.descriptors.current(ctx), writer.set };
			vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
				pass.sky_pipeline.pipeline_layout.layout, 0, 2, sets, 0, nullptr);
			pass.sky_pipeline.dispatch(ctx, SKY_RES / 8, SKY_RES / 8, 1, &push);
		});
	}

	inline GeometryContentHash compute_lighting_signature(const State& in_state, HMM_Vec3 in_sun_irradiance, HMM_Vec3 in_sun_dir)
	{
		GeometryContentHash hash = geometry_arena_hash_bytes(&in_sun_irradiance, sizeof(in_sun_irradiance), 0x5344u);
		hash = geometry_arena_hash_bytes(&in_sun_dir, sizeof(in_sun_dir), hash.low);
		if (in_state.lighting.point_lights.length() > 0)
		{
			hash = geometry_arena_hash_bytes(in_state.lighting.point_lights.data(),
				sizeof(PointLightData) * in_state.lighting.point_lights.length(), hash.low);
		}
		if (in_state.lighting.spot_lights.length() > 0)
		{
			hash = geometry_arena_hash_bytes(in_state.lighting.spot_lights.data(),
				sizeof(SpotLightData) * in_state.lighting.spot_lights.length(), hash.low);
		}
		const HMM_Vec4 sky = HMM_V4V(sky_pass.active_sun_color, sky_pass.active_parameters.sky_intensity);
		return geometry_arena_hash_bytes(&sky, sizeof(sky), hash.low);
	}

	// Per frame, before the geometry pass: rebuild at most one level, refresh the
	// sky, relight one level, and upload the shared parameters.
	inline void update_volumes(FrameRenderGraph& graph, VulkanContext* ctx, State& in_state,
		const Camera& in_camera, const HMM_Mat4& in_view_projection, VkExtent2D in_render_extent)
	{
		CPU_TIMING_SCOPE("SDF GI Volumes");
		const i32 timing_slot = gpu_timestamps_begin_scope(ctx, "SDF GI Volumes");
		vulkan_begin_debug_label(ctx, "SDF GI Volumes");
		ensure_screen_resources(ctx, in_render_extent);
		State::SdfGiState& settings = in_state.sdf_gi;

		// Invalidate: reset / voxel size change / edited objects.
		for (i32 level = 0; level < LEVEL_COUNT; ++level)
		{
			Level& state_level = pass.levels[level];
			if (settings.rebuild_requested || state_level.built_voxel_size != level_voxel_size(in_state, level))
			{
				state_level.dirty = true;
			}
			for (const BoundingBox& region : settings.dirty_regions)
			{
				if (state_level.built && !state_level.dirty
					&& level_overlaps(state_level.built_origin, state_level.built_voxel_size, region))
				{
					state_level.dirty = true;
					printf("SDF GI: object edit, rebuilding level %d\n", level);
				}
			}
		}
		settings.rebuild_requested = false;
		settings.dirty_regions.reset();

		// Rebuild one level: round-robin over the levels that need it.
		i32 rebuild_level_index = -1;
		for (i32 offset = 0; offset < LEVEL_COUNT; ++offset)
		{
			const i32 level = (pass.rebuild_cursor + offset) % LEVEL_COUNT;
			const f32 voxel = level_voxel_size(in_state, level);
			const HMM_Vec3 target = level_target_origin(in_camera.location, voxel);
			const Level& state_level = pass.levels[level];
			const bool moved = !state_level.built
				|| HMM_LenSqrV3(state_level.built_origin - target) > 1e-6f;
			if (state_level.dirty || moved)
			{
				rebuild_level_index = level;
				break;
			}
		}
		if (rebuild_level_index >= 0)
		{
			Level& state_level = pass.levels[rebuild_level_index];
			state_level.built_voxel_size = level_voxel_size(in_state, rebuild_level_index);
			state_level.built_origin = level_target_origin(in_camera.location, state_level.built_voxel_size);
			state_level.built = true;
			state_level.dirty = false;
			pass.rebuild_cursor = (rebuild_level_index + 1) % LEVEL_COUNT;
			settings.level_rebuild_count += 1;
			settings.frames_since_rebuild = 0;
		}
		else
		{
			settings.frames_since_rebuild += 1;
		}
		settings.levels_ready = 0;
		for (const Level& state_level : pass.levels) settings.levels_ready += state_level.built ? 1 : 0;

		// Sun: the atmosphere's sun when there is one, else the first sun light.
		HMM_Vec3 sun_to_light = HMM_V3(0, 0, 1);
		HMM_Vec3 sun_irradiance = HMM_V3(0, 0, 0);
		bool has_sun = false;
		if (in_state.lighting.sun_lights.length() > 0)
		{
			i32 sun_index = in_state.lighting.active_atmosphere_sun_index;
			if (sun_index < 0 || sun_index >= (i32) in_state.lighting.sun_lights.length()) sun_index = 0;
			const SunLightData& sun = in_state.lighting.sun_lights[sun_index];
			sun_to_light = -HMM_NormV3(sun.direction.XYZ);
			sun_irradiance = scene_solar_irradiance(sun.color.XYZ, sun.power);
			has_sun = true;
		}

		// Lighting change: respond faster for a while.
		const GeometryContentHash signature = compute_lighting_signature(in_state, sun_irradiance, sun_to_light);
		if (signature.low != pass.lighting_signature.low || signature.high != pass.lighting_signature.high)
		{
			pass.lighting_signature = signature;
			pass.fast_response_frames = FAST_RESPONSE_FRAMES;
		}
		const bool fast = pass.fast_response_frames > 0;
		pass.fast_response_frames = std::max(pass.fast_response_frames - 1, 0);

		GpuParams params = {};
		for (i32 level = 0; level < LEVEL_COUNT; ++level)
		{
			params.level_origin_voxel[level] = HMM_V4V(pass.levels[level].built_origin, pass.levels[level].built_voxel_size);
		}
		params.view_projection = in_view_projection;
		params.prev_view_projection = pass.prev_view_projection;
		params.inv_view_projection = HMM_InvGeneralM4(in_view_projection);
		params.camera_position = HMM_V4V(in_camera.location, 1.0f);
		params.sun_direction = HMM_V4V(sun_to_light, has_sun ? 1.0f : 0.0f);
		params.counts[0] = (i32) in_state.lighting.point_lights.length();
		params.counts[1] = (i32) in_state.lighting.spot_lights.length();
		params.counts[2] = (i32) (pass.frame_counter & 0x7FFFFFFF);
		params.counts[3] = ready_level_mask();
		params.screen[0] = (i32) pass.screen_extent.width;
		params.screen[1] = (i32) pass.screen_extent.height;
		params.screen[2] = pass.probe_grid_width;
		params.screen[3] = pass.probe_grid_height;
		params.settings = HMM_V4(fast ? 0.5f : 0.9f, fast ? 0.5f : 0.9f,
			settings.sdf_shadows ? 1.0f : 0.0f, settings.voxel_bounce ? 1.0f : 0.0f);
		params.debug[0] = (i32) settings.debug_view;
		params.debug[1] = settings.temporal ? 1 : 0;
		params.debug[2] = settings.spatial_filter ? 1 : 0;
		params.debug[3] = pass.history_valid ? 1 : 0;
		const VkBuffer params_buffer = pass.params.update(ctx, params);
		pass.params_buffer = params_buffer;

		if (rebuild_level_index >= 0)
		{
			const Level& state_level = pass.levels[rebuild_level_index];
			rebuild_level(graph, ctx, in_state, rebuild_level_index, state_level.built_origin, state_level.built_voxel_size);
		}

		record_sky(graph, ctx, in_state, in_camera.location, sun_to_light, sun_irradiance, has_sun);

		// Relight one built level per frame (round-robin).
		i32 light_level = -1;
		for (i32 offset = 0; offset < LEVEL_COUNT; ++offset)
		{
			const i32 level = (pass.lighting_cursor + offset) % LEVEL_COUNT;
			if (pass.levels[level].built)
			{
				light_level = level;
				break;
			}
		}
		if (light_level >= 0)
		{
			pass.lighting_cursor = (light_level + 1) % LEVEL_COUNT;
			const LevelPush push = { { light_level, 0, 0, 0 } };
			const u32 buffer_index = in_state.lighting.buffer_index;
			const VkBuffer point_buffer = in_state.lighting.point_buffers[buffer_index].get_gpu_buffer();
			const VkBuffer spot_buffer = in_state.lighting.spot_buffers[buffer_index].get_gpu_buffer();
			DescriptorWriter writer = pass.voxel_lighting_pipeline.writer(ctx);
			writer.buffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, params_buffer, sizeof(GpuParams))
				.sampled(1, pass.linear_sampler, pass.distance.view)
				.sampled(2, pass.nearest_sampler, pass.albedo.view)
				.sampled(3, pass.nearest_sampler, pass.normal_emission.view)
				.storage_image(4, pass.radiance_front.view)
				.storage_image(5, pass.radiance_back.view)
				.sampled(6, pass.linear_sampler, pass.sky.view)
				.buffer(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, point_buffer)
				.buffer(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, spot_buffer)
				.buffer(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, pass.active_sun_buffer)
				.commit();
			graph.uniform(frame_graph_buffer(params_buffer), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
			graph.sampled(frame_graph_image(pass.distance), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
			graph.sampled(frame_graph_image(pass.albedo), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
			graph.sampled(frame_graph_image(pass.normal_emission), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
			graph.sampled(frame_graph_image(pass.sky), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
			graph.storage_read_write(frame_graph_image(pass.radiance_front));
			graph.storage_read_write(frame_graph_image(pass.radiance_back));
			graph.storage_read(frame_graph_buffer(point_buffer));
			graph.storage_read(frame_graph_buffer(spot_buffer));
			graph.storage_read(frame_graph_buffer(pass.active_sun_buffer));
			graph.compute([&]() { dispatch_level_volume(ctx, pass.voxel_lighting_pipeline, writer.set, &push); });
		}

		vulkan_end_debug_label(ctx);
		gpu_timestamps_end_scope(ctx, timing_slot);
	}

	inline VkDescriptorSet write_shared_set(VulkanContext* ctx)
	{
		DescriptorWriter writer = pass.shared_set.writer(ctx);
		writer.buffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, pass.params_buffer, sizeof(GpuParams))
			.sampled(1, pass.linear_sampler, pass.distance.view)
			.sampled(2, pass.nearest_sampler, pass.albedo.view)
			.sampled(3, pass.nearest_sampler, pass.normal_emission.view)
			.sampled(4, pass.nearest_sampler, pass.radiance_front.view)
			.sampled(5, pass.nearest_sampler, pass.radiance_back.view)
			.sampled(6, pass.linear_sampler, pass.sky.view)
			.commit();
		return writer.set;
	}

	inline void declare_shared_set(FrameRenderGraph& graph)
	{
		graph.uniform(frame_graph_buffer(pass.params_buffer), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
		for (GpuImage* image : { &pass.distance, &pass.albedo, &pass.normal_emission,
			&pass.radiance_front, &pass.radiance_back, &pass.sky })
		{
			graph.sampled(frame_graph_image(*image), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
		}
	}

	inline void dispatch_shared(VulkanContext* ctx, ComputeEffect& effect, VkDescriptorSet set,
		VkDescriptorSet shared, u32 x, u32 y, const void* push = nullptr)
	{
		effect.bind(ctx, set);
		effect.bind_global_set(ctx, shared);
		effect.dispatch(ctx, x, y, 1, push);
	}

	// After the G-buffer (and SSAO), before lighting: screen-probe final gather.
	inline void gather(FrameRenderGraph& graph, VulkanContext* ctx, State& in_state,
		FrameGraphImage in_gbuffer_position, FrameGraphImage in_gbuffer_normal, FrameGraphImage in_gbuffer_rme,
		FrameGraphImage in_previous_color, bool in_previous_color_valid, const HMM_Mat4& in_view_projection)
	{
		CPU_TIMING_SCOPE("SDF GI Gather");
		const i32 timing_slot = gpu_timestamps_begin_scope(ctx, "SDF GI Gather");
		vulkan_begin_debug_label(ctx, "SDF GI Gather");
		const i32 current = (i32) (pass.frame_counter & 1);
		const i32 previous = 1 - current;
		const u32 grid_w = (u32) pass.probe_grid_width;
		const u32 grid_h = (u32) pass.probe_grid_height;
		const VkDescriptorSet shared = write_shared_set(ctx);
		declare_shared_set(graph);

		// 1. Place
		DescriptorWriter place_writer = pass.probe_place_pipeline.writer(ctx);
		place_writer.sampled(0, pass.nearest_sampler, in_gbuffer_position.view())
			.sampled(1, pass.nearest_sampler, in_gbuffer_normal.view())
			.storage_image(2, pass.probe_position[current].view)
			.storage_image(3, pass.probe_normal[current].view).commit();
		graph.sampled(in_gbuffer_position, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
		graph.sampled(in_gbuffer_normal, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
		graph.storage_write(frame_graph_image(pass.probe_position[current]), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);
		graph.storage_write(frame_graph_image(pass.probe_normal[current]), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);
		graph.compute([&]() { dispatch_shared(ctx, pass.probe_place_pipeline, place_writer.set, shared, (grid_w + 7) / 8, (grid_h + 7) / 8); });

		// 2. Trace (screen first, then SDF)
		const ScreenTracePush probe_trace_push = { { 0, 0, in_state.sdf_gi.probe_screen_trace && in_previous_color_valid ? 1 : 0, 0 } };
		DescriptorWriter trace_writer = pass.probe_trace_pipeline.writer(ctx);
		trace_writer.storage_image(0, pass.probe_position[current].view)
			.storage_image(1, pass.probe_normal[current].view)
			.storage_image(2, pass.trace.view)
			.sampled(3, pass.nearest_sampler, in_gbuffer_position.view())
			.sampled(4, pass.nearest_sampler, in_gbuffer_normal.view())
			.sampled(5, pass.linear_sampler, in_previous_color.view()).commit();
		graph.storage_read(frame_graph_image(pass.probe_position[current]));
		graph.storage_read(frame_graph_image(pass.probe_normal[current]));
		graph.sampled(in_previous_color, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
		graph.storage_write(frame_graph_image(pass.trace), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);
		graph.compute([&]()
		{
			dispatch_shared(ctx, pass.probe_trace_pipeline, trace_writer.set, shared, grid_w, grid_h, &probe_trace_push);
		});

		// 3. Temporal (reads last frame's probes + history; a cleared history is never read: history_valid)
		DescriptorWriter temporal_writer = pass.probe_temporal_pipeline.writer(ctx);
		temporal_writer.storage_image(0, pass.probe_position[current].view)
			.storage_image(1, pass.probe_normal[current].view)
			.storage_image(2, pass.trace.view)
			.storage_image(3, pass.history[previous].view)
			.storage_image(4, pass.probe_position[previous].view)
			.storage_image(5, pass.probe_normal[previous].view)
			.storage_image(6, pass.history[current].view).commit();
		graph.storage_read(frame_graph_image(pass.trace));
		graph.storage_read(frame_graph_image(pass.history[previous]));
		graph.storage_read(frame_graph_image(pass.probe_position[previous]));
		graph.storage_read(frame_graph_image(pass.probe_normal[previous]));
		graph.storage_write(frame_graph_image(pass.history[current]), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);
		const u32 texel_groups_x = (grid_w * PROBE_RES + 7) / 8;
		const u32 texel_groups_y = (grid_h * PROBE_RES + 7) / 8;
		graph.compute([&]() { dispatch_shared(ctx, pass.probe_temporal_pipeline, temporal_writer.set, shared, texel_groups_x, texel_groups_y); });

		// 4. Spatial filter: history -> filtered -> scratch -> filtered (one pass when off: a copy)
		const i32 filter_passes = in_state.sdf_gi.spatial_filter ? PROBE_FILTER_PASSES : 1;
		for (i32 filter_pass = 0; filter_pass < filter_passes; ++filter_pass)
		{
			GpuImage& input = filter_pass == 0 ? pass.history[current] : (filter_pass % 2 == 1 ? pass.filtered : pass.filtered_scratch);
			GpuImage& output = filter_pass % 2 == 0 ? pass.filtered : pass.filtered_scratch;
			DescriptorWriter filter_writer = pass.probe_filter_pipeline.writer(ctx);
			filter_writer.storage_image(0, pass.probe_position[current].view)
				.storage_image(1, pass.probe_normal[current].view)
				.storage_image(2, input.view)
				.storage_image(3, output.view).commit();
			graph.storage_read(frame_graph_image(input));
			graph.storage_write(frame_graph_image(output), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);
			graph.compute([&]() { dispatch_shared(ctx, pass.probe_filter_pipeline, filter_writer.set, shared, texel_groups_x, texel_groups_y); });
		}

		// 5. SH9
		const VkBuffer sh_buffer = pass.probe_sh.get_gpu_buffer();
		DescriptorWriter sh_writer = pass.probe_sh_pipeline.writer(ctx);
		sh_writer.storage_image(0, pass.probe_position[current].view)
			.storage_image(1, pass.filtered.view)
			.buffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sh_buffer).commit();
		graph.storage_read(frame_graph_image(pass.filtered));
		graph.storage_write(frame_graph_buffer(sh_buffer));
		graph.compute([&]() { dispatch_shared(ctx, pass.probe_sh_pipeline, sh_writer.set, shared, grid_w, grid_h); });

		// 6. Resolve (+ per-pixel irradiance temporal; a never-written history is only bound, not read)
		const bool pixel_temporal = in_state.sdf_gi.pixel_temporal;
		const ScreenTracePush gi_resolve_push = { { pass.diffuse_history_valid ? 1 : 0, pixel_temporal ? 1 : 0, 0, 0 } };
		DescriptorWriter resolve_writer = pass.resolve_pipeline.writer(ctx);
		resolve_writer.sampled(0, pass.nearest_sampler, in_gbuffer_position.view())
			.sampled(1, pass.nearest_sampler, in_gbuffer_normal.view())
			.storage_image(2, pass.probe_position[current].view)
			.storage_image(3, pass.probe_normal[current].view)
			.buffer(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sh_buffer)
			.storage_image(5, pass.irradiance.view)
			.storage_image(6, pass.specular.view)
			.storage_image(7, pass.filtered.view)
			.sampled(8, pass.linear_sampler, pass.diffuse_history[previous].view)
			.storage_image(9, pass.diffuse_history[current].view).commit();
		graph.storage_read(frame_graph_buffer(sh_buffer));
		graph.sampled(frame_graph_image(pass.diffuse_history[previous]), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
		graph.storage_write(frame_graph_image(pass.diffuse_history[current]), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);
		graph.storage_write(frame_graph_image(pass.irradiance), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);
		graph.storage_write(frame_graph_image(pass.specular), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);
		graph.compute([&]()
		{
			dispatch_shared(ctx, pass.resolve_pipeline, resolve_writer.set, shared,
				(pass.screen_extent.width + 7) / 8, (pass.screen_extent.height + 7) / 8, &gi_resolve_push);
		});
		pass.diffuse_history_valid = pixel_temporal;

		// 7. Reflections: half-res trace (screen -> SDF -> sky), full-res denoise over the specular
		if (in_state.sdf_gi.reflections)
		{
			static constexpr i32 BLOCK_OFFSETS[4][2] = { { 0, 0 }, { 1, 1 }, { 1, 0 }, { 0, 1 } };
			const i32* block_offset = BLOCK_OFFSETS[pass.frame_counter & 3];
			const ScreenTracePush trace_push = { { block_offset[0], block_offset[1], in_previous_color_valid ? 1 : 0, 0 } };
			const ScreenTracePush resolve_push = { { block_offset[0], block_offset[1], pass.reflection_history_valid ? 1 : 0, 0 } };
			DescriptorWriter reflection_trace_writer = pass.reflection_trace_pipeline.writer(ctx);
			reflection_trace_writer.sampled(0, pass.nearest_sampler, in_gbuffer_position.view())
				.sampled(1, pass.nearest_sampler, in_gbuffer_normal.view())
				.sampled(2, pass.nearest_sampler, in_gbuffer_rme.view())
				.sampled(3, pass.linear_sampler, in_previous_color.view())
				.storage_image(4, pass.reflection_trace.view).commit();
			graph.sampled(in_gbuffer_rme, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
			graph.storage_write(frame_graph_image(pass.reflection_trace), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);
			const u32 half_w = (pass.screen_extent.width + 1) / 2;
			const u32 half_h = (pass.screen_extent.height + 1) / 2;
			graph.compute([&]()
			{
				dispatch_shared(ctx, pass.reflection_trace_pipeline, reflection_trace_writer.set, shared,
					(half_w + 7) / 8, (half_h + 7) / 8, &trace_push);
			});

			GpuImage& history_previous = pass.reflection_history[previous];
			GpuImage& history_current = pass.reflection_history[current];
			DescriptorWriter reflection_resolve_writer = pass.reflection_resolve_pipeline.writer(ctx);
			reflection_resolve_writer.sampled(0, pass.nearest_sampler, in_gbuffer_position.view())
				.sampled(1, pass.nearest_sampler, in_gbuffer_normal.view())
				.sampled(2, pass.nearest_sampler, in_gbuffer_rme.view())
				.storage_image(3, pass.reflection_trace.view)
				.sampled(4, pass.linear_sampler, history_previous.view)
				.storage_image(5, history_current.view)
				.storage_image(6, pass.specular.view)
				.storage_image(7, pass.irradiance.view).commit();
			graph.storage_read(frame_graph_image(pass.reflection_trace));
			// A never-written history is only ever bound, not read (reflection_history_valid).
			graph.sampled(frame_graph_image(history_previous), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
			graph.storage_write(frame_graph_image(history_current), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);
			graph.storage_read_write(frame_graph_image(pass.specular), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
			graph.storage_read_write(frame_graph_image(pass.irradiance), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
			graph.compute([&]()
			{
				dispatch_shared(ctx, pass.reflection_resolve_pipeline, reflection_resolve_writer.set, shared,
					(pass.screen_extent.width + 7) / 8, (pass.screen_extent.height + 7) / 8, &resolve_push);
			});
			pass.reflection_history_valid = true;
		}
		else
		{
			pass.reflection_history_valid = false;
		}
		graph.sampled(frame_graph_image(pass.irradiance));
		graph.sampled(frame_graph_image(pass.specular));

		pass.prev_view_projection = in_view_projection;
		pass.history_valid = true;
		pass.frame_counter += 1;
		vulkan_end_debug_label(ctx);
		gpu_timestamps_end_scope(ctx, timing_slot);
	}

	// Automation: every level built and nothing rebuilt for a while.
	inline bool is_converged(const State& in_state)
	{
		return in_state.sdf_gi.levels_ready == LEVEL_COUNT
			&& in_state.sdf_gi.frames_since_rebuild >= CONVERGE_FRAMES;
	}
}
