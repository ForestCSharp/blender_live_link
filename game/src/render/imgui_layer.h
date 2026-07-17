#pragma once

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI

#include "ankerl/unordered_dense.h"
#include "imgui.h"
#include "extern/imgui/backends/imgui_impl_glfw.h"
#include "extern/imgui/backends/imgui_impl_vulkan.h"
#include "ui/stats_ui.h"
#include "ui/cpu_profiler_ui.h"

namespace ImGuiLayer
{
	inline bool initialized = false;
	struct TextureKey
	{
		VkSampler sampler = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		bool operator==(const TextureKey&) const = default;
	};
	struct TextureKeyHash
	{
		using is_avalanching = void;
		u64 operator()(const TextureKey& key) const
		{
			const u64 sampler = (u64) (uintptr_t) key.sampler;
			const u64 view = (u64) (uintptr_t) key.view;
			return ankerl::unordered_dense::hash<u64>{}(view ^ (sampler + 0x9e3779b97f4a7c15ULL + (view << 6) + (view >> 2)));
		}
	};
	inline ankerl::unordered_dense::map<TextureKey, VkDescriptorSet, TextureKeyHash> texture_sets;

	inline void check_vk_result(VkResult result)
	{
		if (result != VK_SUCCESS) { printf("ImGui Vulkan error: %d\n", result); }
	}

	inline void init(VulkanContext* ctx)
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::StyleColorsDark();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = "bin/imgui.ini";
		ImGui_ImplGlfw_InitForVulkan(ctx->window, false);

		VkFormat color_format = ctx->surface_format.format;
		VkPipelineRenderingCreateInfo rendering_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = 1,
			.pColorAttachmentFormats = &color_format,
		};
		ImGui_ImplVulkan_InitInfo init_info = {};
		init_info.ApiVersion = VK_API_VERSION_1_3;
		init_info.Instance = ctx->instance;
		init_info.PhysicalDevice = ctx->physical_device;
		init_info.Device = ctx->device;
		init_info.QueueFamily = ctx->graphics_queue_family_index;
		init_info.Queue = ctx->graphics_queue;
		init_info.DescriptorPoolSize = 512;
		init_info.MinImageCount = ctx->swapchain_min_image_count;
		init_info.ImageCount = ctx->swapchain_image_count;
		init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
		init_info.UseDynamicRendering = true;
		init_info.PipelineRenderingCreateInfo = rendering_info;
		init_info.CheckVkResultFn = check_vk_result;
		ImGui_ImplVulkan_Init(&init_info);
		initialized = true;
	}

	inline void clear_textures()
	{
		if (!initialized) { return; }
		for (const auto& [key, set] : texture_sets)
		{
			ImGui_ImplVulkan_RemoveTexture(set);
		}
		texture_sets.clear();
	}

	inline void handle_swapchain_recreated(VulkanContext* ctx)
	{
		if (initialized)
		{
			ImGui_ImplVulkan_SetMinImageCount(ctx->swapchain_min_image_count);
		}
	}

	inline void unregister_texture(VkImageView view)
	{
		if (!initialized || view == VK_NULL_HANDLE) { return; }
		for (auto it = texture_sets.begin(); it != texture_sets.end();)
		{
			if (it->first.view == view)
			{
				ImGui_ImplVulkan_RemoveTexture(it->second);
				it = texture_sets.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	inline ImTextureRef texture(VkSampler sampler, VkImageView view)
	{
		if (view == VK_NULL_HANDLE) { return ImTextureRef(); }
		const TextureKey key = { .sampler = sampler, .view = view };
		auto found = texture_sets.find(key);
		if (found == texture_sets.end())
		{
			VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			found = texture_sets.emplace(key, set).first;
		}
		return ImTextureRef((ImTextureID) (uintptr_t) found->second);
	}

	inline void begin_frame()
	{
		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	}

	inline ImVec2 image_size(const GpuImage& image, f32 max_size)
	{
		const f32 aspect = image.extent.height > 0 ? (f32) image.extent.width / (f32) image.extent.height : 1.0f;
		return aspect >= 1.0f ? ImVec2(max_size, max_size / aspect) : ImVec2(max_size * aspect, max_size);
	}

	inline void draw_texture(VkSampler sampler, const char* label, const GpuImage& image, f32 max_size = 240.0f, VkImageView override_view = VK_NULL_HANDLE)
	{
		ImGui::TextUnformatted(label);
		ImGui::Image(texture(sampler, override_view != VK_NULL_HANDLE ? override_view : image.view), image_size(image, max_size));
	}

	inline void draw_controls(State& state, GI_Scene& gi_scene)
	{
		if (!state.debug_ui.visible) { return; }
		ImGui::Begin("DEBUG");
		if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Text("Window Resolution: %d x %d", state.window.width, state.window.height);
			ImGui::Text("Render Resolution: %d x %d", state.window.render_width, state.window.render_height);
			ImGui::SetNextItemWidth(220.0f);
			if (ImGui::SliderInt("Resolution Percentage", &state.window.resolution_percentage,
				MIN_RENDER_RESOLUTION_PERCENTAGE, MAX_RENDER_RESOLUTION_PERCENTAGE, "%d%%"))
			{
				handle_resize(true);
			}
			const bool immediate = state.debug_ui.show_immediate_timings;
			if (ImGui::BeginTable("##TimingStats", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings))
			{
				stats_ui_table_columns();
				ImGui::TableNextRow();
				stats_ui_cell_label("Frame Time"); ImGui::TableNextColumn(); ImGui::Text("%.2f ms", immediate ? state.debug_ui.immediate_frame_time_ms : state.debug_ui.frame_time_ms);
				stats_ui_cell_label("FPS"); ImGui::TableNextColumn(); ImGui::Text("%.1f", immediate ? state.debug_ui.immediate_fps : state.debug_ui.fps);
				ImGui::TableNextRow();
				stats_ui_cell_label("CPU Time"); ImGui::TableNextColumn();
				if (immediate ? state.debug_ui.immediate_cpu_time_valid : state.debug_ui.cpu_time_valid) ImGui::Text("%.2f ms", immediate ? state.debug_ui.immediate_cpu_time_ms : state.debug_ui.cpu_time_ms); else ImGui::TextDisabled("pending");
				stats_ui_cell_label("GPU Time"); ImGui::TableNextColumn();
				if (immediate ? state.debug_ui.immediate_gpu_time_valid : state.debug_ui.gpu_time_valid) ImGui::Text("%.2f ms", immediate ? state.debug_ui.immediate_gpu_time_ms : state.debug_ui.gpu_time_ms);
				else ImGui::TextDisabled("%s", (immediate ? state.debug_ui.immediate_gpu_time_pending : state.debug_ui.gpu_time_pending) ? "pending" : "unavailable");
				ImGui::TableNextRow();
				stats_ui_cell_label("Timing Mode"); ImGui::TableNextColumn(); ImGui::Checkbox("Immediate##TimingMode", &state.debug_ui.show_immediate_timings);
				stats_ui_cell_label("Profiler"); ImGui::TableNextColumn(); ImGui::Checkbox("##Profiler", &state.debug_ui.show_profiler);
				ImGui::EndTable();
			}
			draw_stats_ui(state);
		}

		if (ImGui::CollapsingHeader("Animation", ImGuiTreeNodeFlags_DefaultOpen))
		{
			if (ImGui::Button("Play")) state.animation.is_playing = true;
			ImGui::SameLine(); if (ImGui::Button("Pause")) state.animation.is_playing = false;
			ImGui::SameLine(); if (ImGui::Button("Rewind")) rewind_skinned_animations();
			ImGui::SetNextItemWidth(160.0f);
			ImGui::DragFloat("Playback Rate", &state.animation.playback_rate, 0.01f, 0.0f, 4.0f, "%.2fx");
			ImGui::Checkbox("Skinning Debug View", &state.animation.skinning_debug_view);
			scene_ensure_indexes(state);
			for (i32 unique_id : state.scene.indexes.armature_object_ids)
			{
				auto found = state.scene.objects.find(unique_id);
				if (found == state.scene.objects.end() || !found->second.has_armature || found->second.armature.animation_count == 0) continue;
				Object& object = found->second; Armature& armature = object.armature;
				const i32 selected = CLAMP(armature.active_animation_index, 0, (i32) armature.animation_count - 1);
				const char* selected_name = armature.animations[selected].name ? armature.animations[selected].name : "<Unnamed Animation>";
				ImGui::PushID(unique_id);
				const char* object_name = object.name ? object.name : "<Unnamed Object>";
				ImGui::Text("%s Animation:", object_name); ImGui::SameLine(); ImGui::SetNextItemWidth(180.0f);
				if (ImGui::BeginCombo("##Animation", selected_name))
				{
					for (u32 idx = 0; idx < armature.animation_count; ++idx)
					{
						const char* name = armature.animations[idx].name ? armature.animations[idx].name : "<Unnamed Animation>";
						if (ImGui::Selectable(name, selected == (i32) idx)) { armature.active_animation_index = (i32) idx; armature.playback_time = 0.0f; armature.current_frame = 0; }
						if (selected == (i32) idx) ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				ImGui::PopID();
			}
		}

		if (ImGui::CollapsingHeader("Rendering Features", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Indent();
			if (ImGui::CollapsingHeader("Tessellation", ImGuiTreeNodeFlags_DefaultOpen))
			{
				bool changed = false;
				changed |= ImGui::Checkbox("Enable Tessellation", &state.tessellation.enabled);
				changed |= ImGui::Combo("Mode", (i32*) &state.tessellation.mode, ETessellationModeNames, (i32) ETessellationMode::MAX);
				changed |= ImGui::SliderInt("Fixed Factor", &state.tessellation.fixed_factor, 1, state.tessellation.max_factor);
				changed |= ImGui::SliderInt("Max Factor", &state.tessellation.max_factor, 1, (i32) Tessellation::MAX_FACTOR);
				changed |= ImGui::SliderFloat("Target Segment", &state.tessellation.target_pixels_per_segment, 1.0f, 64.0f, "%.1f px");
				changed |= ImGui::SliderFloat("Phong Strength", &state.tessellation.phong_strength, 0.0f, 1.0f, "%.2f");
				changed |= ImGui::Checkbox("Virtual Patches", &state.tessellation.virtual_patches_enabled);
				changed |= ImGui::SliderInt("Virtual Patch Depth", &state.tessellation.virtual_patch_max_depth, 0, 4);
				changed |= ImGui::DragInt("Max Patches", &state.tessellation.max_generated_patches, 256.0f, 1, 1024 * 1024);
				changed |= ImGui::DragInt("Max Vertices", &state.tessellation.max_generated_vertices, 1024.0f, 3, 64 * 1024 * 1024);
				changed |= ImGui::DragInt("Max Indices", &state.tessellation.max_generated_indices, 1024.0f, 3, 128 * 1024 * 1024);
				changed |= ImGui::SliderFloat("Bounds Padding", &state.tessellation.bounds_padding, 0.0f, 10.0f, "%.2f");
				ImGui::Text("Meshes: %d  Overflow: %d", state.tessellation.mesh_count, state.tessellation.overflowed_mesh_count);
				ImGui::Text("Source Tris: %d  Patches: %d", state.tessellation.source_triangle_count, state.tessellation.patch_count);
				ImGui::Text("Generated: %d verts / %d indices", state.tessellation.generated_vertex_count, state.tessellation.generated_index_count);
				ImGui::Text("Max Factor: %d", state.tessellation.max_factor_seen);
				ImGui::Text("Readback: %s  Age: %d", state.tessellation.readback_supported ? "Supported" : "Unsupported", state.tessellation.readback_age);
				if (changed) { if (!state.shadow.depth_freeze) ShadowDepthPass::has_valid_shadow_map = false; state.gi.is_updating = true; }
			}
			if (ImGui::CollapsingHeader("Wireframe", ImGuiTreeNodeFlags_DefaultOpen))
			{
				if (ImGui::Checkbox("Shaded Wireframe", &state.wireframe.shaded_wireframe)) TemporalAAPass::invalidate_history(state);
				ImGui::SliderFloat("Wire Width", &state.wireframe.width, 0.5f, 4.0f, "%.2f px");
				ImGui::SliderFloat("Wire Softness", &state.wireframe.softness, 0.25f, 3.0f, "%.2f px");
				ImGui::SliderFloat("Wire Opacity", &state.wireframe.opacity, 0.0f, 1.0f, "%.2f");
				ImGui::ColorEdit3("Wire Color", &state.wireframe.color.X);
			}
			ImGui::Separator();
			if (ImGui::CollapsingHeader("Image Effects", ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::SliderFloat("Exposure (EV)", &state.tonemapping.exposure_bias, -5.0f, 5.0f, "%.2f stops");
				ImGui::Checkbox("SSAO", &state.ssao.enable); ImGui::Checkbox("Fog", &state.fog.debug_active);
				if (ImGui::CollapsingHeader("Antialiasing", ImGuiTreeNodeFlags_DefaultOpen))
				{
					bool changed = ImGui::Checkbox("Temporal AA", &state.temporal_aa.enable);
					ImGui::Checkbox("FXAA", &state.temporal_aa.enable_fxaa);
					ImGui::BeginDisabled(!state.temporal_aa.enable);
					changed |= ImGui::SliderFloat("TAA History Blend", &state.temporal_aa.blend_alpha, 0.0f, 1.0f, "%.2f");
					changed |= ImGui::SliderFloat("TAA Sharpen", &state.temporal_aa.sharpen_strength, 0.0f, 0.5f, "%.3f");
					changed |= ImGui::SliderFloat("TAA Rejection", &state.temporal_aa.rejection_threshold, 0.0f, 1.0f, "%.3f");
					changed |= ImGui::Combo("TAA Debug", &state.temporal_aa.debug_mode, "Off\0History Acceptance\0Previous UV\0");
					ImGui::EndDisabled(); if (changed) TemporalAAPass::invalidate_history(state);
				}
				if (ImGui::CollapsingHeader("Depth-of-Field", ImGuiTreeNodeFlags_DefaultOpen))
				{
					if (ImGui::Checkbox("Enable DoF", &state.dof.enable)) TemporalAAPass::invalidate_history(state);
					ImGui::BeginDisabled(!state.dof.enable);
					ImGui::SliderFloat("Focus Distance", &state.dof.focus_distance, 0.1f, 500.0f, "%.1f");
					ImGui::SliderFloat("Focus Range", &state.dof.focus_range, 0.1f, 200.0f, "%.1f");
					ImGui::SliderFloat("Max CoC Radius", &state.dof.max_coc_radius, 0.0f, 32.0f, "%.1f px");
					ImGui::SliderFloat("Foreground Scale", &state.dof.foreground_blur_scale, 0.0f, 4.0f, "%.2f");
					ImGui::SliderFloat("Background Scale", &state.dof.background_blur_scale, 0.0f, 4.0f, "%.2f");
					ImGui::Checkbox("Show CoC Debug", &state.dof.debug_show_coc); ImGui::EndDisabled();
				}
			}
			ImGui::Unindent(); ImGui::Separator(); ImGui::Indent();
			if (ImGui::CollapsingHeader("Shadows", ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::Checkbox("Shadow Rendering", &state.shadow.rendering_enable); ImGui::Checkbox("Shadow Blur", &state.shadow.blur_enable);
				ImGui::Checkbox("Freeze Shadow Depth", &state.shadow.depth_freeze);
				if (ImGui::Button("Recapture Shadow Depth")) { state.shadow.force_recapture = true; ShadowDepthPass::has_valid_shadow_map = false; }
				bool changed = ImGui::SliderInt("Num Cascades", &state.shadow.num_cascades, 1, MAX_SHADOW_CASCADES);
				f32& distance_scale = state.shadow.cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares ? state.shadow.centered_square_cascade_distance_scale : state.shadow.frustum_cascade_distance_scale;
				changed |= ImGui::SliderFloat("Cascade Distance Scale", &distance_scale, 0.25f, 4.0f, "%.2f");
				changed |= ImGui::Combo("Cascade Placement", (i32*) &state.shadow.cascade_placement_mode, "Frustum\0Centered Squares\0");
				if (changed && !state.shadow.depth_freeze) ShadowDepthPass::has_valid_shadow_map = false;
				if (state.shadow.cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares)
				{
					if (ImGui::SliderFloat("Centered Square Lookahead", &state.shadow.centered_square_lookahead_distance, 0.0f, 1000.0f, "%.2f") && !state.shadow.depth_freeze) ShadowDepthPass::has_valid_shadow_map = false;
					ImGui::BeginDisabled(!state.shadow.depth_freeze);
					if (ImGui::DragFloat3("Centered Square Center", &state.shadow.centered_square_center.X, 0.25f, -10000.0f, 10000.0f, "%.2f")) ShadowDepthPass::has_valid_shadow_map = false;
					ImGui::EndDisabled();
				}
				ImGui::Checkbox("Show Cascade Selection", &state.shadow.debug_show_cascade_selection);
				if (ImGui::CollapsingHeader("Screen Space Shadows", ImGuiTreeNodeFlags_DefaultOpen))
				{
					ImGui::Checkbox("Enable Screen Space Shadows", &state.shadow.screen_space.enable);
					ImGui::SliderFloat("Contact Ray Length", &state.shadow.screen_space.ray_length, 0.0f, 10.0f, "%.2f");
					ImGui::SliderFloat("Thickness", &state.shadow.screen_space.thickness, 0.001f, 0.5f, "%.3f");
					ImGui::SliderFloat("Jitter Strength", &state.shadow.screen_space.jitter_strength, 0.0f, 2.0f, "%.2f");
					ImGui::SliderInt("Max Steps", &state.shadow.screen_space.max_steps, 1, 64);
					ImGui::SliderFloat("Intensity", &state.shadow.screen_space.intensity, 0.0f, 1.0f, "%.2f");
					ImGui::SliderInt("Filter Radius", &state.shadow.screen_space.filter_radius, 0, 2);
					ImGui::Checkbox("Show Screen Space Shadow Mask", &state.shadow.screen_space.debug_show_mask);
					if (state.shadow.screen_space.debug_show_mask) ImGui::Image(texture(frame_data.linear_sampler, get_render_pass(ERenderPass::ScreenSpaceShadows).get_color_output(0).view), image_size(get_render_pass(ERenderPass::ScreenSpaceShadows).get_color_output(0), 256.0f));
				}
			}
			ImGui::Unindent(); ImGui::Separator(); ImGui::Indent();
			if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::Checkbox("Sky Rendering", &state.sky.rendering_enable); ImGui::Checkbox("Direct Lighting", &state.lighting.direct_enable);
				ImGui::Separator(); ImGui::Indent();
				if (ImGui::CollapsingHeader("Global Illumination", ImGuiTreeNodeFlags_DefaultOpen))
				{
					ImGui::Checkbox("GI", &state.gi.enable); ImGui::Checkbox("GI Probe Occlusion", &state.gi.probe_occlusion);
					if (ImGui::SliderInt("GI Octree Depth", &state.gi.octree_depth, GI_Scene::min_octree_depth, GI_Scene::max_octree_depth)) { state.gi.layout_dirty = true; state.gi.is_updating = true; }
					ImGui::Text("Octree: depth %d  nodes %zu  payloads %d  probes %d", gi_scene.octree_depth, gi_scene.octree_nodes.length(), gi_scene.payload_count, gi_scene.non_fallback_probe_count);
					ImGui::Text("Atlas: %zu / %d", gi_scene.probes.length(), gi_scene_atlas_capacity());
					ImGui::Text("Bounds Min: %.2f %.2f %.2f", gi_scene.scene_bounds.min.X, gi_scene.scene_bounds.min.Y, gi_scene.scene_bounds.min.Z);
					ImGui::Text("Bounds Max: %.2f %.2f %.2f", gi_scene.scene_bounds.max.X, gi_scene.scene_bounds.max.Y, gi_scene.scene_bounds.max.Z);
					ImGui::Text("Cell Extent: %.2f / %.2f  Max Radial Depth: %.2f", gi_scene.min_occupied_cell_extent, gi_scene.max_occupied_cell_extent, gi_scene.max_radial_depth);
					if (ImGui::Combo("Probe Radiance Mode", (i32*) &state.gi.probe_radiance_mode, "Octahedral\0SH9\0SG9\0")) state.gi.is_updating = true;
					if (ImGui::Combo("Probe Occlusion Mode", (i32*) &state.gi.probe_occlusion_mode, "Chebyshev\0EVRP4\0")) state.gi.is_updating = true;
					if (ImGui::Checkbox("render sky to probes", &state.gi.render_sky_to_probes)) state.gi.is_updating = true;
					ImGui::Checkbox("Show Probes", &state.gi.show_probes);
					if (ImGui::Checkbox("Probe Isolation", &state.gi.probe_isolation_enable)) { if (state.gi.probe_isolation_enable) { state.gi.show_probes = true; set_mouse_locked(false); } else state.gi.isolated_probe_index = -1; }
					if (state.gi.probe_isolation_enable) { state.gi.show_probes = true; ImGui::SameLine(); if (ImGui::SmallButton("Clear")) state.gi.isolated_probe_index = -1; ImGui::Text("Isolated Probe: %s", state.gi.isolated_probe_index >= 0 ? std::to_string(state.gi.isolated_probe_index).c_str() : "None"); }
					ImGui::SliderFloat("GI Intensity", &state.gi.intensity, 0.0f, 10.0f, "%.2f");
					if (ImGui::Button("Update GI Probes") && !state.gi.is_updating) state.gi.is_updating = true;
					ImGui::SameLine(); ImGui::Checkbox("Compute Irradiance", &state.gi.compute_irradiance); if (state.gi.is_updating) { ImGui::SameLine(); ImGui::Text("Updating..."); }
					if (ImGui::Combo("Probe Vis Mode", (i32*) &state.gi.probe_vis_mode, "Irradiance\0SH9 Irradiance\0SG9 Irradiance\0Radial Depth\0Radial Depth Squared\0EVRP Positive Moment\0") && (state.gi.probe_vis_mode == EProbeVisMode::SH9Irradiance || state.gi.probe_vis_mode == EProbeVisMode::SG9Irradiance)) state.gi.is_updating = true;
					if (ImGui::Checkbox("Debug Constant White Probes", &state.gi.debug_constant_white_probes)) state.gi.is_updating = true;
				}
				ImGui::Unindent();
			}
			ImGui::Unindent();
		}

		if (ImGui::CollapsingHeader("Render Texture Viewer"))
		{
			GpuImage& shadow_image = get_render_pass(ERenderPass::ShadowDepth).get_color_output(0);
			const i32 active_cascades = ShadowDepthPass::get_active_cascade_count(state);
			ImGui::Text("Shadow Cascades");
			ImGui::Text("%u x %u x %d", shadow_image.extent.width, shadow_image.extent.height, active_cascades);
			const f32 distance_scale = state.shadow.cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares ? state.shadow.centered_square_cascade_distance_scale : state.shadow.frustum_cascade_distance_scale;
			ImGui::Text("Distance Scale: %.2f", distance_scale);
			for (i32 cascade = 0; cascade < active_cascades; ++cascade) ImGui::Text("Cascade %d Distance: %.2f", cascade, ShadowDepthPass::cascade_distances[cascade]);
			state.shadow.debug_cascade_index = CLAMP(state.shadow.debug_cascade_index, 0, MAX(0, active_cascades - 1));
			ImGui::SliderInt("Debug Cascade", &state.shadow.debug_cascade_index, 0, MAX(0, active_cascades - 1));
			ImGui::Combo("Debug View", &state.shadow.debug_view_mode, "Moments\0Depth\0");
			draw_texture(frame_data.linear_sampler, "Shadow Cascade Debug", get_render_pass(ERenderPass::ShadowCascadeDebug).get_color_output(0), 256.0f);

			RenderPass& geometry = get_render_pass(ERenderPass::Geometry);
			if (ImGui::TreeNode("Main Pass"))
			{
				for (i32 output = 0; output < Render::GBUFFER_OUTPUT_COUNT; ++output)
				{
					const GpuImage& image = geometry.get_color_output(output);
					ImGui::ImageWithBg(texture(frame_data.linear_sampler, image.view), ImVec2(image.extent.width / 4.0f, image.extent.height / 4.0f), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 1), ImVec4(1, 1, 1, 1));
				}
				ImGui::TreePop();
			}
			draw_texture(frame_data.linear_sampler, "Octahedral Atlas: Lighting", gi_scene.lighting_capture.cube_to_oct_pass.get_color_output(0), 256.0f);
			draw_texture(frame_data.linear_sampler, "Octahedral Atlas: Depth", gi_scene.lighting_capture.cube_to_oct_pass.get_color_output(1), 256.0f);
			draw_texture(frame_data.linear_sampler, "Baked Sky", sky_pass.bake_render_pass.get_color_output(0), 256.0f);
		}
		if (state.images.items.length() > 0 && ImGui::CollapsingHeader("Debug Image Viewer"))
		{
			state.images.debug_index = CLAMP(state.images.debug_index, 0, (i32) state.images.items.length() - 1);
			ImGui::Checkbox("Fullscreen", &state.images.enable_debug_fullscreen);
			ImGui::SliderInt("Image Index", &state.images.debug_index, 0, (i32) state.images.items.length() - 1, "%d", ImGuiSliderFlags_ClampOnInput);
			draw_texture(frame_data.linear_sampler, "Imported Image", state.images.items[state.images.debug_index], 256.0f);
		}
		ImGui::End();

		draw_cpu_profiler_window();

		ImDrawList* foreground = ImGui::GetForegroundDrawList();
		ImVec2 pos(15.0f, 15.0f);
		if (!state.runtime.blender_data_loaded) { foreground->AddText(pos, IM_COL32_WHITE, "Waiting on data from Blender"); pos.y += 20.0f; }
		if (state.debug_camera.active) { foreground->AddText(pos, IM_COL32_WHITE, "Debug Camera Active"); pos.y += 20.0f; }
		if (!state.runtime.is_simulating) { foreground->AddText(pos, IM_COL32_WHITE, "Simulation Paused"); }
	}

	inline void render(VulkanContext* ctx)
	{
		ImGui::Render();

		// ImGui style colors are authored in display-space sRGB. Convert the
		// packed vertex tint to linear before the Vulkan backend uploads it so
		// the swapchain's sRGB attachment encoding produces the intended color.
		// Texture RGB remains untouched (image vertices normally use a white
		// tint), and alpha stays linear for correct attachment blending.
		ImDrawData* draw_data = ImGui::GetDrawData();
		for (ImDrawList* draw_list : draw_data->CmdLists)
		{
			for (ImDrawVert& vertex : draw_list->VtxBuffer)
			{
				const auto linearize_channel = [](u32 in_channel) -> u32
				{
					const f32 srgb = (f32) in_channel / 255.0f;
					const f32 linear = srgb <= 0.04045f
						? srgb / 12.92f
						: powf((srgb + 0.055f) / 1.055f, 2.4f);
					return (u32) (linear * 255.0f + 0.5f);
				};

				const u32 color = vertex.col;
				const u32 red = linearize_channel((color >> IM_COL32_R_SHIFT) & 0xFFu);
				const u32 green = linearize_channel((color >> IM_COL32_G_SHIFT) & 0xFFu);
				const u32 blue = linearize_channel((color >> IM_COL32_B_SHIFT) & 0xFFu);
				const u32 alpha = (color >> IM_COL32_A_SHIFT) & 0xFFu;
				vertex.col = (red << IM_COL32_R_SHIFT)
					| (green << IM_COL32_G_SHIFT)
					| (blue << IM_COL32_B_SHIFT)
					| (alpha << IM_COL32_A_SHIFT);
			}
		}

		VkRenderingAttachmentInfo color_attachment = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = ctx->swapchain_image_views[ctx->swapchain_image_index],
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		};
		VkRenderingInfo rendering_info = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea = { .offset = {0, 0}, .extent = ctx->swapchain_extent },
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &color_attachment,
		};
		VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];
		vkCmdBeginRendering(command_buffer, &rendering_info);
		ImGui_ImplVulkan_RenderDrawData(draw_data, command_buffer);
		vkCmdEndRendering(command_buffer);
	}

	inline void shutdown()
	{
		if (!initialized) { return; }
		clear_textures();
		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		initialized = false;
	}
}

#else

namespace ImGuiLayer
{
	inline constexpr bool initialized = false;
	inline void init(VulkanContext*) {}
	inline void clear_textures() {}
	inline void handle_swapchain_recreated(VulkanContext*) {}
	inline void unregister_texture(VkImageView) {}
	inline void begin_frame() {}
	inline void draw_controls(State&, GI_Scene&) {}
	inline void render(VulkanContext*) {}
	inline void shutdown() {}
}

#endif
