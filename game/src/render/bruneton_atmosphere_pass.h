#pragma once

#include <chrono>
#include <cstdio>
#include <cstring>

#include "core/types.h"
#include "game_object/game_object.h"
#include "render/frame_data.h"
#include "render/fullscreen_pipeline.h"
#include "render/frame_render_graph.h"
#include "render/gpu_buffer.h"
#include "render/render_pass.h"
#include "render/sky_atmosphere_dirty.h"

static constexpr i32 BRUNETON_TRANSMITTANCE_WIDTH = 256;
static constexpr i32 BRUNETON_TRANSMITTANCE_HEIGHT = 64;
static constexpr i32 BRUNETON_SCATTERING_WIDTH = 256;
static constexpr i32 BRUNETON_SCATTERING_HEIGHT = 128;
static constexpr i32 BRUNETON_SCATTERING_DEPTH = 32;
static constexpr i32 BRUNETON_IRRADIANCE_WIDTH = 64;
static constexpr i32 BRUNETON_IRRADIANCE_HEIGHT = 16;
static constexpr i32 BRUNETON_SCATTERING_ORDERS = 4;
static constexpr VkFormat BRUNETON_LUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

struct BrunetonAtmosphereGpu
{
	HMM_Vec4 radii;
	HMM_Vec4 rayleigh_scattering;
	HMM_Vec4 mie_scattering;
	HMM_Vec4 mie_extinction;
	HMM_Vec4 absorption_extinction;
	HMM_Vec4 ground_albedo;
	HMM_Vec4 solar_irradiance;
};
static_assert(sizeof(BrunetonAtmosphereGpu) == 7 * sizeof(HMM_Vec4));

struct BrunetonPrecomputePushConstants
{
	i32 layer;
	i32 scattering_order;
};

inline BrunetonAtmosphereGpu bruneton_build_gpu_parameters(const SkyAtmosphere& sky)
{
	constexpr f32 bottom_radius_m = 6360000.0f;
	constexpr HMM_Vec3 base_rayleigh = { 5.802e-6f, 13.558e-6f, 33.100e-6f };
	constexpr HMM_Vec3 base_mie_scattering = { 3.996e-6f, 3.996e-6f, 3.996e-6f };
	constexpr HMM_Vec3 base_mie_extinction = { 4.440e-6f, 4.440e-6f, 4.440e-6f };
	constexpr HMM_Vec3 base_ozone = { 0.650e-6f, 1.881e-6f, 0.085e-6f };
	constexpr f32 degrees_to_radians = 0.01745329251994329577f;
	return {
		.radii = HMM_V4(
			bottom_radius_m,
			bottom_radius_m + sky.atmosphere_height_m,
			sky.sun_disc_angular_diameter_degrees * 0.5f * degrees_to_radians,
			cosf(sky.max_sun_zenith_angle_degrees * degrees_to_radians)),
		.rayleigh_scattering = HMM_V4(
			base_rayleigh.X * sky.air_density,
			base_rayleigh.Y * sky.air_density,
			base_rayleigh.Z * sky.air_density,
			-1.0f / sky.rayleigh_scale_height_m),
		.mie_scattering = HMM_V4(
			base_mie_scattering.X * sky.aerosol_density,
			base_mie_scattering.Y * sky.aerosol_density,
			base_mie_scattering.Z * sky.aerosol_density,
			sky.mie_anisotropy),
		.mie_extinction = HMM_V4(
			base_mie_extinction.X * sky.aerosol_density,
			base_mie_extinction.Y * sky.aerosol_density,
			base_mie_extinction.Z * sky.aerosol_density,
			-1.0f / sky.mie_scale_height_m),
		.absorption_extinction = HMM_V4(
			base_ozone.X * sky.ozone_density,
			base_ozone.Y * sky.ozone_density,
			base_ozone.Z * sky.ozone_density, 0.0f),
		.ground_albedo = HMM_V4V(sky.ground_albedo, 0.0f),
		.solar_irradiance = HMM_V4(1.474000f, 1.850400f, 1.911980f, 0.0f),
	};
}

struct BrunetonAtmospherePass
{
	RenderPass transmittance_pass;
	RenderPass irradiance_pass;
	RenderPass scattering_pass;
	RenderPass scattering_density_pass;

	DescriptorSetSchema descriptors;
	EffectPipelineLayout pipeline_layout;
	FullscreenPipeline transmittance_pipeline;
	FullscreenPipeline direct_irradiance_pipeline;
	FullscreenPipeline single_scattering_pipeline;
	FullscreenPipeline scattering_density_pipeline;
	FullscreenPipeline indirect_irradiance_pipeline;
	FullscreenPipeline multiple_scattering_pipeline;
	GpuBuffer<BrunetonAtmosphereGpu> parameter_buffers[MAX_FRAMES_IN_FLIGHT];

	SkyAtmosphere last_lut_parameters = {};
	bool has_precomputed = false;
	u64 precompute_count = 0;
	f64 last_precompute_ms = 0.0;

	void init(VulkanContext* ctx)
	{
		transmittance_pass.init((RenderPassDesc) {
			.num_outputs = 1,
			.outputs = {{ .format = BRUNETON_LUT_FORMAT, .store_op = VK_ATTACHMENT_STORE_OP_STORE }},
			.extent = render_target_extent_fixed(
				BRUNETON_TRANSMITTANCE_WIDTH, BRUNETON_TRANSMITTANCE_HEIGHT),
			.type = ERenderPassType::Single,
			.debug_label = "Bruneton Transmittance",
		});
		irradiance_pass.init((RenderPassDesc) {
			.num_outputs = 2,
			.outputs = {
				{ .format = BRUNETON_LUT_FORMAT, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
				{ .format = BRUNETON_LUT_FORMAT, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
			},
			.extent = render_target_extent_fixed(
				BRUNETON_IRRADIANCE_WIDTH, BRUNETON_IRRADIANCE_HEIGHT),
			.type = ERenderPassType::Single,
			.debug_label = "Bruneton Irradiance",
		});
		scattering_pass.init((RenderPassDesc) {
			.pass_count = BRUNETON_SCATTERING_DEPTH,
			.num_outputs = 3,
			.outputs = {
				{ .format = BRUNETON_LUT_FORMAT, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
				{ .format = BRUNETON_LUT_FORMAT, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
				{ .format = BRUNETON_LUT_FORMAT, .store_op = VK_ATTACHMENT_STORE_OP_STORE },
			},
			.extent = render_target_extent_fixed(
				BRUNETON_SCATTERING_WIDTH, BRUNETON_SCATTERING_HEIGHT),
			.type = ERenderPassType::Array,
			.debug_label = "Bruneton Scattering",
		});
		scattering_density_pass.init((RenderPassDesc) {
			.pass_count = BRUNETON_SCATTERING_DEPTH,
			.num_outputs = 1,
			.outputs = {{ .format = BRUNETON_LUT_FORMAT, .store_op = VK_ATTACHMENT_STORE_OP_STORE }},
			.extent = render_target_extent_fixed(
				BRUNETON_SCATTERING_WIDTH, BRUNETON_SCATTERING_HEIGHT),
			.type = ERenderPassType::Array,
			.debug_label = "Bruneton Scattering Density",
		});

		DescriptorBindingSpec bindings[9] = {};
		bindings[0] = { .binding = 0, .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER };
		for (u32 binding = 1; binding < 9; ++binding)
		{
			bindings[binding] = { .binding = binding,
				.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
		}
		descriptors.init(ctx, bindings, 9,
			EDescriptorSetAllocation::PersistentPerFrame);
		pipeline_layout.init(ctx, &descriptors.layout, 1,
			sizeof(BrunetonPrecomputePushConstants), VK_SHADER_STAGE_FRAGMENT_BIT);

		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			parameter_buffers[frame_idx] = GpuBuffer((GpuBufferDesc<BrunetonAtmosphereGpu>) {
				.data = nullptr,
				.size = sizeof(BrunetonAtmosphereGpu),
				.usage = { .uniform_buffer = true, .stream_update = true },
				.label = "Bruneton Atmosphere Parameters",
			});
			parameter_buffers[frame_idx].get_gpu_buffer();
		}

		const VkFormat one_format[] = { BRUNETON_LUT_FORMAT };
		const VkFormat two_formats[] = { BRUNETON_LUT_FORMAT, BRUNETON_LUT_FORMAT };
		const VkFormat three_formats[] = { BRUNETON_LUT_FORMAT, BRUNETON_LUT_FORMAT, BRUNETON_LUT_FORMAT };
		auto create_pipeline = [&](FullscreenPipeline& pipeline, const char* shader,
			const VkFormat* formats, u32 count, u32 additive_mask = 0) {
			pipeline.init(ctx, {
				.vertex_shader_path = "bin/shaders/bruneton_precompute.vert.spv",
				.fragment_shader_path = shader,
				.pipeline_layout = pipeline_layout.layout,
				.color_formats = formats,
				.color_format_count = count,
				.additive_blend_mask = additive_mask,
			});
		};
		create_pipeline(transmittance_pipeline, "bin/shaders/bruneton_transmittance.frag.spv", one_format, 1);
		create_pipeline(direct_irradiance_pipeline, "bin/shaders/bruneton_direct_irradiance.frag.spv", two_formats, 2);
		create_pipeline(single_scattering_pipeline, "bin/shaders/bruneton_single_scattering.frag.spv", three_formats, 3);
		create_pipeline(scattering_density_pipeline, "bin/shaders/bruneton_scattering_density.frag.spv", one_format, 1);
		create_pipeline(indirect_irradiance_pipeline, "bin/shaders/bruneton_indirect_irradiance.frag.spv", two_formats, 2, 1u << 1);
		create_pipeline(multiple_scattering_pipeline, "bin/shaders/bruneton_multiple_scattering.frag.spv", three_formats, 3, 1u << 2);
	}

	void update(VulkanContext* ctx, const SkyAtmosphere& sky)
	{
		const u32 frame_idx = ctx->frame_index;
		const BrunetonAtmosphereGpu gpu_parameters = bruneton_build_gpu_parameters(sky);
		parameter_buffers[frame_idx].update_gpu_buffer(&gpu_parameters, sizeof(gpu_parameters));

		VkImageView image_views[8] = {
			transmittance_pass.get_color_output(0).view,
			scattering_pass.get_color_output(2).view,
			scattering_pass.get_color_output(0).view,
			scattering_pass.get_color_output(1).view,
			scattering_pass.get_color_output(0).view,
			scattering_density_pass.get_color_output(0).view,
			irradiance_pass.get_color_output(0).view,
			irradiance_pass.get_color_output(1).view,
		};
		DescriptorWriter writer = descriptors.writer(ctx);
		writer.buffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			parameter_buffers[frame_idx].get_gpu_buffer(), sizeof(BrunetonAtmosphereGpu));
		for (u32 image_idx = 0; image_idx < 8; ++image_idx)
			writer.sampled(image_idx + 1, frame_data.linear_sampler, image_views[image_idx]);
		writer.commit();
	}

	void bind_and_draw(VulkanContext* ctx, const FullscreenPipeline& pipeline, i32 layer, i32 order)
	{
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		pipeline.bind(ctx);
		VkDescriptorSet set = descriptors.current(ctx);
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipeline_layout.layout, 0, 1, &set, 0, nullptr);
		BrunetonPrecomputePushConstants push = { layer, order };
		pipeline_layout.push(ctx, push);
		vulkan_cmd_draw(ctx, 3, 1, 0, 0);
	}

	void declare_sampled_resources(
		FrameRenderGraph& graph,
		VulkanContext* ctx,
		VkPipelineStageFlags2 in_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT)
	{
		graph.uniform(frame_graph_buffer(
			parameter_buffers[ctx->frame_index].get_gpu_buffer()), in_stage);
		graph.sampled(frame_graph_color(transmittance_pass), in_stage);
		for (i32 output_idx = 0; output_idx < 3; ++output_idx)
			graph.sampled(frame_graph_color(scattering_pass, output_idx), in_stage);
		graph.sampled(frame_graph_color(scattering_density_pass), in_stage);
		graph.sampled(frame_graph_color(irradiance_pass, 0), in_stage);
		graph.sampled(frame_graph_color(irradiance_pass, 1), in_stage);
	}

	bool precompute_if_needed(
		FrameRenderGraph& graph, VulkanContext* ctx, const SkyAtmosphere& sky)
	{
		if (has_precomputed && bruneton_lut_parameters_equal(last_lut_parameters, sky))
		{
			return false;
		}
		const auto precompute_start = std::chrono::steady_clock::now();
		const auto declare_parameters = [&]() {
			graph.uniform(frame_graph_buffer(
				parameter_buffers[ctx->frame_index].get_gpu_buffer()),
				VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
		};

		declare_parameters();
		graph.execute(transmittance_pass, [&](i32) {
			bind_and_draw(ctx, transmittance_pipeline, 0, 0);
		});
		graph.make_sampled(transmittance_pass);

		irradiance_pass.desc.outputs[0].load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		irradiance_pass.desc.outputs[1].load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
		irradiance_pass.desc.outputs[1].clear_value = {};
		declare_parameters();
		graph.sampled(frame_graph_color(transmittance_pass));
		graph.execute(irradiance_pass, [&](i32) {
			bind_and_draw(ctx, direct_irradiance_pipeline, 0, 0);
		});
		graph.make_sampled(irradiance_pass);

		for (i32 output_idx = 0; output_idx < 3; ++output_idx)
			scattering_pass.desc.outputs[output_idx].load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		declare_parameters();
		graph.sampled(frame_graph_color(transmittance_pass));
		graph.execute(scattering_pass, [&](i32 layer) {
			bind_and_draw(ctx, single_scattering_pipeline, layer, 1);
		});
		graph.make_sampled(scattering_pass);

		for (i32 order = 2; order <= BRUNETON_SCATTERING_ORDERS; ++order)
		{
			scattering_density_pass.desc.outputs[0].load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			declare_parameters();
			graph.sampled(frame_graph_color(transmittance_pass));
			graph.sampled(frame_graph_color(scattering_pass, 0));
			graph.sampled(frame_graph_color(scattering_pass, 1));
			graph.sampled(frame_graph_color(irradiance_pass, 0));
			graph.execute(scattering_density_pass, [&](i32 layer) {
				bind_and_draw(ctx, scattering_density_pipeline, layer, order);
			});
			graph.make_sampled(scattering_density_pass);

			irradiance_pass.desc.outputs[0].load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			irradiance_pass.desc.outputs[1].load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
			declare_parameters();
			graph.sampled(frame_graph_color(scattering_pass, 0));
			graph.sampled(frame_graph_color(scattering_pass, 1));
			graph.execute(irradiance_pass, [&](i32) {
				bind_and_draw(ctx, indirect_irradiance_pipeline, 0, order - 1);
			});
			graph.make_sampled(irradiance_pass);

			scattering_pass.desc.outputs[0].load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			scattering_pass.desc.outputs[1].load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
			scattering_pass.desc.outputs[2].load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
			declare_parameters();
			graph.sampled(frame_graph_color(transmittance_pass));
			graph.sampled(frame_graph_color(scattering_density_pass));
			graph.execute(scattering_pass, [&](i32 layer) {
				bind_and_draw(ctx, multiple_scattering_pipeline, layer, order);
			});
			graph.make_sampled(scattering_pass);
		}

		last_lut_parameters = sky;
		has_precomputed = true;
		++precompute_count;
		last_precompute_ms = std::chrono::duration<f64, std::milli>(
			std::chrono::steady_clock::now() - precompute_start).count();
		printf("Bruneton LUT precompute recorded: #%llu, CPU submit %.2f ms\n",
			(unsigned long long) precompute_count, last_precompute_ms);
		return true;
	}

	void shutdown(VulkanContext* ctx)
	{
		multiple_scattering_pipeline.shutdown(ctx);
		indirect_irradiance_pipeline.shutdown(ctx);
		scattering_density_pipeline.shutdown(ctx);
		single_scattering_pipeline.shutdown(ctx);
		direct_irradiance_pipeline.shutdown(ctx);
		transmittance_pipeline.shutdown(ctx);
		pipeline_layout.shutdown(ctx);
		descriptors.shutdown(ctx);
		for (GpuBuffer<BrunetonAtmosphereGpu>& buffer : parameter_buffers)
			buffer.destroy_gpu_buffer();
		scattering_density_pass.cleanup();
		scattering_pass.cleanup();
		irradiance_pass.cleanup();
		transmittance_pass.cleanup();
	}
};

static BrunetonAtmospherePass bruneton_atmosphere_pass;
