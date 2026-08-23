#pragma once

#include <cstring>

#include "state/state.h"
#include "render/bruneton_atmosphere_pass.h"
#include "render/frame_data.h"
#include "render/frame_render_graph.h"
#include "render/fullscreen_pipeline.h"
#include "render/gpu_buffer.h"
#include "render/gpu_image.h"
#include "render/render_pass.h"
#include "render/shader_module.h"
#include "render/sky_pass.h"

struct CloudLayerGpu
{
	HMM_Vec4 altitude_thickness_coverage_density;
	HMM_Vec4 scales_erosion_anvil;
	HMM_Vec4 wind_phase;
	HMM_Vec4 ambient_multi_profile_seed;
};

struct CloudGpuParams
{
	HMM_Mat4 previous_view_projection;
	HMM_Vec4 planet_center_time;
	HMM_Vec4 wind_weather;
	HMM_Vec4 sun_direction_layer_count;
	HMM_Vec4 sun_color_history;
	HMM_Vec4 shadow_extent_misc;
	HMM_Vec4 march_quality;
	HMM_Vec4 temporal_quality;
	CloudLayerGpu layers[MAX_CLOUD_LAYERS];
};
static_assert(sizeof(CloudGpuParams) == 432, "CloudGpuParams must match cloud_common.h std140 layout");

namespace CloudPass
{
	struct NoisePushConstants
	{
		u32 seed;
		u32 mode;
		u32 size;
		u32 layers;
	};

	struct Pass
	{
		GpuImage base_shape;
		GpuImage erosion;
		GpuImage weather;
		TypedComputeEffect<NoisePushConstants> noise_effect;

		DescriptorSetSchema sampled_descriptors;
		PerFrameDescriptorSets raymarch_sets;
		PerFrameDescriptorSets temporal_sets;
		PerFrameDescriptorSets composite_sets;
		PerFrameDescriptorSets shadow_sets;
		PerFrameUniform<CloudGpuParams> params;

		EffectPipelineLayout atmosphere_pipeline_layout;
		EffectPipelineLayout basic_pipeline_layout;
		FullscreenPipeline raymarch_pipeline;
		FullscreenPipeline temporal_pipeline;
		FullscreenPipeline composite_pipeline;
		FullscreenPipeline shadow_pipeline;

		VkSampler repeat_sampler = VK_NULL_HANDLE;
		bool caches_generated = false;
		bool shadow_initialized = false;
		u32 shadow_update_count = 0;
		u32 generated_seed = 0;
		i32 generated_layer_count = -1;
		u64 parameter_signature = 0;
		bool history_valid = false;
		i32 history_index = 0;
		HMM_Mat4 previous_view_projection = HMM_M4D(1.0f);
		HMM_Vec3 previous_camera_position = HMM_V3(0.0f, 0.0f, 0.0f);
		HMM_Vec3 previous_camera_forward = HMM_V3(0.0f, 0.0f, -1.0f);
		bool has_previous_camera = false;
	};

	inline Pass pass;

	inline u64 hash_mix(u64 hash, u64 value)
	{
		hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
		return hash;
	}

	inline u64 float_bits(f32 value)
	{
		u32 bits = 0;
		memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	inline u64 cloud_signature(i32 controller_id, const CloudSystem& cloud)
	{
		u64 result = hash_mix(0xcbf29ce484222325ull, (u64)(u32)controller_id);
		result = hash_mix(result, cloud.seed);
		result = hash_mix(result, cloud.layer_count);
		result = hash_mix(result, float_bits(cloud.weather_world_scale_m));
		result = hash_mix(result, float_bits(cloud.wind_direction.X));
		result = hash_mix(result, float_bits(cloud.wind_direction.Y));
		result = hash_mix(result, float_bits(cloud.wind_speed_m_s));
		result = hash_mix(result, cloud.shadow_enabled);
		result = hash_mix(result, float_bits(cloud.shadow_extent_m));
		for (i32 layer_index = 0; layer_index < cloud.layer_count; ++layer_index)
		{
			const CloudLayer& layer = cloud.layers[layer_index];
			result = hash_mix(result, layer.enabled);
			result = hash_mix(result, (u64)layer.profile);
			result = hash_mix(result, layer.seed_offset);
			const f32 values[] = { layer.base_altitude_m, layer.thickness_m, layer.coverage,
				layer.density, layer.shape_scale_m, layer.detail_scale_m, layer.erosion,
				layer.anvil_bias, layer.wind_multiplier, layer.phase_forward,
				layer.phase_backward, layer.phase_blend, layer.ambient_scale,
				layer.multi_scattering_strength };
			for (f32 value : values) result = hash_mix(result, float_bits(value));
		}
		return result;
	}

	inline void init(VulkanContext* ctx)
	{
		pass.base_shape = gpu_image_create(ctx->allocator, ctx->device, {
			.width = 128, .height = 128, .format = VK_FORMAT_R16_SFLOAT,
			.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.aspect = VK_IMAGE_ASPECT_COLOR_BIT, .array_layers = 128,
			.label = "Cloud Base Shape 128^3",
		});
		pass.erosion = gpu_image_create(ctx->allocator, ctx->device, {
			.width = 32, .height = 32, .format = VK_FORMAT_R16_SFLOAT,
			.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.aspect = VK_IMAGE_ASPECT_COLOR_BIT, .array_layers = 32,
			.label = "Cloud Erosion 32^3",
		});
		pass.weather = gpu_image_create(ctx->allocator, ctx->device, {
			.width = 512, .height = 512, .format = VK_FORMAT_R16G16_SFLOAT,
			.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.aspect = VK_IMAGE_ASPECT_COLOR_BIT, .array_layers = MAX_CLOUD_LAYERS,
			.label = "Cloud Weather Fields",
		});
		const DescriptorBindingSpec noise_bindings[] = {
			{ .binding = 0, .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				.stages = VK_SHADER_STAGE_COMPUTE_BIT },
			{ .binding = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				.stages = VK_SHADER_STAGE_COMPUTE_BIT },
			{ .binding = 2, .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				.stages = VK_SHADER_STAGE_COMPUTE_BIT },
		};
		pass.noise_effect.init(ctx, {
			.shader_path = "bin/shaders/cloud_noise.comp.spv",
			.bindings = noise_bindings,
			.binding_count = 3,
		});

		VkSamplerCreateInfo sampler_info = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_LINEAR,
			.minFilter = VK_FILTER_LINEAR,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
		};
		VK_CHECK(vkCreateSampler(ctx->device, &sampler_info, nullptr, &pass.repeat_sampler));

		DescriptorBindingSpec sampled_bindings[5] = {};
		sampled_bindings[0] = { .binding = 0, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER };
		for (u32 binding = 1; binding < 5; ++binding)
			sampled_bindings[binding] = { .binding = binding,
				.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
		pass.sampled_descriptors.init(ctx, sampled_bindings, 5,
			EDescriptorSetAllocation::Transient);
		pass.raymarch_sets.init_persistent(ctx, pass.sampled_descriptors.layout);
		pass.temporal_sets.init_persistent(ctx, pass.sampled_descriptors.layout);
		pass.composite_sets.init_persistent(ctx, pass.sampled_descriptors.layout);
		pass.shadow_sets.init_persistent(ctx, pass.sampled_descriptors.layout);
		pass.params.init("CloudPass::params");

		VkDescriptorSetLayout atmosphere_layouts[] = {
			frame_data.per_frame_layout, pass.sampled_descriptors.layout,
			bruneton_atmosphere_pass.descriptors.layout };
		pass.atmosphere_pipeline_layout.init(ctx, atmosphere_layouts,
			3, 0, 0);
		VkDescriptorSetLayout basic_layouts[] = {
			frame_data.per_frame_layout, pass.sampled_descriptors.layout };
		pass.basic_pipeline_layout.init(ctx, basic_layouts,
			2, 0, 0);

		const VkFormat pair_formats[] = { Render::SCENE_COLOR_FORMAT, Render::SCENE_COLOR_FORMAT };
		const VkFormat triple_formats[] = { Render::SCENE_COLOR_FORMAT, Render::SCENE_COLOR_FORMAT, Render::SCENE_COLOR_FORMAT };
		const VkFormat shadow_format = VK_FORMAT_R16_SFLOAT;
		pass.raymarch_pipeline.init(ctx, {
			.vertex_shader_path = "bin/shaders/cloud_raymarch.vert.spv",
			.fragment_shader_path = "bin/shaders/cloud_raymarch.frag.spv",
			.pipeline_layout = pass.atmosphere_pipeline_layout.layout,
			.color_formats = pair_formats, .color_format_count = 2,
		});
		pass.temporal_pipeline.init(ctx, {
			.vertex_shader_path = "bin/shaders/cloud_temporal.vert.spv",
			.fragment_shader_path = "bin/shaders/cloud_temporal.frag.spv",
			.pipeline_layout = pass.basic_pipeline_layout.layout,
			.color_formats = pair_formats, .color_format_count = 2,
		});
		pass.composite_pipeline.init(ctx, {
			.vertex_shader_path = "bin/shaders/cloud_composite.vert.spv",
			.fragment_shader_path = "bin/shaders/cloud_composite.frag.spv",
			.pipeline_layout = pass.atmosphere_pipeline_layout.layout,
			.color_formats = triple_formats, .color_format_count = 3,
		});
		pass.shadow_pipeline.init(ctx, {
			.vertex_shader_path = "bin/shaders/cloud_shadow.vert.spv",
			.fragment_shader_path = "bin/shaders/cloud_shadow.frag.spv",
			.pipeline_layout = pass.basic_pipeline_layout.layout,
			.color_formats = &shadow_format, .color_format_count = 1,
		});
	}

	inline void generate_caches(
		FrameRenderGraph& graph, VulkanContext* ctx, u32 seed, i32 layer_count)
	{
		layer_count = CLAMP(layer_count, 1, MAX_CLOUD_LAYERS);
		if (pass.caches_generated && pass.generated_seed == seed
			&& pass.generated_layer_count == layer_count) return;
		const i32 timing_slot = gpu_timestamps_begin_scope(ctx, "Cloud Cache Generation");
		GpuImage* images[] = { &pass.base_shape, &pass.erosion, &pass.weather };
		for (GpuImage* image : images)
			graph.storage_write(frame_graph_image(*image),
				VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, true);

		DescriptorWriter noise_writer = pass.noise_effect.writer(ctx);
		for (u32 index = 0; index < 3; ++index)
		{
			noise_writer.storage_image(index, images[index]->view);
		}
		noise_writer.commit();
		const NoisePushConstants dispatches[] = {
			{ seed, 0, 128, 128 }, { seed, 1, 32, 32 }, { seed, 2, 512, (u32)layer_count },
		};
		graph.compute([&]() {
			pass.noise_effect.bind(ctx, noise_writer.set);
			for (const NoisePushConstants& push : dispatches)
			{
				pass.noise_effect.dispatch(ctx, push,
					(push.size + 7) / 8, (push.size + 7) / 8, push.layers);
			}
		});
		for (GpuImage* image : images)
			graph.sampled(frame_graph_image(*image));
		graph.apply();
		gpu_timestamps_end_scope(ctx, timing_slot);
		pass.caches_generated = true;
		pass.generated_seed = seed;
		pass.generated_layer_count = layer_count;
		pass.history_valid = false;
		pass.shadow_initialized = false;
		pass.shadow_update_count = 0;
	}

	inline CloudGpuParams build_params(
		State& state, const Object& controller, HMM_Mat4 view_projection,
		HMM_Vec3 camera_position, HMM_Vec3 camera_forward, f32 delta_time)
	{
		const CloudSystem& cloud = controller.cloud_system;
		if (pass.has_previous_camera)
		{
			const f32 translation = HMM_LenV3(camera_position - pass.previous_camera_position);
			const f32 direction_alignment = HMM_DotV3(
				HMM_NormV3(camera_forward), HMM_NormV3(pass.previous_camera_forward));
			if (translation > 500.0f || direction_alignment < 0.5f)
				pass.history_valid = false;
		}
		CloudGpuParams params = {};
		params.previous_view_projection = pass.previous_view_projection;
		HMM_Vec2 wind = cloud.wind_direction;
		const f32 wind_length = HMM_LenV2(wind);
		wind = wind_length > 1.0e-5f ? wind / wind_length : HMM_V2(1.0f, 0.0f);
		params.planet_center_time = HMM_V4(controller.sky_atmosphere.planet_center_z_m,
			state.clouds.elapsed_time_seconds, (f32)state.vk.frame_number, delta_time);
		params.wind_weather = HMM_V4(wind.X, wind.Y, cloud.wind_speed_m_s, cloud.weather_world_scale_m);
		params.sun_direction_layer_count = HMM_V4V(sky_pass.active_sun_direction, (f32)cloud.layer_count);
		params.sun_color_history = HMM_V4V(sky_pass.active_sun_color, pass.history_valid ? 1.0f : 0.0f);
		params.shadow_extent_misc = HMM_V4(
			cloud.shadow_extent_m,
			cloud.shadow_enabled ? 1.0f : 0.0f,
			CLAMP(state.clouds.history_clip_sigma, 0.25f, 4.0f),
			CLAMP(state.clouds.opacity_rejection, 0.01f, 1.0f));
		params.march_quality = HMM_V4(
			(f32)CLAMP(state.clouds.view_steps, 12, 48),
			CLAMP(state.clouds.dense_step_scale, 0.5f, 1.0f),
			CLAMP(state.clouds.empty_step_scale, 1.0f, 4.0f),
			(f32)CLAMP(state.clouds.sun_cone_samples, 1, 8));
		params.temporal_quality = HMM_V4(
			CLAMP(state.clouds.history_weight, 0.0f, 0.98f),
			CLAMP(state.clouds.depth_rejection, 0.01f, 0.5f),
			CLAMP(state.clouds.low_density_edge_fade, 0.0f, 0.2f),
			CLAMP(state.clouds.minimum_density, 0.0f, 0.02f));
		for (i32 layer_index = 0; layer_index < cloud.layer_count; ++layer_index)
		{
			const CloudLayer& layer = cloud.layers[layer_index];
			CloudLayerGpu& gpu = params.layers[layer_index];
			gpu.altitude_thickness_coverage_density = HMM_V4(layer.base_altitude_m,
				layer.thickness_m, layer.coverage, layer.enabled ? layer.density : 0.0f);
			gpu.scales_erosion_anvil = HMM_V4(layer.shape_scale_m, layer.detail_scale_m,
				layer.erosion, layer.anvil_bias);
			gpu.wind_phase = HMM_V4(layer.wind_multiplier, layer.phase_forward,
				layer.phase_backward, layer.phase_blend);
			gpu.ambient_multi_profile_seed = HMM_V4(layer.ambient_scale,
				layer.multi_scattering_strength, (f32)layer.profile, (f32)layer.seed_offset);
		}
		pass.previous_view_projection = view_projection;
		pass.previous_camera_position = camera_position;
		pass.previous_camera_forward = camera_forward;
		pass.has_previous_camera = true;
		return params;
	}

	inline void write_sampled_set(
		VulkanContext* ctx, VkDescriptorSet set, VkBuffer params_buffer,
		VkImageView image1, VkSampler sampler1, VkImageView image2, VkSampler sampler2,
		VkImageView image3, VkSampler sampler3, VkImageView image4, VkSampler sampler4)
	{
		DescriptorWriter writer = pass.sampled_descriptors.writer(ctx, set, true);
		writer.buffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			params_buffer, sizeof(CloudGpuParams))
			.sampled(1, sampler1, image1)
			.sampled(2, sampler2, image2)
			.sampled(3, sampler3, image3)
			.sampled(4, sampler4, image4);
		writer.commit();
	}

	inline void bind_and_draw(VulkanContext* ctx, const FullscreenPipeline& pipeline,
		const EffectPipelineLayout& layout, VkDescriptorSet sampled_set, bool atmosphere)
	{
		VkDescriptorSet sets[] = { frame_data.per_frame_sets[ctx->frame_index], sampled_set,
			bruneton_atmosphere_pass.descriptors.current(ctx) };
		pipeline.bind(ctx);
		vkCmdBindDescriptorSets(vulkan_current_command_buffer(ctx), VK_PIPELINE_BIND_POINT_GRAPHICS,
			layout.layout, 0, atmosphere ? 3 : 2, sets, 0, nullptr);
		vulkan_cmd_draw(ctx, 3, 1, 0, 0);
	}

	inline void declare_params(
		FrameRenderGraph& graph, VkBuffer in_params_buffer)
	{
		graph.uniform(frame_graph_buffer(in_params_buffer),
			VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
	}

	inline void record_shadow_subgraph(
		FrameRenderGraph& graph,
		VulkanContext* ctx,
		RenderPass& in_shadow_target,
		FrameGraphImage in_position,
		VkBuffer in_params_buffer)
	{
		declare_params(graph, in_params_buffer);
		graph.sampled(frame_graph_image(pass.base_shape));
		graph.sampled(frame_graph_image(pass.erosion));
		graph.sampled(frame_graph_image(pass.weather));
		graph.sampled(in_position);
		graph.execute(in_shadow_target, [&](i32) {
			bind_and_draw(ctx, pass.shadow_pipeline, pass.basic_pipeline_layout,
				pass.shadow_sets.current(ctx), false);
		});
		pass.shadow_update_count += 1;
	}

	inline void record_main_subgraph(
		FrameRenderGraph& graph,
		VulkanContext* ctx,
		RenderPass& in_raymarch_target,
		RenderPass& in_previous_history,
		RenderPass& in_history_target,
		RenderPass& in_lighting,
		FrameGraphImage in_position,
		RenderPass& in_composite_target,
		VkBuffer in_params_buffer)
	{
		declare_params(graph, in_params_buffer);
		graph.sampled(frame_graph_image(pass.base_shape));
		graph.sampled(frame_graph_image(pass.erosion));
		graph.sampled(frame_graph_image(pass.weather));
		graph.sampled(in_position);
		bruneton_atmosphere_pass.declare_sampled_resources(graph, ctx);
		graph.execute(in_raymarch_target, [&](i32) {
			bind_and_draw(ctx, pass.raymarch_pipeline, pass.atmosphere_pipeline_layout,
				pass.raymarch_sets.current(ctx), true);
		});

		declare_params(graph, in_params_buffer);
		graph.sampled(frame_graph_color(in_raymarch_target, 0));
		graph.sampled(frame_graph_color(in_raymarch_target, 1));
		graph.sampled(frame_graph_color(in_previous_history, 0));
		graph.sampled(frame_graph_color(in_previous_history, 1));
		graph.execute(in_history_target, [&](i32) {
			bind_and_draw(ctx, pass.temporal_pipeline, pass.basic_pipeline_layout,
				pass.temporal_sets.current(ctx), false);
		});

		declare_params(graph, in_params_buffer);
		graph.sampled(frame_graph_color(in_lighting));
		graph.sampled(in_position);
		graph.sampled(frame_graph_color(in_history_target, 0));
		graph.sampled(frame_graph_color(in_history_target, 1));
		bruneton_atmosphere_pass.declare_sampled_resources(graph, ctx);
		graph.execute(in_composite_target, [&](i32) {
			bind_and_draw(ctx, pass.composite_pipeline, pass.atmosphere_pipeline_layout,
				pass.composite_sets.current(ctx), true);
		});
	}

	inline void initialize_shadow(
		FrameRenderGraph& graph, VulkanContext* ctx, GpuImage& shadow)
	{
		if (pass.shadow_initialized) return;
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		VkClearColorValue clear = { .float32 = { 1.0f, 1.0f, 1.0f, 1.0f } };
		VkImageSubresourceRange range = { .aspectMask = shadow.aspects, .baseMipLevel = 0,
			.levelCount = shadow.mip_levels, .baseArrayLayer = 0, .layerCount = shadow.array_layers };
		graph.transfer_destination(frame_graph_image(shadow), true);
		graph.transfer([&]() {
			vkCmdClearColorImage(command_buffer, shadow.image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
		});
		graph.make_sampled(frame_graph_image(shadow));
		pass.shadow_initialized = true;
		pass.shadow_update_count = 0;
	}

	inline void shutdown(VulkanContext* ctx)
	{
		pass.raymarch_pipeline.shutdown(ctx);
		pass.temporal_pipeline.shutdown(ctx);
		pass.composite_pipeline.shutdown(ctx);
		pass.shadow_pipeline.shutdown(ctx);
		pass.noise_effect.shutdown(ctx);
		pass.atmosphere_pipeline_layout.shutdown(ctx);
		pass.basic_pipeline_layout.shutdown(ctx);
		pass.params.shutdown();
		pass.sampled_descriptors.shutdown(ctx);
		vkDestroySampler(ctx->device, pass.repeat_sampler, nullptr);
		gpu_image_destroy(ctx->allocator, ctx->device, pass.base_shape);
		gpu_image_destroy(ctx->allocator, ctx->device, pass.erosion);
		gpu_image_destroy(ctx->allocator, ctx->device, pass.weather);
	}
}
