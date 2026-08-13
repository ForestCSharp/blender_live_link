#pragma once

#include <limits>

#include "core/runtime_config.h"
#include "core/timings.h"
#include "render/auto_adaptation_math.h"
#include "render/bruneton_atmosphere_pass.h"
#include "render/gpu_buffer.h"
#include "render/gpu_image.h"
#include "render/shader_module.h"
#include "render/vulkan_context.h"
#include "state/state.h"

namespace AutoAdaptationPass
{
	static constexpr i32 STATE_VEC4_COUNT = 8;
	static constexpr i32 RESET_EXPOSURE = 1;
	static constexpr i32 RESET_WHITE_BALANCE = 2;

	struct MeasurementBufferData
	{
		HMM_Vec4 exposure_white;
		HMM_Vec4 target_log_lms;
		HMM_Vec4 solar_diagnostics;
	};
	static_assert(sizeof(MeasurementBufferData) == 48);

	struct StateBufferData
	{
		HMM_Vec4 values[STATE_VEC4_COUNT];
	};
	static_assert(sizeof(StateBufferData) == 128);

	struct SolarGuardPushConstants
	{
		HMM_Vec4 sun_screen_uv_radius_enabled;
		HMM_Vec4 camera_position_and_planet_center;
		HMM_Vec4 sun_direction_and_disc_intensity;
		HMM_Vec4 sun_tint_and_irradiance;
		HMM_Vec4 extent_and_ev_limits;
	};
	static_assert(sizeof(SolarGuardPushConstants) == 80);

	struct UpdatePushConstants
	{
		f32 delta_time;
		i32 auto_exposure_enabled;
		i32 auto_white_balance_enabled;
		i32 reset_mask;
		f32 auto_exposure_min_ev;
		f32 auto_exposure_max_ev;
		f32 auto_exposure_darkening_seconds;
		f32 auto_exposure_brightening_seconds;
		f32 auto_white_balance_seconds;
		f32 auto_white_balance_strength;
	};
	static_assert(sizeof(UpdatePushConstants) == 40);

	struct ReducePushConstants
	{
		f32 auto_exposure_min_ev;
		f32 auto_exposure_max_ev;
	};
	static_assert(sizeof(ReducePushConstants) == 8);

	struct ComputePipeline
	{
		VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
	};

	struct Pass
	{
		ComputePipeline histogram_pipeline;
		ComputePipeline reduce_pipeline;
		ComputePipeline solar_guard_pipeline;
		ComputePipeline update_pipeline;
		GpuBuffer<AutoAdaptationMath::HistogramBin> histogram;
		GpuBuffer<MeasurementBufferData> measurement;
		GpuBuffer<StateBufferData> state;
		GpuBuffer<StateBufferData> diagnostic_readback[MAX_FRAMES_IN_FLIGHT];
		bool readback_pending[MAX_FRAMES_IN_FLIGHT] = {};
		VkSampler sampler = VK_NULL_HANDLE;
		VkSampler nearest_sampler = VK_NULL_HANDLE;
		bool metered_this_frame = false;
		bool camera_key_initialized = false;
		i32 previous_camera_key = 0;
		bool enable_state_initialized = false;
		bool previous_auto_exposure_enabled = true;
		bool previous_auto_white_balance_enabled = true;
		i32 pending_reset_mask = RESET_EXPOSURE | RESET_WHITE_BALANCE;
	};

	inline Pass pass;

	inline StateBufferData identity_state()
	{
		StateBufferData result = {};
		result.values[0] = HMM_V4(0.0f, 0.0f, 0.3127f, 0.3290f);
		result.values[3] = HMM_V4(1.0f, 0.0f, 0.0f, 0.0f);
		result.values[4] = HMM_V4(0.0f, 1.0f, 0.0f, 0.0f);
		result.values[5] = HMM_V4(0.0f, 0.0f, 1.0f, 0.0f);
		return result;
	}

	inline void create_pipeline(
		VulkanContext* ctx,
		ComputePipeline& out_pipeline,
		const VkDescriptorSetLayoutBinding* in_bindings,
		u32 in_binding_count,
		const char* in_shader_path,
		u32 in_push_constant_size = 0)
	{
		VkDescriptorSetLayoutCreateInfo set_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = in_binding_count,
			.pBindings = in_bindings,
		};
		VK_CHECK(vkCreateDescriptorSetLayout(
			ctx->device, &set_info, nullptr, &out_pipeline.set_layout));

		VkPushConstantRange push_range = {
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.offset = 0,
			.size = in_push_constant_size,
		};
		VkPipelineLayoutCreateInfo layout_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &out_pipeline.set_layout,
			.pushConstantRangeCount = in_push_constant_size > 0 ? 1u : 0u,
			.pPushConstantRanges = in_push_constant_size > 0 ? &push_range : nullptr,
		};
		VK_CHECK(vkCreatePipelineLayout(
			ctx->device, &layout_info, nullptr, &out_pipeline.pipeline_layout));

		VkShaderModule module = create_shader_module_from_file(ctx->device, in_shader_path);
		VkComputePipelineCreateInfo pipeline_info = {
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT,
				.module = module,
				.pName = "main",
			},
			.layout = out_pipeline.pipeline_layout,
		};
		VK_CHECK(vulkan_create_compute_pipelines(
			ctx, 1, &pipeline_info, &out_pipeline.pipeline));
		vkDestroyShaderModule(ctx->device, module, nullptr);
	}

	inline void init(VulkanContext* ctx, VkSampler in_sampler)
	{
		pass.sampler = in_sampler;
		VkSamplerCreateInfo nearest_info = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_NEAREST,
			.minFilter = VK_FILTER_NEAREST,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};
		VK_CHECK(vkCreateSampler(ctx->device, &nearest_info, nullptr, &pass.nearest_sampler));
		const VkDescriptorSetLayoutBinding histogram_bindings[] = {
			{
				.binding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			},
			{
				.binding = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			},
		};
		const VkDescriptorSetLayoutBinding buffer_bindings[] = {
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
		};
		const VkDescriptorSetLayoutBinding solar_guard_bindings[] = {
			{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
			{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
			{ .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
			{ .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
		};
		create_pipeline(ctx, pass.histogram_pipeline, histogram_bindings, 2,
			"bin/shaders/auto_adaptation_histogram.comp.spv");
		create_pipeline(ctx, pass.reduce_pipeline, buffer_bindings, 2,
			"bin/shaders/auto_adaptation_reduce.comp.spv", sizeof(ReducePushConstants));
		create_pipeline(ctx, pass.solar_guard_pipeline, solar_guard_bindings, 4,
			"bin/shaders/auto_adaptation_solar_guard.comp.spv",
			sizeof(SolarGuardPushConstants));
		create_pipeline(ctx, pass.update_pipeline, buffer_bindings, 2,
			"bin/shaders/auto_adaptation_update.comp.spv", sizeof(UpdatePushConstants));

		AutoAdaptationMath::HistogramBin initial_histogram[AutoAdaptationMath::HISTOGRAM_BIN_COUNT] = {};
		MeasurementBufferData initial_measurement = {};
		StateBufferData initial_state = identity_state();
		pass.histogram = GpuBuffer((GpuBufferDesc<AutoAdaptationMath::HistogramBin>){
			.data = initial_histogram,
			.size = sizeof(initial_histogram),
			.usage = { .storage_buffer = true, .stream_update = true },
			.label = "Auto adaptation histogram",
		});
		pass.measurement = GpuBuffer((GpuBufferDesc<MeasurementBufferData>){
			.data = &initial_measurement,
			.size = sizeof(initial_measurement),
			.usage = { .storage_buffer = true, .stream_update = true },
			.label = "Auto adaptation measurement",
		});
		pass.state = GpuBuffer((GpuBufferDesc<StateBufferData>){
			.data = &initial_state,
			.size = sizeof(initial_state),
			.usage = { .storage_buffer = true, .stream_update = true, .transfer_src = true },
			.label = "Auto adaptation state",
		});
		pass.histogram.get_gpu_buffer();
		pass.measurement.get_gpu_buffer();
		pass.state.get_gpu_buffer();
		for (u32 frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame)
		{
			pass.diagnostic_readback[frame] = GpuBuffer((GpuBufferDesc<StateBufferData>){
				.data = nullptr,
				.size = sizeof(StateBufferData),
				.usage = { .stream_update = true, .readback = true },
				.label = "Auto adaptation diagnostic readback",
			});
			pass.diagnostic_readback[frame].get_gpu_buffer();
		}
	}

	inline VkBuffer state_buffer()
	{
		return pass.state.get_gpu_buffer();
	}

	inline bool active(const State::TonemappingState& state)
	{
		return RuntimeConfig::get().tonemap_validation_chart == 0
			&& (state.auto_exposure_enabled || state.auto_white_balance_enabled);
	}

	inline void consume_diagnostics(VulkanContext* ctx, State::TonemappingState& state)
	{
		const u32 frame = ctx->frame_index;
		if (!pass.readback_pending[frame]) return;
		StateBufferData data = {};
		pass.diagnostic_readback[frame].read_gpu_buffer(&data, sizeof(data));
		pass.readback_pending[frame] = false;
		state.adaptation_current_ev = data.values[0].X;
		state.adaptation_target_ev = data.values[0].Y;
		state.adaptation_measured_white_x = data.values[0].Z;
		state.adaptation_measured_white_y = data.values[0].W;
		state.adaptation_current_l_gain = std::exp2(data.values[1].X);
		state.adaptation_current_m_gain = std::exp2(data.values[1].Y);
		state.adaptation_current_s_gain = std::exp2(data.values[1].Z);
		state.adaptation_measurement_valid = data.values[6].X > 0.5f;
		state.adaptation_accepted_sample_count = (i32)data.values[6].W;
		state.adaptation_base_target_ev = data.values[7].X;
		state.adaptation_guarded_target_ev = data.values[7].Y;
		state.adaptation_solar_guard_weight = data.values[7].Z;
		state.adaptation_solar_disc_ev = data.values[7].W;
	}

	inline void prepare_frame(State& state)
	{
		const i32 no_camera = std::numeric_limits<i32>::min() + 2;
		const i32 debug_camera = std::numeric_limits<i32>::min() + 1;
		const i32 camera_key = state.debug_camera.active
			? debug_camera
			: state.scene.camera_control_id.value_or(no_camera);
		if (!pass.camera_key_initialized || camera_key != pass.previous_camera_key)
		{
			pass.pending_reset_mask |= RESET_EXPOSURE | RESET_WHITE_BALANCE;
			pass.camera_key_initialized = true;
			pass.previous_camera_key = camera_key;
		}
		if (!pass.enable_state_initialized)
		{
			pass.enable_state_initialized = true;
			pass.previous_auto_exposure_enabled = state.tonemapping.auto_exposure_enabled;
			pass.previous_auto_white_balance_enabled = state.tonemapping.auto_white_balance_enabled;
		}
		else
		{
			if (state.tonemapping.auto_exposure_enabled
				&& !pass.previous_auto_exposure_enabled)
				pass.pending_reset_mask |= RESET_EXPOSURE;
			if (state.tonemapping.auto_white_balance_enabled
				&& !pass.previous_auto_white_balance_enabled)
				pass.pending_reset_mask |= RESET_WHITE_BALANCE;
			pass.previous_auto_exposure_enabled = state.tonemapping.auto_exposure_enabled;
			pass.previous_auto_white_balance_enabled = state.tonemapping.auto_white_balance_enabled;
		}
		if (state.tonemapping.adaptation_reset_requested)
		{
			pass.pending_reset_mask |= RESET_EXPOSURE | RESET_WHITE_BALANCE;
			state.tonemapping.adaptation_reset_requested = false;
		}
	}

	inline VkDescriptorSet allocate_buffer_set(
		VulkanContext* ctx,
		const ComputePipeline& pipeline,
		VkBuffer buffer0,
		VkBuffer buffer1)
	{
		VkDescriptorSet set = vulkan_allocate_transient_descriptor_set(ctx, pipeline.set_layout);
		VkDescriptorBufferInfo infos[] = {
			descriptor_buffer(buffer0),
			descriptor_buffer(buffer1),
		};
		VkWriteDescriptorSet writes[] = {
			descriptor_write_buffer(set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &infos[0]),
			descriptor_write_buffer(set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &infos[1]),
		};
		vulkan_update_descriptor_sets(ctx, 2, writes, 0, nullptr, false);
		return set;
	}

	inline void bind_compute(
		VulkanContext* ctx,
		const ComputePipeline& pipeline,
		VkDescriptorSet set)
	{
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
			pipeline.pipeline_layout, 0, 1, &set, 0, nullptr);
	}

	inline void meter(
		VulkanContext* ctx,
		GpuImage& scene_color,
		GpuImage& position,
		const State::TonemappingState& state,
		SolarGuardPushConstants solar_constants)
	{
		pass.metered_this_frame = false;
		if (!active(state)) return;

		CPU_TIMING_SCOPE("Auto Adaptation Meter");
		const i32 timing_slot = gpu_timestamps_begin_scope(ctx, "Auto Adaptation Meter");
		vulkan_begin_debug_label(ctx, "Auto Adaptation Meter");
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		VkBuffer histogram = pass.histogram.get_gpu_buffer();
		VkBuffer measurement = pass.measurement.get_gpu_buffer();

		PassResourceUsage clear_usage;
		clear_usage.buffers.add({
			.buffer = histogram,
			.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			.access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		});
		vulkan_apply_pass_resource_usage(ctx, clear_usage);
		vkCmdFillBuffer(command_buffer, histogram, 0, VK_WHOLE_SIZE, 0);

		PassResourceUsage histogram_usage;
		histogram_usage.images.add({
			.image = &scene_color,
			.range = {
				.aspectMask = scene_color.aspects,
				.baseMipLevel = 0,
				.levelCount = scene_color.mip_levels,
				.baseArrayLayer = 0,
				.layerCount = scene_color.array_layers,
			},
			.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		});
		histogram_usage.buffers.add({
			.buffer = histogram,
			.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		});
		vulkan_apply_pass_resource_usage(ctx, histogram_usage);
		VkDescriptorSet histogram_set = vulkan_allocate_transient_descriptor_set(
			ctx, pass.histogram_pipeline.set_layout);
		VkDescriptorImageInfo image_info = descriptor_sampled(pass.sampler, scene_color.view);
		VkDescriptorBufferInfo histogram_info = descriptor_buffer(histogram);
		VkWriteDescriptorSet histogram_writes[] = {
			descriptor_write_image(histogram_set, 0,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &image_info),
			descriptor_write_buffer(histogram_set, 1,
				VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &histogram_info),
		};
		vulkan_update_descriptor_sets(ctx, 2, histogram_writes, 0, nullptr, false);
		bind_compute(ctx, pass.histogram_pipeline, histogram_set);
		vulkan_cmd_dispatch(ctx, 16, 9, 1);

		PassResourceUsage reduce_usage;
		reduce_usage.buffers.add({
			.buffer = histogram,
			.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
		});
		reduce_usage.buffers.add({
			.buffer = measurement,
			.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		});
		vulkan_apply_pass_resource_usage(ctx, reduce_usage);
		VkDescriptorSet reduce_set = allocate_buffer_set(
			ctx, pass.reduce_pipeline, histogram, measurement);
		bind_compute(ctx, pass.reduce_pipeline, reduce_set);
		ReducePushConstants reduce_constants = {
			.auto_exposure_min_ev = CLAMP(state.auto_exposure_min_ev, -16.0f, 16.0f),
			.auto_exposure_max_ev = CLAMP(state.auto_exposure_max_ev, -16.0f, 16.0f),
		};
		vkCmdPushConstants(command_buffer, pass.reduce_pipeline.pipeline_layout,
			VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(reduce_constants), &reduce_constants);
		vulkan_cmd_dispatch(ctx, 1, 1, 1);

		PassResourceUsage solar_usage;
		solar_usage.images.add({
			.image = &position,
			.range = { .aspectMask = position.aspects, .baseMipLevel = 0,
				.levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
			.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		});
		GpuImage& transmittance =
			bruneton_atmosphere_pass.transmittance_pass.get_color_output(0);
		solar_usage.images.add({
			.image = &transmittance,
			.range = { .aspectMask = transmittance.aspects, .baseMipLevel = 0,
				.levelCount = 1, .baseArrayLayer = 0, .layerCount = 1 },
			.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		});
		solar_usage.buffers.add({ .buffer = measurement,
			.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT
				| VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT });
		solar_usage.buffers.add({
			.buffer = bruneton_atmosphere_pass.parameter_buffers[ctx->frame_index]
				.get_gpu_buffer(),
			.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.access = VK_ACCESS_2_UNIFORM_READ_BIT });
		vulkan_apply_pass_resource_usage(ctx, solar_usage);
		VkDescriptorSet solar_set = vulkan_allocate_transient_descriptor_set(
			ctx, pass.solar_guard_pipeline.set_layout);
		VkDescriptorBufferInfo solar_buffer_infos[] = {
			descriptor_buffer(measurement),
			descriptor_buffer(
				bruneton_atmosphere_pass.parameter_buffers[ctx->frame_index].get_gpu_buffer(),
				sizeof(BrunetonAtmosphereGpu)),
		};
		VkDescriptorImageInfo solar_image_infos[] = {
			descriptor_sampled(pass.nearest_sampler, position.view),
			descriptor_sampled(pass.sampler, transmittance.view),
		};
		VkWriteDescriptorSet solar_writes[] = {
			descriptor_write_buffer(solar_set, 0,
				VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &solar_buffer_infos[0]),
			descriptor_write_image(solar_set, 1,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &solar_image_infos[0]),
			descriptor_write_buffer(solar_set, 2,
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &solar_buffer_infos[1]),
			descriptor_write_image(solar_set, 3,
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &solar_image_infos[1]),
		};
		vulkan_update_descriptor_sets(ctx, 4, solar_writes, 0, nullptr, false);
		bind_compute(ctx, pass.solar_guard_pipeline, solar_set);
		solar_constants.extent_and_ev_limits = HMM_V4(
			(f32)position.extent.width, (f32)position.extent.height,
			reduce_constants.auto_exposure_min_ev,
			reduce_constants.auto_exposure_max_ev);
		vkCmdPushConstants(command_buffer, pass.solar_guard_pipeline.pipeline_layout,
			VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(solar_constants), &solar_constants);
		vulkan_cmd_dispatch(ctx, 1, 1, 1);

		pass.metered_this_frame = true;
		vulkan_end_debug_label(ctx);
		gpu_timestamps_end_scope(ctx, timing_slot);
	}

	inline void update_after_tonemapping(
		VulkanContext* ctx,
		State::TonemappingState& state,
		f32 delta_time)
	{
		if (!pass.metered_this_frame) return;
		CPU_TIMING_SCOPE("Auto Adaptation Update");
		const i32 timing_slot = gpu_timestamps_begin_scope(ctx, "Auto Adaptation Update");
		vulkan_begin_debug_label(ctx, "Auto Adaptation Update");
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		VkBuffer measurement = pass.measurement.get_gpu_buffer();
		VkBuffer state_buffer_handle = pass.state.get_gpu_buffer();

		PassResourceUsage update_usage;
		update_usage.buffers.add({
			.buffer = measurement,
			.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
		});
		update_usage.buffers.add({
			.buffer = state_buffer_handle,
			.stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		});
		vulkan_apply_pass_resource_usage(ctx, update_usage);
		VkDescriptorSet update_set = allocate_buffer_set(
			ctx, pass.update_pipeline, measurement, state_buffer_handle);
		bind_compute(ctx, pass.update_pipeline, update_set);
		UpdatePushConstants constants = {
			.delta_time = CLAMP(delta_time, 0.0f, AutoAdaptationMath::MAX_FRAME_DELTA),
			.auto_exposure_enabled = state.auto_exposure_enabled ? 1 : 0,
			.auto_white_balance_enabled = state.auto_white_balance_enabled ? 1 : 0,
			.reset_mask = pass.pending_reset_mask,
			.auto_exposure_min_ev = CLAMP(state.auto_exposure_min_ev, -16.0f, 16.0f),
			.auto_exposure_max_ev = CLAMP(state.auto_exposure_max_ev, -16.0f, 16.0f),
			.auto_exposure_darkening_seconds = CLAMP(
				state.auto_exposure_darkening_seconds, 0.01f, 10.0f),
			.auto_exposure_brightening_seconds = CLAMP(
				state.auto_exposure_brightening_seconds, 0.01f, 10.0f),
			.auto_white_balance_seconds = CLAMP(
				state.auto_white_balance_seconds, 0.01f, 10.0f),
			.auto_white_balance_strength = CLAMP(
				state.auto_white_balance_strength, 0.0f, 1.0f),
		};
		vkCmdPushConstants(command_buffer, pass.update_pipeline.pipeline_layout,
			VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
		vulkan_cmd_dispatch(ctx, 1, 1, 1);
		pass.pending_reset_mask = 0;

		const u32 frame = ctx->frame_index;
		VkBuffer readback = pass.diagnostic_readback[frame].get_gpu_buffer();
		PassResourceUsage copy_usage;
		copy_usage.buffers.add({
			.buffer = state_buffer_handle,
			.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			.access = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
		});
		copy_usage.buffers.add({
			.buffer = readback,
			.stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			.access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		});
		vulkan_apply_pass_resource_usage(ctx, copy_usage);
		VkBufferCopy copy = { .size = sizeof(StateBufferData) };
		vkCmdCopyBuffer(command_buffer, state_buffer_handle, readback, 1, &copy);
		pass.readback_pending[frame] = true;
		pass.metered_this_frame = false;
		vulkan_end_debug_label(ctx);
		gpu_timestamps_end_scope(ctx, timing_slot);
	}

	inline void mark_state_for_fragment_read(VulkanContext* ctx)
	{
		PassResourceUsage usage;
		usage.buffers.add({
			.buffer = state_buffer(),
			.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
			.access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
		});
		vulkan_apply_pass_resource_usage(ctx, usage);
	}

	inline void shutdown(VulkanContext* ctx)
	{
		for (u32 frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame)
			pass.diagnostic_readback[frame].destroy_gpu_buffer();
		pass.state.destroy_gpu_buffer();
		pass.measurement.destroy_gpu_buffer();
		pass.histogram.destroy_gpu_buffer();
		ComputePipeline* pipelines[] = {
			&pass.update_pipeline, &pass.solar_guard_pipeline,
			&pass.reduce_pipeline, &pass.histogram_pipeline,
		};
		for (ComputePipeline* pipeline : pipelines)
		{
			vkDestroyPipeline(ctx->device, pipeline->pipeline, nullptr);
			vkDestroyPipelineLayout(ctx->device, pipeline->pipeline_layout, nullptr);
			vkDestroyDescriptorSetLayout(ctx->device, pipeline->set_layout, nullptr);
		}
		vkDestroySampler(ctx->device, pass.nearest_sampler, nullptr);
	}
}
