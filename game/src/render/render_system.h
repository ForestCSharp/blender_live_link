#pragma once

#include <cmath>
#include <limits>

#include "input/input_system.h"
#include "render/auto_adaptation_pass.h"
#include "render/bloom_pass.h"
#include "render/cloud_pass.h"
#include "render/blur_pass.h"
#include "render/copy_to_swapchain_pass.h"
#include "render/dof_combine_pass.h"
#include "render/fog_pass.h"
#include "render/frame_render_graph.h"
#include "render/frame_data.h"
#include "render/fxaa_pass.h"
#include "render/geometry_pass.h"
#include "render/gi.h"
#include "render/gi_debug_pass.h"
#include "render/gpu_skinning.h"
#include "render/imgui_layer.h"
#include "render/lighting_pass.h"
#include "render/screen_space_shadows_pass.h"
#include "render/shadow_blur_pass.h"
#include "render/shadow_cascade_debug_pass.h"
#include "render/shadow_depth_pass.h"
#include "render/sky_pass.h"
#include "render/ssao_pass.h"
#include "render/temporal_aa_pass.h"
#include "render/tessellation.h"
#include "render/tonemapping_pass.h"
#include "render/wire_overlay_pass.h"
#include "state/state.h"

namespace RenderSystem
{
	inline GI_Scene g_gi_scene;

	inline GI_Scene& gi_scene()
	{
		return g_gi_scene;
	}

	inline bool dump_tonemapping_validation(State& in_state, const std::string& prefix)
	{
		GpuImage& tonemapped =
			get_render_target(RenderTargetId::Tonemapping).get_color_output(0);
		GpuImage& composite =
			get_render_target(RenderTargetId::PresentationComposite).get_color_output(0);
		return vulkan_context_dump_image_pfm(
			&in_state.vk, &tonemapped, (prefix + ".tonemapped.pfm").c_str())
			&& vulkan_context_dump_image_pfm(
				&in_state.vk, &composite, (prefix + ".composite.pfm").c_str());
	}

	inline bool dump_cloud_shadow_validation(State& in_state, const std::string& path)
	{
		const bool shadow_succeeded = vulkan_context_dump_image_pfm(
			&in_state.vk,
			&get_render_target(RenderTargetId::CloudShadow).get_color_output(0),
			path.c_str());
		const std::string lighting_path = path + ".lighting.pfm";
		const bool lighting_succeeded = vulkan_context_dump_image_pfm(
			&in_state.vk,
			&get_render_target(RenderTargetId::Lighting).get_color_output(0),
			lighting_path.c_str());
		return shadow_succeeded && lighting_succeeded;
	}

	// Derives the internal render size from the window size and resolution
	// percentage.
	void update_render_resolution(State& in_state)
	{
		in_state.window.resolution_percentage = CLAMP(
			in_state.window.resolution_percentage,
			MIN_RENDER_RESOLUTION_PERCENTAGE,
			MAX_RENDER_RESOLUTION_PERCENTAGE
		);
	
		const i32 source_width = in_state.window.width > 0 ? in_state.window.width : 1920;
		const i32 source_height = in_state.window.height > 0 ? in_state.window.height : 1080;
		const f32 scale = (f32) in_state.window.resolution_percentage / 100.0f;
	
		in_state.window.render_width = MAX(1, (i32)(source_width * scale + 0.5f));
		in_state.window.render_height = MAX(1, (i32)(source_height * scale + 0.5f));
	}
	
	// Resizes all pass targets when the framebuffer size changes.
	// Swapchain and explicitly full-output passes track the window; everything
	// else tracks the scaled render resolution.
	void resize(State& in_state, bool in_force = false)
	{
		const i32 framebuffer_width = (i32) in_state.vk.swapchain_extent.width;
		const i32 framebuffer_height = (i32) in_state.vk.swapchain_extent.height;
	
		if (!in_force && framebuffer_width == in_state.window.width && framebuffer_height == in_state.window.height)
		{
			return;
		}
	
		in_state.window.width = framebuffer_width;
		in_state.window.height = framebuffer_height;
		update_render_resolution(in_state);
		const f32 cloud_resolution_scale = CLAMP(in_state.clouds.resolution_scale, 0.5f, 1.0f);
		for (RenderTargetId id : {
			RenderTargetId::CloudRaymarch,
			RenderTargetId::CloudHistory0,
			RenderTargetId::CloudHistory1 })
		{
			RenderPass& target = in_state.render_targets.get(id);
			target.desc.extent.width_scale = cloud_resolution_scale;
			target.desc.extent.height_scale = cloud_resolution_scale;
		}
	
		// Old pass targets are retired against this frame slot; no device-wide
		// wait is required for render-scale or offscreen target replacement.
		ImGuiLayer::handle_swapchain_recreated(&in_state.vk);
		ImGuiLayer::clear_textures();
	
		in_state.render_targets.handle_resize(
			in_state.window.render_width, in_state.window.render_height,
			in_state.window.width, in_state.window.height);
		tonemapping_pass_handle_resize(
			&in_state.vk,
			in_state.window.render_width,
			in_state.window.render_height);
		BloomPass::handle_resize(
			&in_state.vk,
			in_state.window.render_width,
			in_state.window.render_height);
	
		// The TAA history targets were just recreated
		in_state.temporal_aa.history_valid = false;
		in_state.clouds.history_reset_requested = true;
		in_state.window.render_resolution_dirty = false;
	}

	bool ray_sphere_intersect(
		const HMM_Vec3& in_ray_origin,
		const HMM_Vec3& in_ray_direction,
		const HMM_Vec3& in_sphere_center,
		const f32 in_sphere_radius,
		f32& out_t)
	{
		const HMM_Vec3 oc = in_ray_origin - in_sphere_center;
		const f32 b = HMM_DotV3(oc, in_ray_direction);
		const f32 c = HMM_DotV3(oc, oc) - in_sphere_radius * in_sphere_radius;
		const f32 discriminant = b * b - c;
		if (discriminant < 0.0f)
		{
			return false;
		}
	
		const f32 sqrt_discriminant = HMM_SqrtF(discriminant);
		f32 t = -b - sqrt_discriminant;
		if (t < 0.0f) { t = -b + sqrt_discriminant; }
		if (t < 0.0f) { return false; }
		out_t = t;
		return true;
	}
	
	void pick_isolated_gi_probe(State& in_state)
	{
		i32 logical_width = 0;
		i32 logical_height = 0;
		glfwGetWindowSize(in_state.window.handle, &logical_width, &logical_height);
		const f32 width = (f32) logical_width;
		const f32 height = (f32) logical_height;
		if (width <= 0.0f || height <= 0.0f)
		{
			return;
		}
	
		const f32 fov = HMM_AngleDeg(60.0f);
		const f32 ndc_x = 2.0f * in_state.input.mouse_position.X / width - 1.0f;
		const f32 ndc_y = 1.0f - 2.0f * in_state.input.mouse_position.Y / height;
		const Camera& camera = InputSystem::active_camera(in_state);
		const HMM_Vec3 camera_forward = HMM_NormV3(camera.forward);
		const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera_forward, camera.up));
		const HMM_Vec3 camera_up = HMM_NormV3(HMM_Cross(camera_right, camera_forward));
		const f32 tan_half_fov = HMM_TanF(fov * 0.5f);
		const HMM_Vec3 ray_direction = HMM_NormV3(
			camera_forward
			+ camera_right * (ndc_x * (width / height) * tan_half_fov)
			+ camera_up * (ndc_y * tan_half_fov));
	
		i32 closest_probe_index = -1;
		f32 closest_t = std::numeric_limits<f32>::max();
		for (i32 probe_index = 0; probe_index < (i32)g_gi_scene.probes.length(); ++probe_index)
		{
			if (!gi_scene_debug_probe_matches_level_filter(
				g_gi_scene,
				probe_index,
				in_state.gi.probe_level_filter_enable,
				in_state.gi.probe_level_filter_selection))
			{
				continue;
			}
			f32 t = 0.0f;
			if (ray_sphere_intersect(
				camera.location,
				ray_direction,
				gi_scene_probe_position_from_index(g_gi_scene, probe_index),
				gi_scene_debug_probe_radius_for_probe(g_gi_scene, probe_index),
				t) && t < closest_t)
			{
				closest_t = t;
				closest_probe_index = probe_index;
			}
		}
		if (closest_probe_index >= 0)
		{
			in_state.gi.isolated_probe_index = closest_probe_index;
		}
	}

	inline void initialize(State& in_state, GLFWwindow* in_window)
	{
		vulkan_context_init(&in_state.vk, in_window);
			Render::configure_formats(in_state.vk);
			ImGuiLayer::init(&in_state.vk);
			#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
			glfwSetMonitorCallback(ImGui_ImplGlfw_MonitorCallback);
			#endif
			frame_data_init(&in_state.vk);
			geometry_pass_init(&in_state.vk);
			ShadowDepthPass::init(&in_state.vk);
			ShadowBlurPass::init(&in_state.vk);
			ssao_pass_init(&in_state.vk, frame_data.linear_sampler);
			BlurPass::init(&in_state.vk, Render::SSAO_FORMAT);
			ScreenSpaceShadowsPass::init(&in_state.vk, frame_data.linear_sampler);
			fog_pass_init(&in_state.vk, frame_data.linear_sampler);
			dof_combine_pass_init(&in_state.vk, frame_data.linear_sampler);
			WireOverlayPass::init(&in_state.vk, frame_data.linear_sampler);
			TemporalAAPass::init(&in_state.vk, frame_data.linear_sampler);
			FXAAPass::init(&in_state.vk, frame_data.linear_sampler);
			GpuSkinning::init(&in_state.vk);
			Tessellation::init(&in_state.vk);
			lighting_pass_init(&in_state.vk, frame_data.linear_sampler);
			sky_pass_init(&in_state.vk);
			CloudPass::init(&in_state.vk);
			BloomPass::init(&in_state.vk, frame_data.linear_sampler);
			AutoAdaptationPass::init(&in_state.vk, frame_data.linear_sampler);
			tonemapping_pass_init(&in_state.vk);
			copy_to_swapchain_pass_init(&in_state.vk);
		
			// These buffers must exist even for empty scenes: descriptor set 0
			// bindings 1-3 are written every frame
			render_object_snapshot_ensure_capacity(in_state, 1);
			init_materials_buffer(in_state);
			skin_matrix_arena_ensure_capacity(in_state, 1);
			init_lighting_buffers(in_state);
			gi_scene_init(&in_state.vk, g_gi_scene, in_state);
			GIDebugPass::init(&in_state.vk);
		
		// Register render passes and size their targets
			// Fixed-size cascaded shadow map: one moments image with a layer per
			// cascade. Clear {1,1,0,0} = "fully lit" EVSM moments so unrendered
			// cascades never darken receivers.
			in_state.render_targets.init(RenderTargetId::ShadowDepth, (RenderPassDesc) {
				.pass_count = MAX_SHADOW_CASCADES,
				.num_outputs = 1,
				.outputs = {
					{
						.format = Render::SHADOW_MOMENTS_FORMAT,
						.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
						.store_op = VK_ATTACHMENT_STORE_OP_STORE,
						.clear_value = {{{ 1.0f, 1.0f, 0.0f, 0.0f }}},
					},
				},
				.depth_output = {
					.format = Render::SCENE_DEPTH_FORMAT,
					.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
					.store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,
					.clear_value = { .depthStencil = { .depth = Render::DEPTH_CLEAR_VALUE } },
				},
				.extent = render_target_extent_fixed(
					ShadowDepthPass::ShadowMapResolution, ShadowDepthPass::ShadowMapResolution),
				.type = ERenderPassType::Array,
				.debug_label = "Shadow Depth",
			});
		
			// Separable moments blur: horizontal (intermediate) -> vertical (final),
			// same layered layout as the shadow map, no depth
			const auto make_shadow_blur_desc = [](const char* in_debug_label)
			{
				return (RenderPassDesc) {
					.pass_count = MAX_SHADOW_CASCADES,
					.num_outputs = 1,
					.outputs = {
						{
							.format = Render::SHADOW_MOMENTS_FORMAT,
							.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
							.store_op = VK_ATTACHMENT_STORE_OP_STORE,
							.clear_value = {{{ 1.0f, 1.0f, 0.0f, 0.0f }}},
						},
					},
					.extent = render_target_extent_fixed(
						ShadowDepthPass::ShadowMapResolution, ShadowDepthPass::ShadowMapResolution),
					.type = ERenderPassType::Array,
					.debug_label = in_debug_label,
				};
			};
			in_state.render_targets.init(RenderTargetId::ShadowBlurHorizontal, make_shadow_blur_desc("Shadow Blur Horizontal"));
			in_state.render_targets.init(RenderTargetId::ShadowBlurred, make_shadow_blur_desc("Shadow Blur Vertical"));
			in_state.render_targets.init(RenderTargetId::ShadowCascadeDebug, (RenderPassDesc) {
				.num_outputs = 1,
				.outputs = {{
					.format = Render::SCENE_COLOR_FORMAT,
					.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
					.store_op = VK_ATTACHMENT_STORE_OP_STORE,
					.clear_value = {{{ 0.0f, 0.0f, 0.0f, 1.0f }}},
				}},
				.extent = render_target_extent_fixed(
					ShadowDepthPass::ShadowMapResolution, ShadowDepthPass::ShadowMapResolution),
				.debug_label = "Shadow Cascade Debug",
			});
			ShadowCascadeDebugPass::init(&in_state.vk);
		
			// The blur's input sets are static: both source images are fixed-size and
			// never recreated
			ShadowBlurPass::init_sets(
				&in_state.vk,
				get_render_target(RenderTargetId::ShadowDepth).get_color_output(0).view,
				get_render_target(RenderTargetId::ShadowBlurHorizontal).get_color_output(0).view,
				frame_data.linear_sampler
			);
		
			// SSAO at half render resolution; the generic BlurPass smooths it before
			// lighting samples it.
			const auto make_ssao_desc = [](const char* in_debug_label)
			{
				return render_target_color_desc(
					in_debug_label, Render::SSAO_FORMAT, render_target_extent_scaled(0.5f),
					VK_ATTACHMENT_LOAD_OP_CLEAR, {{{ 1.0f, 1.0f, 1.0f, 1.0f }}});
			};
			in_state.render_targets.init(RenderTargetId::SSAO, make_ssao_desc("SSAO"));
			in_state.render_targets.init(RenderTargetId::SSAOBlurHorizontal, make_ssao_desc("SSAO Blur Horizontal"));
			in_state.render_targets.init(RenderTargetId::SSAOBlurred, make_ssao_desc("SSAO Blur Vertical"));
		
			// Screen-space contact shadows share the SSAO target shape (half res,
			// R8, clear 1 = fully visible)
			in_state.render_targets.init(RenderTargetId::ScreenSpaceShadowTrace, make_ssao_desc("Screen Space Shadows Trace"));
			in_state.render_targets.init(RenderTargetId::ScreenSpaceShadows, make_ssao_desc("Screen Space Shadows Filter"));
		
			in_state.render_targets.init(RenderTargetId::Geometry, (RenderPassDesc) {
				.num_outputs = Render::GBUFFER_OUTPUT_COUNT,
				.outputs = {
					// 0: base/emission color — sky-blue clear keeps empty scenes
					// readable until the sky pass executes.
					{
						.format = Render::GBUFFER_FORMAT,
						.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
						.store_op = VK_ATTACHMENT_STORE_OP_STORE,
						.clear_value = {{{ 0.0f, 0.0f, 0.0f, 1.0f }}},
					},
					// 1: world position
					{
						.format = Render::GBUFFER_FORMAT,
						.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
						.store_op = VK_ATTACHMENT_STORE_OP_STORE,
						.clear_value = {{{ 0.0f, 0.0f, 0.0f, 0.0f }}},
					},
					// 2: world normal
					{
						.format = Render::GBUFFER_FORMAT,
						.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
						.store_op = VK_ATTACHMENT_STORE_OP_STORE,
						.clear_value = {{{ 0.0f, 0.0f, 0.0f, 0.0f }}},
					},
					// 3: roughness / metallic / emission strength
					{
						.format = Render::GBUFFER_FORMAT,
						.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
						.store_op = VK_ATTACHMENT_STORE_OP_STORE,
						.clear_value = {{{ 0.0f, 0.0f, 0.0f, 0.0f }}},
					},
				},
				.depth_output = {
					.format = Render::SCENE_DEPTH_FORMAT,
					.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
					.store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,
					.clear_value = { .depthStencil = { .depth = Render::DEPTH_CLEAR_VALUE } },
				},
				.type = ERenderPassType::Single,
				.debug_label = "Geometry",
			});
		
			in_state.render_targets.init(RenderTargetId::Lighting,
				render_target_color_desc("Lighting", Render::SCENE_COLOR_FORMAT));

			const auto make_cloud_pair_desc = [&in_state](const char* label)
			{
				return render_target_mrt_desc(label, Render::SCENE_COLOR_FORMAT, 2,
					render_target_extent_scaled(in_state.clouds.resolution_scale),
					VK_ATTACHMENT_LOAD_OP_CLEAR);
			};
			in_state.render_targets.init(RenderTargetId::CloudRaymarch,
				make_cloud_pair_desc("Cloud Ray March"));
			in_state.render_targets.init(RenderTargetId::CloudHistory0,
				make_cloud_pair_desc("Cloud Temporal Set 0"));
			in_state.render_targets.init(RenderTargetId::CloudHistory1,
				make_cloud_pair_desc("Cloud Temporal Set 1"));
			in_state.render_targets.init(RenderTargetId::CloudComposite,
				render_target_mrt_desc("Cloud Composite", Render::SCENE_COLOR_FORMAT, 3,
					{}, VK_ATTACHMENT_LOAD_OP_CLEAR));
			in_state.render_targets.init(RenderTargetId::CloudShadow, (RenderPassDesc) {
				.num_outputs = 1,
				.outputs = {{ .format = VK_FORMAT_R16_SFLOAT,
					.load_op = VK_ATTACHMENT_LOAD_OP_LOAD, .store_op = VK_ATTACHMENT_STORE_OP_STORE }},
				.extent = render_target_extent_fixed(512, 512),
				.debug_label = "Cloud Sun Shadow",
			});
		
			in_state.render_targets.init(RenderTargetId::Fog,
				render_target_color_desc("Fog", Render::SCENE_COLOR_FORMAT));
			in_state.render_targets.init(RenderTargetId::DofCombine,
				render_target_color_desc("DOF Combine", Render::SCENE_COLOR_FORMAT));
			in_state.render_targets.init(RenderTargetId::WireOverlay,
				render_target_color_desc("Wire Overlay", Render::SCENE_COLOR_FORMAT));
		
			// TAA ping-pong: two target sets (intermediate = set 0, final = set 1),
			// each MRT [resolved, history]; the shader reads the other set's history
			const auto make_temporal_aa_desc = [](const char* in_debug_label)
			{
				return render_target_mrt_desc(
					in_debug_label, Render::SCENE_COLOR_FORMAT, 2);
			};
			in_state.render_targets.init(RenderTargetId::TemporalAAHistory0, make_temporal_aa_desc("Temporal AA (Set 0)"));
			in_state.render_targets.init(RenderTargetId::TemporalAAHistory1, make_temporal_aa_desc("Temporal AA (Set 1)"));
		
			in_state.render_targets.init(RenderTargetId::Tonemapping,
				render_target_color_desc("Tonemapping", Render::SCENE_COLOR_FORMAT));
			in_state.render_targets.init(RenderTargetId::FXAA,
				render_target_color_desc("FXAA", Render::SCENE_COLOR_FORMAT));
			in_state.render_targets.init(RenderTargetId::PresentationComposite,
				render_target_color_desc("Presentation Composite", Render::SCENE_COLOR_FORMAT,
					render_target_extent_output()));
		
			in_state.render_targets.init(RenderTargetId::Swapchain, (RenderPassDesc) {
				.extent = render_target_extent_output(),
				.type = ERenderPassType::Swapchain,
				.debug_label = "Copy To Swapchain",
			});
		
			resize(in_state, /*in_force=*/ true);
	}

	inline bool begin_frame(State& in_state)
	{
		if (!vulkan_context_begin_frame(&in_state.vk))
		{
			return false;
		}
		resize(in_state, in_state.window.render_resolution_dirty);
		AutoAdaptationPass::consume_diagnostics(
			&in_state.vk, in_state.tonemapping);
		ImGuiLayer::begin_frame();
		return true;
	}

	inline void render(State& in_state, f32 in_delta_time)
	{
		CPU_TIMING_SCOPE("Rendering");
		if (!in_state.tonemapping.local_enabled)
		{
			in_state.debug_ui.show_local_tonemapping_debug = false;
		}
		ImGuiLayer::draw_controls(in_state, g_gi_scene);
		AutoAdaptationPass::prepare_frame(in_state);
		FrameRenderGraph graph(&in_state.vk);
		
			// View + Projection matrix setup (TAA jitters the projection; the
			// unjittered previous VP is not kept, so reprojection uses the jittered
			// matrix).
			const Camera& camera = InputSystem::active_camera(in_state);
			const f32 fov = HMM_AngleDeg(60.0f);
			const f32 aspect_ratio = (f32) in_state.window.render_width / (f32) in_state.window.render_height;
			HMM_Mat4 projection_matrix = mat4_perspective(fov, aspect_ratio);
			if (in_state.temporal_aa.enable)
			{
				in_state.temporal_aa.jitter_phase = (i32) (in_state.vk.frame_number & 1);
				in_state.temporal_aa.current_jitter_pixels = TemporalAAPass::get_decima_jitter_pixels(in_state.temporal_aa.jitter_phase);
				projection_matrix = TemporalAAPass::apply_projection_jitter(
					projection_matrix,
					in_state.temporal_aa.current_jitter_pixels,
					HMM_V2((f32) in_state.window.render_width, (f32) in_state.window.render_height)
				);
			}
			else
			{
				in_state.temporal_aa.current_jitter_pixels = HMM_V2(0.0f, 0.0f);
			}
		
			const HMM_Vec3 target = camera.location + camera.forward * 10;
			const HMM_Mat4 view_matrix = HMM_LookAt_RH(camera.location, target, camera.up);
			const HMM_Mat4 view_projection_matrix = HMM_MulM4(projection_matrix, view_matrix);

			// Resolve/update the active atmosphere before any lighting, fog, probe,
			// or exposure descriptors consume this frame's parameters.
			sky_pass_update_atmosphere(graph, &in_state.vk, in_state);
		
			// Deform source vertices first, then plan/emit tessellation. GI captures
			// and all raster passes below consume the resulting MeshRenderView.
			GpuSkinning::update(graph, &in_state.vk, in_state,
				in_state.tessellation.enabled || in_state.wireframe.shaded_wireframe);
			Tessellation::update(graph, &in_state.vk, in_state, camera, fov);
		
			// Per-frame UBO sun data serves shadow/fog compatibility. The direct sky
			// uses its selected atmosphere controller, while geometry lighting comes
			// solely from the light buffers.
			PerFrameData per_frame_data = {
				.view = view_matrix,
				.projection = projection_matrix,
				.view_projection = view_projection_matrix,
				.inv_view_projection = HMM_InvGeneralM4(view_projection_matrix),
				.camera_position = HMM_V4V(camera.location, 1.0f),
				.camera_forward = HMM_V4V(camera.forward, 0.0f),
				.sun_direction = HMM_V4V(-HMM_NormV3(HMM_V3(0.3f, 0.5f, 0.8f)), 0.0f),
				.sun_color = HMM_V4(1.0f, 1.0f, 1.0f, 1.0f),
			};
		
			if (in_state.scene.primary_sun_id)
			{
				Object& sun_object = in_state.scene.objects[*in_state.scene.primary_sun_id];
				const HMM_Vec3 sun_direction = HMM_NormV3(HMM_RotateV3Q(HMM_V3(0.0f, 0.0f, -1.0f), sun_object.current_transform.rotation));
				per_frame_data.sun_direction = HMM_V4V(sun_direction, 0.0f);
				per_frame_data.sun_color = HMM_V4V(scene_solar_irradiance(
					sun_object.light.color, sun_object.light.sun.power), 1.0f);
			}
		
			// Descriptor writes happen before any of this frame's binds are recorded
			RenderPass& geometry_render_pass = get_render_target(RenderTargetId::Geometry);
			RenderPass& lighting_render_pass = get_render_target(RenderTargetId::Lighting);
			frame_data_update(
				&in_state.vk,
				per_frame_data,
				get_render_object_snapshot_buffer(in_state).get_gpu_buffer(),
				in_state.materials.buffer.get_gpu_buffer(),
				get_skin_matrix_arena_buffer(in_state).get_gpu_buffer(),
				in_state.images.items.data(),
				(i32) in_state.images.items.length()
			);
			RenderPass& tonemapping_render_pass = get_render_target(RenderTargetId::Tonemapping);
			RenderPass& cloud_raymarch_render_pass = get_render_target(RenderTargetId::CloudRaymarch);
			RenderPass& cloud_composite_render_pass = get_render_target(RenderTargetId::CloudComposite);
			RenderPass& cloud_shadow_render_pass = get_render_target(RenderTargetId::CloudShadow);
			const i32 cloud_history_output_index = CloudPass::pass.history_index;
			const i32 cloud_history_previous_index = (cloud_history_output_index + 1) % 2;
			const auto get_cloud_history_pass = [&](i32 set_index) -> RenderPass& {
				return get_render_target(set_index == 0
					? RenderTargetId::CloudHistory0 : RenderTargetId::CloudHistory1);
			};
			const bool cloud_render_active = in_state.clouds.active
				&& in_state.scene.active_cloud_controller_id.has_value()
				&& in_state.scene.objects.contains(*in_state.scene.active_cloud_controller_id)
				&& sky_pass.has_active_atmosphere
				&& bruneton_atmosphere_pass.has_precomputed
				&& in_state.clouds.active_layer_count > 0;
			const bool cloud_shadow_render_active = cloud_render_active
				&& in_state.scene.objects[*in_state.scene.active_cloud_controller_id]
					.cloud_system.shadow_enabled
				&& in_state.scene.objects[*in_state.scene.active_cloud_controller_id]
					.light.sun.cast_shadows;
			const bool cloud_shadow_lighting_active = cloud_shadow_render_active
				&& in_state.clouds.shadow_lighting_enabled;
			VkBuffer cloud_params_buffer = VK_NULL_HANDLE;
			if (cloud_render_active)
			{
				Object& cloud_controller =
					in_state.scene.objects[*in_state.scene.active_cloud_controller_id];
				const CloudSystem& cloud_system = cloud_controller.cloud_system;
				const u64 signature = CloudPass::cloud_signature(
					*in_state.scene.active_cloud_controller_id, cloud_system);
				if (signature != CloudPass::pass.parameter_signature
					|| in_state.clouds.history_reset_requested)
				{
					CloudPass::pass.parameter_signature = signature;
					CloudPass::pass.history_valid = false;
					in_state.clouds.history_reset_requested = false;
				}
				CloudPass::generate_caches(
					graph, &in_state.vk, cloud_system.seed, cloud_system.layer_count);
				in_state.clouds.elapsed_time_seconds += MAX(in_delta_time, 0.0f);
				CloudGpuParams cloud_params = CloudPass::build_params(
					in_state, cloud_controller, view_projection_matrix,
					camera.location, camera.forward, in_delta_time);
				cloud_params_buffer = CloudPass::pass.params.update(&in_state.vk, cloud_params);
				CloudPass::write_sampled_set(&in_state.vk,
					CloudPass::pass.raymarch_sets.current(&in_state.vk), cloud_params_buffer,
					CloudPass::pass.base_shape.view, CloudPass::pass.repeat_sampler,
					CloudPass::pass.erosion.view, CloudPass::pass.repeat_sampler,
					CloudPass::pass.weather.view, CloudPass::pass.repeat_sampler,
					geometry_render_pass.get_color_output(1).view, frame_data.linear_sampler);
				CloudPass::write_sampled_set(&in_state.vk,
					CloudPass::pass.temporal_sets.current(&in_state.vk), cloud_params_buffer,
					cloud_raymarch_render_pass.get_color_output(0).view, frame_data.linear_sampler,
					cloud_raymarch_render_pass.get_color_output(1).view, frame_data.linear_sampler,
					get_cloud_history_pass(cloud_history_previous_index).get_color_output(0).view, frame_data.linear_sampler,
					get_cloud_history_pass(cloud_history_previous_index).get_color_output(1).view, frame_data.linear_sampler);
				CloudPass::write_sampled_set(&in_state.vk,
					CloudPass::pass.composite_sets.current(&in_state.vk), cloud_params_buffer,
					lighting_render_pass.get_color_output(0).view, frame_data.linear_sampler,
					geometry_render_pass.get_color_output(1).view, frame_data.linear_sampler,
					get_cloud_history_pass(cloud_history_output_index).get_color_output(0).view, frame_data.linear_sampler,
					get_cloud_history_pass(cloud_history_output_index).get_color_output(1).view, frame_data.linear_sampler);
				CloudPass::write_sampled_set(&in_state.vk,
					CloudPass::pass.shadow_sets.current(&in_state.vk), cloud_params_buffer,
					CloudPass::pass.base_shape.view, CloudPass::pass.repeat_sampler,
					CloudPass::pass.erosion.view, CloudPass::pass.repeat_sampler,
					CloudPass::pass.weather.view, CloudPass::pass.repeat_sampler,
					geometry_render_pass.get_color_output(1).view, frame_data.linear_sampler);
			}
			else
			{
				CloudPass::pass.history_valid = false;
			}
			CloudPass::initialize_shadow(graph, &in_state.vk,
				cloud_shadow_render_pass.get_color_output(0));
			FrameGraphImage pre_fog_scene_color = frame_graph_select(cloud_render_active,
				frame_graph_color(cloud_composite_render_pass),
				frame_graph_color(lighting_render_pass));
			FrameGraphImage pre_fog_position = frame_graph_select(cloud_render_active,
				frame_graph_color(cloud_composite_render_pass, 1),
				frame_graph_color(geometry_render_pass, 1));
			// Height fog runs when an enabled fog controller exists; downstream
			// passes read its output instead of the lighting target
			RenderPass& fog_render_pass = get_render_target(RenderTargetId::Fog);
			const bool fog_render_active = in_state.fog.active && in_state.fog.debug_active
				&& in_state.fog.active_fog_controller_id.has_value()
				&& in_state.scene.objects.contains(*in_state.fog.active_fog_controller_id);
			if (fog_render_active)
			{
				const FogController& fog_controller = in_state.scene.objects[*in_state.fog.active_fog_controller_id].fog_controller;
				FogFsParams fog_fs_params = {
					.camera_position = camera.location,
					.fog_base_height = fog_controller.base_height,
					.fog_color = fog_controller.fog_color,
					.density = fog_controller.density,
					.scale_height = fog_controller.scale_height,
					.max_distance = fog_controller.max_distance,
					.ceiling_enabled = fog_controller.ceiling_enabled ? 1 : 0,
					.ceiling_height = fog_controller.ceiling_height,
					.ceiling_fade = fog_controller.ceiling_fade,
					.ambient_intensity = fog_controller.ambient_intensity,
					.sun_intensity = fog_controller.sun_intensity,
					.anisotropy = fog_controller.anisotropy,
					// No real sun means no sun in-scatter; the PerFrameData fallback
					// sun drives only the sky.
					.sun_direction = in_state.scene.primary_sun_id ? per_frame_data.sun_direction.XYZ : HMM_V3(0.0f, 0.0f, -1.0f),
					.sun_color = in_state.scene.primary_sun_id ? per_frame_data.sun_color.XYZ : HMM_V3(0.0f, 0.0f, 0.0f),
					.atmosphere_enabled = sky_pass.has_active_atmosphere
						&& bruneton_atmosphere_pass.has_precomputed ? 1 : 0,
					.atmosphere_planet_center_z = sky_pass.active_parameters.planet_center_z_m,
				};
				fog_pass_update(
					&in_state.vk,
					fog_fs_params,
					pre_fog_scene_color.view(),
					pre_fog_position.view(),
					bruneton_atmosphere_pass.parameter_buffers[in_state.vk.frame_index].get_gpu_buffer(),
					bruneton_atmosphere_pass.transmittance_pass.get_color_output(0).view
				);
			}
		
			FrameGraphImage post_fog_scene_color = frame_graph_select(fog_render_active,
				frame_graph_color(fog_render_pass), pre_fog_scene_color);
		
			// DOF gathers from the post-fog color; tonemapping reads whichever pass
			// ran last
			RenderPass& dof_combine_render_pass = get_render_target(RenderTargetId::DofCombine);
			if (in_state.dof.enable)
			{
				DofCombineFsParams dof_combine_fs_params = {
					.cam_pos = HMM_V4V(camera.location, 1.0f),
					.cam_forward = HMM_V4V(camera.forward, 0.0f),
					.screen_size = HMM_V2((f32) in_state.window.render_width, (f32) in_state.window.render_height),
					.focus_distance = in_state.dof.focus_distance,
					.focus_range = in_state.dof.focus_range,
					.max_coc_radius = in_state.dof.max_coc_radius,
					.foreground_blur_scale = in_state.dof.foreground_blur_scale,
					.background_blur_scale = in_state.dof.background_blur_scale,
					.debug_mode = in_state.dof.debug_show_coc ? 1 : 0,
				};
				dof_combine_pass_update(
					&in_state.vk,
					dof_combine_fs_params,
					post_fog_scene_color.view(),
					pre_fog_position.view()
				);
			}
		
			FrameGraphImage post_dof_scene_color = frame_graph_select(in_state.dof.enable,
				frame_graph_color(dof_combine_render_pass), post_fog_scene_color);
		
			// Shaded wireframe copies the post-DOF color and blends wires on top
			RenderPass& wire_overlay_render_pass = get_render_target(RenderTargetId::WireOverlay);
			if (in_state.wireframe.shaded_wireframe)
			{
				WireOverlayMeshFsParams wire_fs_params = {
					.color = in_state.wireframe.color,
					.camera_position = HMM_V4V(camera.location, 1.0f),
					.camera_forward = HMM_V4V(HMM_NormV3(camera.forward), 0.0f),
					.screen_size = HMM_V2((f32) in_state.window.render_width, (f32) in_state.window.render_height),
					.width = in_state.wireframe.width,
					.softness = in_state.wireframe.softness,
					.opacity = in_state.wireframe.opacity,
					.visibility_tolerance = in_state.wireframe.visibility_tolerance,
				};
				WireOverlayPass::update(
					&in_state.vk,
					wire_fs_params,
					post_dof_scene_color.view(),
					geometry_render_pass.get_color_output(1).view
				);
			}
		
			FrameGraphImage post_wire_scene_color = frame_graph_select(
				in_state.wireframe.shaded_wireframe,
				frame_graph_color(wire_overlay_render_pass), post_dof_scene_color);
		
			// TAA ping-pong: write into set history_index, read the other set's
			// history output
			const i32 temporal_aa_output_index = in_state.temporal_aa.history_index;
			const i32 temporal_aa_previous_index = (temporal_aa_output_index + 1) % 2;
			const auto get_temporal_aa_pass = [&](i32 in_set_idx) -> RenderPass& {
				return get_render_target(in_set_idx == 0
					? RenderTargetId::TemporalAAHistory0 : RenderTargetId::TemporalAAHistory1);
			};
			if (in_state.temporal_aa.enable)
			{
				TemporalAaFsParams temporal_aa_fs_params = {
					.previous_view_projection = in_state.temporal_aa.previous_view_projection,
					.screen_size = HMM_V2((f32) in_state.window.render_width, (f32) in_state.window.render_height),
					.sharpen_axis = (in_state.temporal_aa.jitter_phase & 1) == 1 ? HMM_V2(1.0f, 0.0f) : HMM_V2(0.0f, 1.0f),
					.blend_alpha = in_state.temporal_aa.blend_alpha,
					.sharpen_strength = in_state.temporal_aa.sharpen_strength,
					.rejection_threshold = in_state.temporal_aa.rejection_threshold,
					.history_valid = in_state.temporal_aa.history_valid ? 1 : 0,
					.debug_mode = in_state.temporal_aa.debug_mode,
					.reactive_enable = cloud_render_active ? 1 : 0,
				};
				TemporalAAPass::update(
					&in_state.vk,
					temporal_aa_fs_params,
					post_wire_scene_color.view(),
					pre_fog_position.view(),
					get_temporal_aa_pass(temporal_aa_previous_index).get_color_output(1).view,
					cloud_render_active
						? cloud_composite_render_pass.get_color_output(2).view
						: geometry_render_pass.get_color_output(0).view
				);
			}
		
			FrameGraphImage pre_tonemap_scene_color = frame_graph_select(
				in_state.temporal_aa.enable,
				frame_graph_color(get_temporal_aa_pass(temporal_aa_output_index)),
				post_wire_scene_color);
			const bool bloom_active = in_state.bloom.enable && in_state.bloom.intensity > 0.0f;
			const f32 bloom_intensity = bloom_active
				? CLAMP(in_state.bloom.intensity, 0.0f, State::BloomState::MAX_INTENSITY)
				: 0.0f;
			tonemapping_pass_update(
				&in_state.vk,
				pre_tonemap_scene_color,
				frame_graph_color(geometry_render_pass, 1),
				frame_graph_mip(BloomPass::get_pyramid(), 0),
				bloom_intensity,
				frame_data.linear_sampler,
				AutoAdaptationPass::state_buffer());
		
			// FXAA reads the tonemapped target. Presentation composite upscales the
			// selected result at output resolution before UI and display encoding.
			RenderPass& fxaa_render_pass = get_render_target(RenderTargetId::FXAA);
			const bool fxaa_active = in_state.temporal_aa.enable_fxaa;
			if (fxaa_active)
			{
				FXAAPass::update(&in_state.vk, tonemapping_render_pass.get_color_output(0).view);
			}
			const bool show_cloud_shadow_fullscreen =
				in_state.clouds.debug_show_shadow_map_fullscreen;
			FrameGraphImage presentation_input = show_cloud_shadow_fullscreen
				? frame_graph_color(cloud_shadow_render_pass)
				: in_state.images.enable_debug_fullscreen && in_state.images.items.length() > 0
					? frame_graph_image(in_state.images.items[CLAMP(
						in_state.images.debug_index, 0, (i32) in_state.images.items.length() - 1)])
					: frame_graph_select(fxaa_active, frame_graph_color(fxaa_render_pass),
						frame_graph_color(tonemapping_render_pass));
			copy_to_swapchain_pass_update_presentation_input(
				&in_state.vk, presentation_input.view());
			frame_data_write_copy_input(
				&in_state.vk,
				get_render_target(RenderTargetId::PresentationComposite).get_color_output(0).view);
			// Incrementally capture and project GI probes after GPU skinning and the
			// atmosphere update, before the main pass chain samples the probe atlas.
			{
				CPU_TIMING_SCOPE("GI Scene Update");
				gi_scene_update(graph, &in_state.vk, g_gi_scene, in_state);
			}
		
			// Cascade matrices are CPU-side inputs to both the shadow draw and the
			// lighting shader's receiver reprojection — compute before the fs_params
			// upload below. The centered-squares anchor tracks the camera unless the
			// shadow map is frozen.
			if (!in_state.shadow.depth_freeze)
			{
				in_state.shadow.centered_square_center = camera.location
					+ HMM_NormV3(camera.forward) * in_state.shadow.centered_square_lookahead_distance;
			}
			const bool shadow_map_updated = ShadowDepthPass::compute_cascade_matrices(in_state, camera);
			in_state.shadow.force_recapture = false;
		
			// Lighting fs_params: direct, shadow, post-occlusion, and probe GI in_state.
			LightingFsParams lighting_fs_params = {};
			lighting_fs_params.view_position = camera.location;
			lighting_fs_params.view_forward = camera.forward;
			lighting_fs_params.num_point_lights = (i32) in_state.lighting.point_lights.length();
			lighting_fs_params.num_spot_lights = (i32) in_state.lighting.spot_lights.length();
			lighting_fs_params.num_sun_lights = (i32) in_state.lighting.sun_lights.length();
			lighting_fs_params.direct_lighting_enable = in_state.lighting.direct_enable ? 1 : 0;
			lighting_fs_params.ssao_enable = in_state.ssao.enable ? 1 : 0;
			lighting_fs_params.gi_enable = in_state.gi.enable ? 1 : 0;
			lighting_fs_params.gi_probe_occlusion = in_state.gi.probe_occlusion ? 1 : 0;
			lighting_fs_params.probe_occlusion_mode = (i32) in_state.gi.probe_occlusion_mode;
			lighting_fs_params.probe_radiance_mode = (i32) in_state.gi.probe_radiance_mode;
			lighting_fs_params.probe_specular_enable = in_state.gi.probe_specular_enable ? 1 : 0;
			lighting_fs_params.specular_atlas_total_size = g_gi_scene.lighting_capture.specular_atlas_total_size;
			lighting_fs_params.specular_atlas_entry_size = g_gi_scene.lighting_capture.desc.specular_entry_size;
			lighting_fs_params.specular_mip_count = g_gi_scene.lighting_capture.desc.specular_mip_count;
			lighting_fs_params.gi_intensity = in_state.gi.intensity;
			lighting_fs_params.atlas_total_size = GI_Scene::atlas_total_size;
			lighting_fs_params.atlas_entry_size = GI_Scene::atlas_entry_size;
			lighting_fs_params.gi_fallback_probe_index = g_gi_scene.fallback_probe_index;
			lighting_fs_params.gi_octree_node_count = (i32) g_gi_scene.octree_nodes.length();
			lighting_fs_params.isolated_probe_index = in_state.gi.probe_isolation_enable
				? (in_state.gi.isolated_probe_index >= 0 ? in_state.gi.isolated_probe_index : -2)
				: -1;
			lighting_fs_params.shadow_bias = in_state.shadow.shadow_bias;
			lighting_fs_params.shadow_map_texel_size = HMM_V2(
				1.0f / (f32) ShadowDepthPass::ShadowMapResolution,
				1.0f / (f32) ShadowDepthPass::ShadowMapResolution
			);
			if (ShadowDepthPass::has_valid_shadow_map)
			{
				lighting_fs_params.shadow_map_enable = 1;
				lighting_fs_params.shadow_num_cascades = ShadowDepthPass::get_active_cascade_count(in_state);
				lighting_fs_params.shadow_cascade_placement_mode = (i32) in_state.shadow.cascade_placement_mode;
				lighting_fs_params.shadow_debug_show_cascade_selection = in_state.shadow.debug_show_cascade_selection ? 1 : 0;
				lighting_fs_params.shadow_cascade_distances = HMM_V4(
					ShadowDepthPass::cascade_distances[0],
					ShadowDepthPass::cascade_distances[1],
					ShadowDepthPass::cascade_distances[2],
					ShadowDepthPass::cascade_distances[3]
				);
				lighting_fs_params.shadow_cascade_view_position = ShadowDepthPass::cascade_view_position;
				lighting_fs_params.shadow_cascade_view_forward = ShadowDepthPass::cascade_view_forward;
				for (i32 cascade_idx = 0; cascade_idx < MAX_SHADOW_CASCADES; ++cascade_idx)
				{
					lighting_fs_params.shadow_view_projections[cascade_idx] = ShadowDepthPass::shadow_view_projections[cascade_idx];
				}
			}
		
			RenderPass& shadow_render_pass = get_render_target(RenderTargetId::ShadowDepth);
			RenderPass& shadow_blur_horizontal = get_render_target(RenderTargetId::ShadowBlurHorizontal);
			RenderPass& shadow_blurred = get_render_target(RenderTargetId::ShadowBlurred);
			RenderPass& ssao_render_pass = get_render_target(RenderTargetId::SSAO);
			RenderPass& ssao_blur_horizontal = get_render_target(RenderTargetId::SSAOBlurHorizontal);
			RenderPass& ssao_blurred = get_render_target(RenderTargetId::SSAOBlurred);
			RenderPass& screen_space_shadow_trace = get_render_target(RenderTargetId::ScreenSpaceShadowTrace);
			RenderPass& screen_space_shadows = get_render_target(RenderTargetId::ScreenSpaceShadows);
		
			// Screen-space contact shadows trace toward the shadow-casting sun
			Object* screen_space_shadow_sun = in_state.shadow.rendering_enable && in_state.shadow.screen_space.enable
				? ShadowDepthPass::get_valid_shadow_sun(in_state)
				: nullptr;
			const bool screen_space_shadows_valid = screen_space_shadow_sun != nullptr;
			if (screen_space_shadows_valid)
			{
				lighting_fs_params.screen_space_shadows_enable = 1;
				lighting_fs_params.screen_space_shadow_intensity = in_state.shadow.screen_space.intensity;
			}
			lighting_fs_params.atmosphere_sun_index =
				in_state.lighting.active_atmosphere_sun_index;
			lighting_fs_params.atmosphere_enabled =
				sky_pass.has_active_atmosphere
				&& bruneton_atmosphere_pass.has_precomputed
				&& in_state.lighting.active_atmosphere_sun_index >= 0 ? 1 : 0;
			lighting_fs_params.atmosphere_planet_center_z =
				sky_pass.active_parameters.planet_center_z_m;
			lighting_fs_params.cloud_shadow_extent_enabled = HMM_V4(
				cloud_shadow_lighting_active
					? in_state.scene.objects[*in_state.scene.active_cloud_controller_id]
						.cloud_system.shadow_extent_m
					: 8000.0f,
				cloud_shadow_lighting_active ? 1.0f : 0.0f,
				0.0f, 0.0f);
			// Lighting samples the blurred moments (soft penumbra) unless the blur is
			// disabled; blur is enabled by default.
			VkImageView shadow_moments_view = in_state.shadow.blur_enable
				? shadow_blurred.get_color_output(0).view
				: shadow_render_pass.get_color_output(0).view;
			lighting_pass_update(
				&in_state.vk,
				lighting_fs_params,
				geometry_render_pass.color_outputs.data(),
				shadow_moments_view,
				ssao_blurred.get_color_output(0).view,
				screen_space_shadows.get_color_output(0).view,
				in_state.lighting.point_buffers[in_state.lighting.buffer_index].get_gpu_buffer(),
				in_state.lighting.spot_buffers[in_state.lighting.buffer_index].get_gpu_buffer(),
				in_state.lighting.sun_buffers[in_state.lighting.buffer_index].get_gpu_buffer(),
				g_gi_scene.probes_buffer.get_gpu_buffer(),
				g_gi_scene.cells_buffer.get_gpu_buffer(),
				gi_scene_get_octahedral_lighting_view(g_gi_scene),
				gi_scene_get_octahedral_depth_view(g_gi_scene),
				g_gi_scene.sh9_coefficients_buffer.get_gpu_buffer(),
				g_gi_scene.sg9_lobes_buffer.get_gpu_buffer(),
				g_gi_scene.octree_nodes_buffer.get_gpu_buffer(),
				g_gi_scene.lighting_capture.specular_sampler,
				gi_scene_get_specular_lighting_view(g_gi_scene),
				gi_scene_get_brdf_lut_view(g_gi_scene),
				bruneton_atmosphere_pass.parameter_buffers[in_state.vk.frame_index].get_gpu_buffer(),
				bruneton_atmosphere_pass.has_precomputed
					? bruneton_atmosphere_pass.transmittance_pass.get_color_output(0).view
					: cloud_shadow_render_pass.get_color_output(0).view,
				cloud_shadow_render_pass.get_color_output(0).view
			);
		
			ssao_pass_update(
				&in_state.vk,
				HMM_V2((f32) ssao_render_pass.current_width, (f32) ssao_render_pass.current_height),
				view_matrix,
				projection_matrix,
				in_state.ssao.enable,
				geometry_render_pass.get_color_output(1).view,	// world position
				geometry_render_pass.get_color_output(2).view,	// world normal
				ssao_render_pass.get_color_output(0).view,
				ssao_blur_horizontal.get_color_output(0).view
			);
		
			{
				RenderPass& screen_space_trace_pass = screen_space_shadow_trace;
				const HMM_Vec3 screen_space_sun_dir = screen_space_shadows_valid
					? HMM_NormV3(HMM_RotateV3Q(HMM_V3(0.0f, 0.0f, -1.0f), screen_space_shadow_sun->current_transform.rotation))
					: HMM_V3(0.0f, 0.0f, -1.0f);
				ScreenSpaceShadowsPass::update(
					&in_state.vk,
					in_state,
					HMM_V2((f32) screen_space_trace_pass.current_width, (f32) screen_space_trace_pass.current_height),
					view_matrix,
					projection_matrix,
					screen_space_sun_dir,
					screen_space_shadows_valid,
					geometry_render_pass.get_color_output(1).view,	// world position
					geometry_render_pass.get_color_output(2).view,	// world normal
					screen_space_trace_pass.get_color_output(0).view
				);
			}
		
			// Shadow cascades: one moments slice per active cascade. Skipped entirely
			// without a valid shadow sun; under depth_freeze the stale map remains a
			// persistent graph resource sampled with its frozen matrices.
			if (shadow_map_updated)
			{
				shadow_render_pass.execute(&in_state.vk, [&](i32 in_cascade_idx)
				{
					ShadowDepthPass::render_cascade(&in_state.vk, in_state, in_cascade_idx);
				}, ShadowDepthPass::get_active_cascade_count(in_state));
			}
			// Separable blur over the moments — the source of EVSM's soft penumbra.
			// Runs only when the shadow map re-rendered; graph reads transition stale
			// or freshly rendered targets when a consumer actually selects them.
			if (shadow_map_updated && in_state.shadow.blur_enable)
			{
				graph.make_sampled(frame_graph_color(shadow_render_pass));
				ShadowBlurPass::execute_separable(
					&in_state.vk,
					shadow_blur_horizontal,
					shadow_blurred,
					ShadowDepthPass::get_active_cascade_count(in_state)
				);
			}
			in_state.shadow.debug_cascade_index = CLAMP(
				in_state.shadow.debug_cascade_index,
				0,
				MAX(0, ShadowDepthPass::get_active_cascade_count(in_state) - 1)
			);
			RenderPass& shadow_debug_pass = get_render_target(RenderTargetId::ShadowCascadeDebug);
			graph.sampled(frame_graph_color(in_state.shadow.blur_enable
				? shadow_blurred : shadow_render_pass));
			graph.execute(shadow_debug_pass, [&](i32)
			{
				ShadowCascadeDebugPass::render(
					&in_state.vk,
					shadow_moments_view,
					frame_data.linear_sampler,
					in_state.shadow.debug_cascade_index,
					in_state.shadow.debug_view_mode
				);
			});
			graph.make_sampled(frame_graph_color(shadow_debug_pass));
		
			// Geometry: scene meshes -> G-buffer at render resolution, camera-frustum
			// culled on the CPU; skinned meshes bypass the frustum test.
			graph.execute(geometry_render_pass, [&](i32)
			{
				geometry_pass_bind(&in_state.vk);
		
				if (in_state.render_objects.valid)
				{
					CullResult cull_result = cull_objects(in_state, view_projection_matrix,
						in_state.tessellation.enabled ? in_state.tessellation.bounds_padding : 0.0f);
					for (i32 mesh_object_id : cull_result.object_ids)
					{
						auto found = in_state.scene.objects.find(mesh_object_id);
						if (found == in_state.scene.objects.end())
						{
							continue;
						}
		
						Object& object = found->second;
						if (object.render_object_index >= 0)
						{
							geometry_pass_draw_mesh(&in_state.vk, object.mesh, object.render_object_index, in_state.animation.skinning_debug_view);
							in_state.data_oriented.frame.draw_calls += 1;
							in_state.data_oriented.frame.draw_mesh_count += 1;
						}
					}
				}
		
				// Sky composite fills the background at the far plane
				GIDebugPass::draw(&in_state.vk, g_gi_scene, in_state, view_projection_matrix);
		
				// Sky composite fills the background at the far plane
				if (in_state.sky.rendering_enable)
				{
					sky_pass_draw_composite(&in_state.vk);
				}
			});
			// SSAO reads G-buffer position/normal (already SHADER_READ_ONLY), then
			// its raw output is blurred; lighting samples the blurred result
			graph.sampled(frame_graph_color(geometry_render_pass, 1));
			graph.sampled(frame_graph_color(geometry_render_pass, 2));
			graph.execute(ssao_render_pass, [&](i32)
			{
				ssao_pass_draw(&in_state.vk);
			});
			graph.make_sampled(frame_graph_color(ssao_render_pass));
			BlurPass::execute_separable(
				&in_state.vk,
				ssao_blur_horizontal,
				ssao_blurred,
				ssao_pass.blur_horizontal_sets.current(&in_state.vk),
				ssao_pass.blur_vertical_sets.current(&in_state.vk),
				4
			);
		
			// Contact shadows trace + filter (skipped without a shadow sun; the
			// transition keeps the bound mask image legal — the lighting shader only
			// samples it when screen_space_shadows_enable is set)
			if (screen_space_shadows_valid)
			{
				graph.make_sampled(frame_graph_color(geometry_render_pass, 1));
				graph.make_sampled(frame_graph_color(geometry_render_pass, 2));
				ScreenSpaceShadowsPass::execute(
					&in_state.vk, screen_space_shadow_trace, screen_space_shadows);
			}
			// Update one interleaved quarter before deferred lighting so opaque
			// surfaces receive this frame's cloud transmittance.
			if (cloud_shadow_render_active)
			{
				CloudPass::record_shadow_subgraph(
					graph, &in_state.vk, cloud_shadow_render_pass,
					frame_graph_color(geometry_render_pass, 1), cloud_params_buffer);
			}
		
			for (i32 output_index = 0; output_index < geometry_render_pass.desc.num_outputs; ++output_index)
			{
				graph.sampled(frame_graph_color(geometry_render_pass, output_index));
			}
			graph.sampled(frame_graph_color(in_state.shadow.blur_enable
				? shadow_blurred : shadow_render_pass));
			graph.sampled(frame_graph_color(ssao_blurred));
			graph.sampled(frame_graph_color(screen_space_shadows));
			graph.sampled(frame_graph_color(cloud_shadow_render_pass));
			graph.execute(lighting_render_pass, [&](i32)
			{
				lighting_pass_draw(&in_state.vk);
			});

			if (cloud_render_active)
			{
				const i32 cloud_timing = gpu_timestamps_begin_scope(&in_state.vk, "Cloud System");
				vulkan_begin_debug_label(&in_state.vk, "Cloud System");
				RenderPass& cloud_history_target = get_cloud_history_pass(cloud_history_output_index);
				CloudPass::record_main_subgraph(
					graph, &in_state.vk, cloud_raymarch_render_pass,
					get_cloud_history_pass(cloud_history_previous_index),
					cloud_history_target, lighting_render_pass,
					frame_graph_color(geometry_render_pass, 1),
					cloud_composite_render_pass, cloud_params_buffer);
				CloudPass::pass.history_valid = true;
				CloudPass::pass.history_index = cloud_history_previous_index;
				gpu_timestamps_end_scope(&in_state.vk, cloud_timing);
				vulkan_end_debug_label(&in_state.vk);
			}
		
			// Fog reads the lit scene + G-buffer position; tonemapping then reads the
			// post-fog color (or the lighting output directly when fog is off)
			if (fog_render_active)
			{
				graph.sampled(pre_fog_scene_color);
				graph.sampled(pre_fog_position);
				graph.execute(fog_render_pass, [&](i32)
				{
					fog_pass_draw(&in_state.vk);
				});
			}
			if (in_state.dof.enable)
			{
				graph.sampled(post_fog_scene_color);
				graph.sampled(pre_fog_position);
				graph.execute(dof_combine_render_pass, [&](i32)
				{
					dof_combine_pass_draw(&in_state.vk);
				});
			}
			if (in_state.wireframe.shaded_wireframe)
			{
				graph.sampled(post_dof_scene_color);
				graph.sampled(frame_graph_color(geometry_render_pass, 1));
				graph.execute(wire_overlay_render_pass, [&](i32)
				{
					WireOverlayPass::draw(&in_state.vk, in_state, view_projection_matrix);
				});
			}
		
			if (in_state.temporal_aa.enable)
			{
				RenderPass& temporal_aa_target_pass = get_temporal_aa_pass(temporal_aa_output_index);
				graph.sampled(post_wire_scene_color);
				graph.sampled(pre_fog_position);
				graph.sampled(frame_graph_color(
					get_temporal_aa_pass(temporal_aa_previous_index), 1));
				graph.sampled(cloud_render_active
					? frame_graph_color(cloud_composite_render_pass, 2)
					: frame_graph_color(geometry_render_pass, 0));
				graph.execute(temporal_aa_target_pass, [&](i32)
				{
					TemporalAAPass::draw(&in_state.vk);
				});
		
				in_state.temporal_aa.previous_view_projection = view_projection_matrix;
				in_state.temporal_aa.history_valid = true;
				in_state.temporal_aa.history_index = temporal_aa_previous_index;
			}

			AutoAdaptationPass::SolarGuardPushConstants solar_guard = {};
			if (sky_pass.has_active_atmosphere
				&& bruneton_atmosphere_pass.has_precomputed)
			{
				const HMM_Vec4 sun_clip = HMM_MulM4V4(
					view_projection_matrix,
					HMM_V4V(sky_pass.active_sun_direction, 0.0f));
				const bool sun_in_front = std::isfinite(sun_clip.W)
					&& sun_clip.W > 1.0e-6f;
				HMM_Vec2 sun_uv = HMM_V2(-1.0f, -1.0f);
				if (sun_in_front)
				{
					const HMM_Vec2 sun_ndc = HMM_V2(
						sun_clip.X / sun_clip.W, sun_clip.Y / sun_clip.W);
					sun_uv = HMM_V2(
						sun_ndc.X * 0.5f + 0.5f,
						0.5f - sun_ndc.Y * 0.5f);
				}
				const f32 angular_radius = HMM_AngleDeg(
					sky_pass.active_parameters.sun_disc_angular_diameter_degrees * 0.5f);
				const f32 projected_radius_pixels =
					0.5f * (f32)in_state.window.render_height
					* tanf(angular_radius) / tanf(fov * 0.5f);
				solar_guard.sun_screen_uv_radius_enabled = HMM_V4(
					sun_uv.X, sun_uv.Y, MAX(projected_radius_pixels, 1.0f),
					sun_in_front ? 1.0f : 0.0f);
				solar_guard.camera_position_and_planet_center = HMM_V4V(
					camera.location, sky_pass.active_parameters.planet_center_z_m);
				solar_guard.sun_direction_and_disc_intensity = HMM_V4V(
					sky_pass.active_sun_direction,
					sky_pass.active_parameters.sun_disc_intensity);
				solar_guard.sun_tint_and_irradiance = HMM_V4V(
					sky_pass.active_sun_tint,
					sky_pass.active_sun_irradiance_w_m2);
			}
			AutoAdaptationPass::meter(
				graph, &in_state.vk, pre_tonemap_scene_color,
				frame_graph_color(geometry_render_pass, 1),
				in_state.tonemapping, solar_guard);

			// Bloom reconstructs an exposure-aware HDR pyramid after the temporal
			// resolve. Local tonemapping still derives its guide from the original
			// scene so the glow cannot suppress itself through local adaptation.
			if (bloom_active)
			{
				BloomPass::execute(
					graph, &in_state.vk,
					pre_tonemap_scene_color,
					in_state.bloom,
					in_state.tonemapping,
					AutoAdaptationPass::state_buffer());
			}
		
			if (in_state.tonemapping.local_enabled)
			{
				tonemapping_pass_prepare_local(
					graph, &in_state.vk, in_state.tonemapping);
			#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
				if (in_state.debug_ui.show_local_tonemapping_debug)
				{
					tonemapping_pass_prepare_local_debug(
						graph, &in_state.vk,
						in_state.tonemapping,
						in_state.debug_ui.local_tonemapping_debug_mip);
				}
			#endif
			}
			tonemapping_pass_declare_final_resources(graph, in_state.tonemapping);
			graph.execute(tonemapping_render_pass, [&](i32)
			{
				tonemapping_pass_draw(
					&in_state.vk,
					in_state.tonemapping,
					bloom_intensity,
					BloomPass::get_profile_base_gain(),
					in_state.bloom.auto_exposure_influence);
			});
			AutoAdaptationPass::update_after_tonemapping(
				graph, &in_state.vk, in_state.tonemapping, in_delta_time);
		
			// FXAA filters at render resolution. Presentation composite performs the
			// one upscale and places UI into the same normalized float target.
			if (fxaa_active)
			{
				graph.sampled(frame_graph_color(tonemapping_render_pass));
				graph.execute(fxaa_render_pass, [&](i32)
				{
					FXAAPass::draw(&in_state.vk, HMM_V2((f32) in_state.window.render_width, (f32) in_state.window.render_height));
				});
			}
			graph.sampled(presentation_input);
			graph.sampled(frame_graph_color(tonemapping_render_pass));
			graph.execute(get_render_target(RenderTargetId::PresentationComposite), [&](i32)
			{
				copy_to_swapchain_pass_draw_presentation(
					&in_state.vk, show_cloud_shadow_fullscreen);
				ImGuiLayer::draw_local_tonemapping_debug(
					in_state,
					tonemapping_render_pass.get_color_output(0).view);
				const f32 paper_white_scale = in_state.vk.active_output_mode == EDisplayOutputMode::SDR
					? 1.0f
					: GT7Tonemapping::HDR_PAPER_WHITE_NITS / GT7Tonemapping::HDR_PEAK_NITS;
				ImGuiLayer::render(&in_state.vk, paper_white_scale);
			});
			graph.sampled(frame_graph_color(
				get_render_target(RenderTargetId::PresentationComposite)));
			graph.execute(get_render_target(RenderTargetId::Swapchain), [&](i32)
			{
				copy_to_swapchain_pass_draw(&in_state.vk);
			});
	}

	inline void end_frame(State& in_state)
	{
		vulkan_context_end_frame(&in_state.vk);
	}

	inline void shutdown(State& in_state)
	{
		ImGuiLayer::shutdown();
		GIDebugPass::shutdown(&in_state.vk);
		gi_scene_cleanup(&in_state.vk, g_gi_scene);

		in_state.render_targets.cleanup();

		in_state.render_objects.shutdown();
		in_state.skin_matrices.shutdown();
		in_state.materials.buffer.destroy_gpu_buffer();
		for (i32 buffer_idx = 0; buffer_idx < RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT; ++buffer_idx)
		{
			in_state.lighting.point_buffers[buffer_idx].destroy_gpu_buffer();
			in_state.lighting.spot_buffers[buffer_idx].destroy_gpu_buffer();
			in_state.lighting.sun_buffers[buffer_idx].destroy_gpu_buffer();
		}

		copy_to_swapchain_pass_shutdown(&in_state.vk);
		CloudPass::shutdown(&in_state.vk);
		sky_pass_shutdown(&in_state.vk);
		tonemapping_pass_shutdown(&in_state.vk);
		AutoAdaptationPass::shutdown(&in_state.vk);
		BloomPass::shutdown(&in_state.vk);
		Tessellation::shutdown(&in_state.vk);
		GpuSkinning::shutdown(&in_state.vk);
		FXAAPass::shutdown(&in_state.vk);
		TemporalAAPass::shutdown(&in_state.vk);
		WireOverlayPass::shutdown(&in_state.vk);
		dof_combine_pass_shutdown(&in_state.vk);
		fog_pass_shutdown(&in_state.vk);
		lighting_pass_shutdown(&in_state.vk);
		ScreenSpaceShadowsPass::shutdown(&in_state.vk);
		BlurPass::shutdown(&in_state.vk);
		ssao_pass_shutdown(&in_state.vk);
		ShadowBlurPass::shutdown(&in_state.vk);
		ShadowCascadeDebugPass::shutdown(&in_state.vk);
		ShadowDepthPass::shutdown(&in_state.vk);
		geometry_pass_shutdown(&in_state.vk);
		frame_data_shutdown(&in_state.vk);
		vulkan_context_shutdown(&in_state.vk);
	}
}
