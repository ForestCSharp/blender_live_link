#pragma once

#include <limits>

#include "core/runtime_config.h"
#include "core/timings.h"
#include "render/auto_adaptation_math.h"
#include "render/bruneton_atmosphere_pass.h"
#include "render/fullscreen_pipeline.h"
#include "render/gpu_buffer.h"
#include "render/gpu_image.h"
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

	struct Pass
	{
		ComputeEffect histogram_pipeline;
		TypedComputeEffect<ReducePushConstants> reduce_pipeline;
		TypedComputeEffect<SolarGuardPushConstants> solar_guard_pipeline;
		TypedComputeEffect<UpdatePushConstants> update_pipeline;
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
		const DescriptorBindingSpec histogram_bindings[] = {
			{
				.binding = 0,
				.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.stages = VK_SHADER_STAGE_COMPUTE_BIT,
			},
			{
				.binding = 1,
				.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.stages = VK_SHADER_STAGE_COMPUTE_BIT,
			},
		};
		const DescriptorBindingSpec buffer_bindings[] = {
			{
				.binding = 0,
				.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.stages = VK_SHADER_STAGE_COMPUTE_BIT,
			},
			{
				.binding = 1,
				.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.stages = VK_SHADER_STAGE_COMPUTE_BIT,
			},
		};
		const DescriptorBindingSpec solar_guard_bindings[] = {
			{ .binding = 0, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.stages = VK_SHADER_STAGE_COMPUTE_BIT },
			{ .binding = 1, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.stages = VK_SHADER_STAGE_COMPUTE_BIT },
			{ .binding = 2, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.stages = VK_SHADER_STAGE_COMPUTE_BIT },
			{ .binding = 3, .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.stages = VK_SHADER_STAGE_COMPUTE_BIT },
		};
		pass.histogram_pipeline.init(ctx, {
			.shader_path = "bin/shaders/auto_adaptation_histogram.comp.spv",
			.bindings = histogram_bindings,
			.binding_count = 2,
		});
		pass.reduce_pipeline.init(ctx, {
			.shader_path = "bin/shaders/auto_adaptation_reduce.comp.spv",
			.bindings = buffer_bindings,
			.binding_count = 2,
		});
		pass.solar_guard_pipeline.init(ctx, {
			.shader_path = "bin/shaders/auto_adaptation_solar_guard.comp.spv",
			.bindings = solar_guard_bindings,
			.binding_count = 4,
		});
		pass.update_pipeline.init(ctx, {
			.shader_path = "bin/shaders/auto_adaptation_update.comp.spv",
			.bindings = buffer_bindings,
			.binding_count = 2,
		});

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
		ComputeEffect& effect,
		VkBuffer buffer0,
		VkBuffer buffer1)
	{
		DescriptorWriter writer = effect.writer(ctx);
		writer.buffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer0)
			.buffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer1)
			.commit();
		return writer.set;
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
		DescriptorWriter histogram_writer = pass.histogram_pipeline.writer(ctx);
		histogram_writer.sampled(0, pass.sampler, scene_color.view)
			.buffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, histogram)
			.commit();
		pass.histogram_pipeline.bind_and_dispatch(
			ctx, histogram_writer.set, 16, 9, 1);

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
			ctx, pass.reduce_pipeline.effect, histogram, measurement);
		ReducePushConstants reduce_constants = {
			.auto_exposure_min_ev = CLAMP(state.auto_exposure_min_ev, -16.0f, 16.0f),
			.auto_exposure_max_ev = CLAMP(state.auto_exposure_max_ev, -16.0f, 16.0f),
		};
		pass.reduce_pipeline.bind_and_dispatch(
			ctx, reduce_set, reduce_constants, 1, 1, 1);

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
		DescriptorWriter solar_writer = pass.solar_guard_pipeline.writer(ctx);
		solar_writer.buffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, measurement)
			.sampled(1, pass.nearest_sampler, position.view)
			.buffer(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				bruneton_atmosphere_pass.parameter_buffers[ctx->frame_index]
					.get_gpu_buffer(), sizeof(BrunetonAtmosphereGpu))
			.sampled(3, pass.sampler, transmittance.view)
			.commit();
		solar_constants.extent_and_ev_limits = HMM_V4(
			(f32)position.extent.width, (f32)position.extent.height,
			reduce_constants.auto_exposure_min_ev,
			reduce_constants.auto_exposure_max_ev);
		pass.solar_guard_pipeline.bind_and_dispatch(
			ctx, solar_writer.set, solar_constants, 1, 1, 1);

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
			ctx, pass.update_pipeline.effect, measurement, state_buffer_handle);
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
		pass.update_pipeline.bind_and_dispatch(
			ctx, update_set, constants, 1, 1, 1);
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
		pass.update_pipeline.shutdown(ctx);
		pass.solar_guard_pipeline.shutdown(ctx);
		pass.reduce_pipeline.shutdown(ctx);
		pass.histogram_pipeline.shutdown(ctx);
		vkDestroySampler(ctx->device, pass.nearest_sampler, nullptr);
	}
}
