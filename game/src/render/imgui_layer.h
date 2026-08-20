#pragma once

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI

#include "ankerl/unordered_dense.h"
#include "imgui.h"
#include "imgui/backends/imgui_impl_glfw.h"
#include "imgui/backends/imgui_impl_vulkan.h"
#include "render/gt7_tonemapping.h"
#include "ui/stats_ui.h"
#include "ui/cpu_profiler_ui.h"
#include "input/input_api.h"
#include "render/cloud_pass.h"

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
		if (ctx->active_output_mode != EDisplayOutputMode::SDR)
		{
			// Partially transparent SDR panels leak proportionally more light when
			// composited over a scene that can reach 1000 nits. Remap only window
			// surfaces so their apparent opacity remains close to SDR; text and
			// texture alpha must remain untouched for correct antialiasing.
			constexpr f32 paper_white_scale =
				GT7Tonemapping::HDR_PAPER_WHITE_NITS / GT7Tonemapping::HDR_PEAK_NITS;
			constexpr ImGuiCol background_colors[] = {
				ImGuiCol_WindowBg,
				ImGuiCol_ChildBg,
				ImGuiCol_PopupBg,
			};
			ImGuiStyle& style = ImGui::GetStyle();
			for (ImGuiCol color : background_colors)
			{
				f32& alpha = style.Colors[color].w;
				if (alpha > 0.0f)
					alpha = 1.0f - (1.0f - alpha) * paper_white_scale;
			}
		}
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = "bin/imgui.ini";
		ImGui_ImplGlfw_InitForVulkan(ctx->window, false);

		VkFormat color_format = Render::SCENE_COLOR_FORMAT;
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
			ImGui::Text("Display Output: requested %s, active %s",
				vulkan_output_mode_name(state.vk.requested_output_mode),
				vulkan_output_mode_name(state.vk.active_output_mode));
			ImGui::Text("Surface: %s + %s",
				vulkan_format_name(state.vk.surface_format.format),
				vulkan_color_space_name(state.vk.surface_format.colorSpace));
			ImGui::Text("HDR Metadata: %s", state.vk.hdr_metadata_enabled ? "enabled" : "unavailable");
			if (state.vk.output_fallback_reason[0])
			{
				ImGui::TextWrapped("Output fallback: %s", state.vk.output_fallback_reason);
			}
			if (state.vk.active_output_mode == EDisplayOutputMode::SDR)
				ImGui::Text("UI white: 1.000 normalized SDR");
			else
				ImGui::Text("UI white: 203 nits (0.203 normalized HDR)");
			ImGui::SetNextItemWidth(220.0f);
			if (ImGui::SliderInt("Resolution Percentage", &state.window.resolution_percentage,
				MIN_RENDER_RESOLUTION_PERCENTAGE, MAX_RENDER_RESOLUTION_PERCENTAGE, "%d%%"))
			{
				state.window.render_resolution_dirty = true;
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

		if (ImGui::CollapsingHeader("Debug Camera"))
		{
			ImGui::InputFloat("Speed", &state.debug_camera.move_speed, 1.0f, 10.0f, "%.1f");
			state.debug_camera.move_speed = MAX(state.debug_camera.move_speed, 0.0f);
			if (ImGui::Button("Reset Camera Position"))
			{
				state.debug_camera.camera.location = state.debug_camera.initial_location;
			}
			ImGui::TextUnformatted("Hold Shift to move 5x faster.");
		}

		if (ImGui::CollapsingHeader("Animation"))
		{
			if (ImGui::Button("Play")) state.animation.is_playing = true;
			ImGui::SameLine(); if (ImGui::Button("Pause")) state.animation.is_playing = false;
			ImGui::SameLine(); if (ImGui::Button("Rewind")) AnimationSystem::rewind(state);
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
			const auto draw_tessellation_controls = [&]()
			{
				if (ImGui::CollapsingHeader("Tessellation"))
				{
					bool changed = false;
					changed |= ImGui::Checkbox("Enable Tessellation", &state.tessellation.enabled);
					changed |= ImGui::Combo("Mode", (i32*)&state.tessellation.mode, ETessellationModeNames, (i32)ETessellationMode::MAX);
					changed |= ImGui::SliderInt("Fixed Factor", &state.tessellation.fixed_factor, 1, state.tessellation.max_factor);
					changed |= ImGui::SliderInt("Max Factor", &state.tessellation.max_factor, 1, (i32)Tessellation::MAX_FACTOR);
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
					if (changed && !state.shadow.depth_freeze)
						ShadowDepthPass::has_valid_shadow_map = false;
				}
			};
			const auto draw_wireframe_controls = [&]()
			{
				if (ImGui::CollapsingHeader("Wireframe"))
				{
					if (ImGui::Checkbox("Shaded Wireframe", &state.wireframe.shaded_wireframe))
						TemporalAAPass::invalidate_history(state);
					ImGui::SliderFloat("Wire Width", &state.wireframe.width, 0.5f, 4.0f, "%.2f px");
					ImGui::SliderFloat("Wire Softness", &state.wireframe.softness, 0.25f, 3.0f, "%.2f px");
					ImGui::SliderFloat("Wire Opacity", &state.wireframe.opacity, 0.0f, 1.0f, "%.2f");
					ImGui::ColorEdit3("Wire Color", &state.wireframe.color.X);
				}
			};
			const auto draw_tonemapping_controls = [&]()
			{
				if (ImGui::CollapsingHeader("Tonemapping"))
				{
					ImGui::Combo(
						"Method##Tonemapping",
						(i32*)&state.tonemapping.method,
						ETonemappingMethodNames,
						(i32)ETonemappingMethod::MAX
                    );

					ImGui::SliderFloat(
						state.tonemapping.auto_exposure_enabled
							? "Exposure Compensation (EV)"
							: "Exposure (EV)",
						&state.tonemapping.exposure_bias,
						-5.0f, 5.0f, "%.2f stops"
                    );

					if (ImGui::Button("Reset Adaptation"))
                    {
						state.tonemapping.adaptation_reset_requested = true;
                    }

					ImGui::Checkbox("Auto Exposure", &state.tonemapping.auto_exposure_enabled);
                    ImGui::Indent();
					ImGui::BeginDisabled(!state.tonemapping.auto_exposure_enabled);
					{
						if (ImGui::SliderFloat("Minimum Auto EV",
							&state.tonemapping.auto_exposure_min_ev,
							-16.0f, 16.0f, "%.2f EV"))
						{
							state.tonemapping.auto_exposure_max_ev = MAX(
								state.tonemapping.auto_exposure_max_ev,
								state.tonemapping.auto_exposure_min_ev);
						}
						if (ImGui::SliderFloat("Maximum Auto EV",
							&state.tonemapping.auto_exposure_max_ev,
							-16.0f, 16.0f, "%.2f EV"))
						{
							state.tonemapping.auto_exposure_min_ev = MIN(
								state.tonemapping.auto_exposure_min_ev,
								state.tonemapping.auto_exposure_max_ev);
						}
						ImGui::SliderFloat("Brightening Response",
							&state.tonemapping.auto_exposure_brightening_seconds,
							0.01f, 10.0f, "%.2f s", ImGuiSliderFlags_Logarithmic);
						ImGui::SliderFloat("Darkening Response",
							&state.tonemapping.auto_exposure_darkening_seconds,
							0.01f, 10.0f, "%.2f s", ImGuiSliderFlags_Logarithmic);
    
                        ImGui::TextDisabled(
                            "Auto EV %.2f -> %.2f | white xy %.4f, %.4f",
                            state.tonemapping.adaptation_current_ev,
                            state.tonemapping.adaptation_target_ev,
                            state.tonemapping.adaptation_measured_white_x,
                            state.tonemapping.adaptation_measured_white_y
                        );
						ImGui::TextDisabled(
							"Solar guard %.2f -> %.2f EV | weight %.3f | disc %.2f EV",
							state.tonemapping.adaptation_base_target_ev,
							state.tonemapping.adaptation_guarded_target_ev,
							state.tonemapping.adaptation_solar_guard_weight,
							state.tonemapping.adaptation_solar_disc_ev);
					}
					ImGui::EndDisabled();
                    ImGui::Unindent();

					ImGui::Checkbox(
						"Auto White Balance", &state.tonemapping.auto_white_balance_enabled);
                    ImGui::Indent();
					ImGui::BeginDisabled(!state.tonemapping.auto_white_balance_enabled);
					{
						ImGui::SliderFloat("White-Balance Response",
							&state.tonemapping.auto_white_balance_seconds,
							0.01f, 10.0f, "%.2f s", ImGuiSliderFlags_Logarithmic);
						ImGui::SliderFloat("AWB Strength",
							&state.tonemapping.auto_white_balance_strength,
							0.0f, 1.0f, "%.2f");

                        ImGui::TextDisabled(
                            "Bradford LMS %.3f, %.3f, %.3f | %d samples%s",
                            state.tonemapping.adaptation_current_l_gain,
                            state.tonemapping.adaptation_current_m_gain,
                            state.tonemapping.adaptation_current_s_gain,
                            state.tonemapping.adaptation_accepted_sample_count,
                            state.tonemapping.adaptation_measurement_valid ? "" : " (invalid)"
                        );
					}

					ImGui::EndDisabled();
                    ImGui::Unindent();

					ImGui::Checkbox("Local Tonemapping", &state.tonemapping.local_enabled);
					if (!state.tonemapping.local_enabled)
					{
						state.debug_ui.show_local_tonemapping_debug = false;
					}
					ImGui::Indent();
					ImGui::BeginDisabled(!state.tonemapping.local_enabled);
					{
						ImGui::SliderFloat(
							"Shadow Recovery",
							&state.tonemapping.local_shadow_recovery,
							0.0f, 4.0f, "%.2f EV");
						ImGui::SliderFloat(
							"Highlight Recovery",
							&state.tonemapping.local_highlight_recovery,
							0.0f, 4.0f, "%.2f EV");
						ImGui::SliderFloat(
							"Exposure Preference Sigma",
							&state.tonemapping.local_exposure_preference_sigma,
							0.0f, 10.0f, "%.2f");

						const i32 max_mip = tonemapping_pass_get_max_full_resolution_mip();
						const i32 minimum_reconstruction_mip = MIN(2, max_mip);
						state.tonemapping.local_reconstruction_mip = CLAMP(
							state.tonemapping.local_reconstruction_mip,
							minimum_reconstruction_mip,
							max_mip);
						state.tonemapping.local_coarsest_mip = CLAMP(
							state.tonemapping.local_coarsest_mip,
							state.tonemapping.local_reconstruction_mip,
							max_mip);
						ImGui::SliderInt(
							"Coarsest Mip",
							&state.tonemapping.local_coarsest_mip,
							state.tonemapping.local_reconstruction_mip,
							max_mip,
							"%d",
							ImGuiSliderFlags_ClampOnInput);
						if (ImGui::SliderInt(
								"Reconstruction Mip",
								&state.tonemapping.local_reconstruction_mip,
								minimum_reconstruction_mip,
								state.tonemapping.local_coarsest_mip,
								"%d",
								ImGuiSliderFlags_ClampOnInput))
						{
							state.tonemapping.local_coarsest_mip = MAX(
								state.tonemapping.local_coarsest_mip,
								state.tonemapping.local_reconstruction_mip);
						}
						ImGui::Checkbox(
							"Boost Local Contrast",
							&state.tonemapping.local_contrast_boost);
						ImGui::Checkbox(
							"Show Local Tonemapping Debug Overlay",
							&state.debug_ui.show_local_tonemapping_debug);
						state.debug_ui.local_tonemapping_debug_mip = CLAMP(
							state.debug_ui.local_tonemapping_debug_mip,
							state.tonemapping.local_reconstruction_mip,
							state.tonemapping.local_coarsest_mip);
						ImGui::BeginDisabled(
							!state.debug_ui.show_local_tonemapping_debug);
						ImGui::SliderInt(
							"Debug Mip",
							&state.debug_ui.local_tonemapping_debug_mip,
							state.tonemapping.local_reconstruction_mip,
							state.tonemapping.local_coarsest_mip,
							"%d",
							ImGuiSliderFlags_ClampOnInput);
						ImGui::EndDisabled();
						if (ImGui::Button("Reset Defaults"))
						{
							state.tonemapping.reset_defaults();
						}
						ImGui::TextDisabled(
							"Effective mip range: %d -> %d (available through %d)",
							state.tonemapping.local_coarsest_mip,
							state.tonemapping.local_reconstruction_mip,
							max_mip);
					}
					ImGui::EndDisabled();
					ImGui::Unindent();
				}
			};
			const auto draw_bloom_controls = [&]()
			{
				if (ImGui::CollapsingHeader("Bloom"))
				{
					ImGui::Checkbox("Enable Bloom", &state.bloom.enable);
					ImGui::BeginDisabled(!state.bloom.enable);
					ImGui::SliderFloat("Bloom Threshold", &state.bloom.threshold, 0.0f, 10.0f, "%.2f");
					ImGui::SliderFloat("Bloom Soft Knee", &state.bloom.soft_knee, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat(
						"Bloom Intensity",
						&state.bloom.intensity,
						0.0f,
						State::BloomState::MAX_INTENSITY,
						"%.3f");
					f32 auto_exposure_influence_percent =
						state.bloom.auto_exposure_influence * 100.0f;
					ImGui::BeginDisabled(!state.tonemapping.auto_exposure_enabled);
					if (ImGui::SliderFloat(
						"Bloom Auto-Exposure Influence",
						&auto_exposure_influence_percent,
						0.0f, 100.0f, "%.0f%%"))
					{
						state.bloom.auto_exposure_influence = CLAMP(
							auto_exposure_influence_percent * 0.01f, 0.0f, 1.0f);
					}
					ImGui::EndDisabled();
					const i32 available_bloom_mips = BloomPass::get_available_mip_count();
					state.bloom.requested_mip_count = CLAMP(
						state.bloom.requested_mip_count, 1, available_bloom_mips);
					ImGui::SliderInt(
						"Bloom Mip Count",
						&state.bloom.requested_mip_count,
						1,
						available_bloom_mips,
						"%d",
						ImGuiSliderFlags_ClampOnInput);
					if (ImGui::Button("Reset Bloom Defaults"))
					{
						state.bloom.reset_defaults();
					}
					GpuImage& bloom_pyramid = BloomPass::get_pyramid();
					ImGui::TextDisabled(
						"Half-res pyramid: %u x %u, %d / %d mips",
						bloom_pyramid.extent.width,
						bloom_pyramid.extent.height,
						state.bloom.requested_mip_count,
						available_bloom_mips);
					ImGui::EndDisabled();
				}
			};
			const auto draw_ssao_controls = [&]()
			{
				if (ImGui::CollapsingHeader("SSAO"))
				{
					ImGui::Checkbox("Enable SSAO", &state.ssao.enable);
				}
			};
			const auto draw_fog_controls = [&]()
			{
				if (ImGui::CollapsingHeader("Fog"))
				{
					ImGui::Checkbox("Enable Fog", &state.fog.debug_active);
				}
			};
			const auto draw_temporal_aa_controls = [&]()
			{
				if (ImGui::CollapsingHeader("Temporal AA"))
				{
					bool changed = ImGui::Checkbox("Enable Temporal AA", &state.temporal_aa.enable);
					ImGui::BeginDisabled(!state.temporal_aa.enable);
					changed |= ImGui::SliderFloat("TAA History Blend", &state.temporal_aa.blend_alpha, 0.0f, 1.0f, "%.2f");
					changed |= ImGui::SliderFloat("TAA Sharpen", &state.temporal_aa.sharpen_strength, 0.0f, 0.5f, "%.3f");
					changed |= ImGui::SliderFloat("TAA Rejection", &state.temporal_aa.rejection_threshold, 0.0f, 1.0f, "%.3f");
					changed |= ImGui::Combo("TAA Debug", &state.temporal_aa.debug_mode, "Off\0History Acceptance\0Previous UV\0");
					ImGui::EndDisabled();
					if (changed)
						TemporalAAPass::invalidate_history(state);
				}
			};
			const auto draw_fxaa_controls = [&]()
			{
				if (ImGui::CollapsingHeader("FXAA"))
				{
					ImGui::Checkbox("Enable FXAA", &state.temporal_aa.enable_fxaa);
				}
			};
			const auto draw_dof_controls = [&]()
			{
				if (ImGui::CollapsingHeader("Depth-of-Field"))
				{
					if (ImGui::Checkbox("Enable DoF", &state.dof.enable))
						TemporalAAPass::invalidate_history(state);
					ImGui::BeginDisabled(!state.dof.enable);
					ImGui::SliderFloat("Focus Distance", &state.dof.focus_distance, 0.1f, 500.0f, "%.1f");
					ImGui::SliderFloat("Focus Range", &state.dof.focus_range, 0.1f, 200.0f, "%.1f");
					ImGui::SliderFloat("Max CoC Radius", &state.dof.max_coc_radius, 0.0f, 32.0f, "%.1f px");
					ImGui::SliderFloat("Foreground Scale", &state.dof.foreground_blur_scale, 0.0f, 4.0f, "%.2f");
					ImGui::SliderFloat("Background Scale", &state.dof.background_blur_scale, 0.0f, 4.0f, "%.2f");
					ImGui::Checkbox("Show CoC Debug", &state.dof.debug_show_coc);
					ImGui::EndDisabled();
				}
			};
			const auto draw_shadow_map_controls = [&]()
			{
				if (ImGui::CollapsingHeader("Shadow Maps"))
				{
					ImGui::Checkbox("Shadow Rendering", &state.shadow.rendering_enable);
					ImGui::Checkbox("Shadow Blur", &state.shadow.blur_enable);
					ImGui::Checkbox("Freeze Shadow Depth", &state.shadow.depth_freeze);
					if (ImGui::Button("Recapture Shadow Depth"))
					{
						state.shadow.force_recapture = true;
						ShadowDepthPass::has_valid_shadow_map = false;
					}
					bool changed = ImGui::SliderInt("Num Cascades", &state.shadow.num_cascades, 1, MAX_SHADOW_CASCADES);
					f32& distance_scale = state.shadow.cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares ? state.shadow.centered_square_cascade_distance_scale : state.shadow.frustum_cascade_distance_scale;
					changed |= ImGui::SliderFloat("Cascade Distance Scale", &distance_scale, 0.25f, 4.0f, "%.2f");
					changed |= ImGui::Combo("Cascade Placement", (i32*)&state.shadow.cascade_placement_mode, "Frustum\0Centered Squares\0");
					if (changed && !state.shadow.depth_freeze)
						ShadowDepthPass::has_valid_shadow_map = false;
					if (state.shadow.cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares)
					{
						if (ImGui::SliderFloat("Centered Square Lookahead", &state.shadow.centered_square_lookahead_distance, 0.0f, 1000.0f, "%.2f") && !state.shadow.depth_freeze)
							ShadowDepthPass::has_valid_shadow_map = false;
						ImGui::BeginDisabled(!state.shadow.depth_freeze);
						if (ImGui::DragFloat3("Centered Square Center", &state.shadow.centered_square_center.X, 0.25f, -10000.0f, 10000.0f, "%.2f"))
							ShadowDepthPass::has_valid_shadow_map = false;
						ImGui::EndDisabled();
					}
					ImGui::Checkbox("Show Cascade Selection", &state.shadow.debug_show_cascade_selection);
				}
			};
			const auto draw_screen_space_shadow_controls = [&]()
			{
				if (ImGui::CollapsingHeader("Screen Space Shadows"))
				{
					ImGui::Checkbox("Enable Screen Space Shadows", &state.shadow.screen_space.enable);
					ImGui::SliderFloat("Contact Ray Length", &state.shadow.screen_space.ray_length, 0.0f, 10.0f, "%.2f");
					ImGui::SliderFloat("Thickness", &state.shadow.screen_space.thickness, 0.001f, 0.5f, "%.3f");
					ImGui::SliderFloat("Jitter Strength", &state.shadow.screen_space.jitter_strength, 0.0f, 2.0f, "%.2f");
					ImGui::SliderInt("Max Steps", &state.shadow.screen_space.max_steps, 1, 64);
					ImGui::SliderFloat("Intensity", &state.shadow.screen_space.intensity, 0.0f, 1.0f, "%.2f");
					ImGui::SliderInt("Filter Radius", &state.shadow.screen_space.filter_radius, 0, 2);
					ImGui::Checkbox("Show Screen Space Shadow Mask", &state.shadow.screen_space.debug_show_mask);
					if (state.shadow.screen_space.debug_show_mask)
						ImGui::Image(texture(frame_data.linear_sampler, get_render_pass(ERenderPass::ScreenSpaceShadows).get_color_output(0).view), image_size(get_render_pass(ERenderPass::ScreenSpaceShadows).get_color_output(0), 256.0f));
				}
			};
			const auto draw_lighting_controls = [&]()
			{
				if (ImGui::CollapsingHeader("Lighting"))
				{
					ImGui::Checkbox("Sky Rendering", &state.sky.rendering_enable);
					ImGui::Text("LUT precomputes: %llu",
						(unsigned long long) bruneton_atmosphere_pass.precompute_count);
					ImGui::Text("Last LUT CPU submit: %.2f ms",
						bruneton_atmosphere_pass.last_precompute_ms);
					ImGui::Checkbox("Direct Lighting", &state.lighting.direct_enable);
					ImGui::Separator();
					ImGui::Indent();
					if (ImGui::CollapsingHeader("Global Illumination"))
					{
						ImGui::Checkbox("GI", &state.gi.enable);
						if (ImGui::Checkbox("GI Probe Influence Culling", &state.gi.probe_influence_culling))
							state.gi.is_updating = true;
						ImGui::Checkbox("GI Probe Occlusion", &state.gi.probe_occlusion);
						if (ImGui::SliderInt("GI Octree Depth", &state.gi.octree_depth, GI_Scene::min_octree_depth, GI_Scene::max_octree_depth))
						{
							state.gi.layout_dirty = true;
							state.gi.is_updating = true;
						}
						ImGui::Text("Octree: depth %d  nodes %zu  payloads %d  probes %d", gi_scene.octree_depth, gi_scene.octree_nodes.length(), gi_scene.payload_count, gi_scene.non_fallback_probe_count);
						ImGui::Text("Atlas: %zu / %d", gi_scene.probes.length(), gi_scene_atlas_capacity());
						u64 specular_pixels = 0;
						for (i32 mip = 0; mip < gi_scene.lighting_capture.desc.specular_mip_count; ++mip)
						{
							const u64 dimension = (u64)gi_scene.lighting_capture.specular_atlas_total_size >> mip;
							specular_pixels += dimension * dimension;
						}
						const u64 specular_bytes_per_pixel =
							gi_scene.lighting_capture.specular_atlas.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 16;
						ImGui::Text(
							"Specular Atlas: %d x %d, tile %d, %d mips, %.1f MiB",
							gi_scene.lighting_capture.specular_atlas_total_size,
							gi_scene.lighting_capture.specular_atlas_total_size,
							gi_scene.lighting_capture.desc.specular_entry_size,
							gi_scene.lighting_capture.desc.specular_mip_count,
							(f64)(specular_pixels * specular_bytes_per_pixel) / (1024.0 * 1024.0));
						ImGui::Text("Bounds Min: %.2f %.2f %.2f", gi_scene.scene_bounds.min.X, gi_scene.scene_bounds.min.Y, gi_scene.scene_bounds.min.Z);
						ImGui::Text("Bounds Max: %.2f %.2f %.2f", gi_scene.scene_bounds.max.X, gi_scene.scene_bounds.max.Y, gi_scene.scene_bounds.max.Z);
						ImGui::Text("Cell Extent: %.2f / %.2f  Max Radial Depth: %.2f", gi_scene.min_occupied_cell_extent, gi_scene.max_occupied_cell_extent, gi_scene.max_radial_depth);
						if (ImGui::Combo("Probe Radiance Mode", (i32*)&state.gi.probe_radiance_mode, "Octahedral\0SH9\0SG9\0"))
							state.gi.is_updating = true;
						if (ImGui::Combo("Probe Occlusion Mode", (i32*)&state.gi.probe_occlusion_mode, "Chebyshev\0EVRP4\0"))
							state.gi.is_updating = true;
						if (ImGui::Checkbox("render sky to probes", &state.gi.render_sky_to_probes))
							state.gi.is_updating = true;
						ImGui::Checkbox("Probe Specular IBL", &state.gi.probe_specular_enable);
						ImGui::Checkbox("Show Probes", &state.gi.show_probes);
						ImGui::Checkbox("Filter Probe Level", &state.gi.probe_level_filter_enable);
						ImGui::SameLine();
						state.gi.probe_level_filter_selection = CLAMP(
							state.gi.probe_level_filter_selection, 0, gi_scene.octree_depth + 1);
						char probe_level_label[48] = {};
						if (state.gi.probe_level_filter_selection == 0)
							snprintf(probe_level_label, sizeof(probe_level_label), "Fallback");
						else if (state.gi.probe_level_filter_selection == 1)
							snprintf(probe_level_label, sizeof(probe_level_label), "Level 0 (largest)");
						else if (state.gi.probe_level_filter_selection == gi_scene.octree_depth + 1)
							snprintf(probe_level_label, sizeof(probe_level_label),
								"Level %d (deepest)", gi_scene.octree_depth);
						else
							snprintf(probe_level_label, sizeof(probe_level_label),
								"Level %d", state.gi.probe_level_filter_selection - 1);
						ImGui::BeginDisabled(!state.gi.probe_level_filter_enable);
						ImGui::SetNextItemWidth(180.0f);
						ImGui::SliderInt(
							"##Probe Level",
							&state.gi.probe_level_filter_selection,
							0,
							gi_scene.octree_depth + 1,
							probe_level_label,
							ImGuiSliderFlags_AlwaysClamp);
						ImGui::EndDisabled();
						if (ImGui::Checkbox("Probe Isolation", &state.gi.probe_isolation_enable))
						{
							if (state.gi.probe_isolation_enable)
							{
								state.gi.show_probes = true;
								InputSystem::set_mouse_locked(state, false);
							}
							else
								state.gi.isolated_probe_index = -1;
						}
						if (state.gi.probe_isolation_enable)
						{
							state.gi.show_probes = true;
							ImGui::SameLine();
							if (ImGui::SmallButton("Clear"))
								state.gi.isolated_probe_index = -1;
							ImGui::Text("Isolated Probe: %s", state.gi.isolated_probe_index >= 0 ? std::to_string(state.gi.isolated_probe_index).c_str() : "None");
						}
						ImGui::SliderFloat("GI Intensity", &state.gi.intensity, 0.0f, 10.0f, "%.2f");
						if (ImGui::Button("Update GI Probes") && !state.gi.is_updating)
							state.gi.is_updating = true;
						ImGui::SameLine();
						ImGui::Checkbox("Compute Irradiance", &state.gi.compute_irradiance);
						if (state.gi.is_updating)
						{
							ImGui::SameLine();
							ImGui::Text("Updating...");
						}
						if (ImGui::Combo("Probe Vis Mode", (i32*)&state.gi.probe_vis_mode, "Irradiance\0SH9 Irradiance\0SG9 Irradiance\0Radial Depth\0Radial Depth Squared\0EVRP Positive Moment\0Specular\0") && (state.gi.probe_vis_mode == EProbeVisMode::SH9Irradiance || state.gi.probe_vis_mode == EProbeVisMode::SG9Irradiance))
							state.gi.is_updating = true;
						if (state.gi.probe_vis_mode == EProbeVisMode::Specular)
							ImGui::SliderFloat("Specular Debug Roughness", &state.gi.specular_debug_roughness, 0.0f, 1.0f, "%.2f");
						if (ImGui::Checkbox("Debug Constant White Probes", &state.gi.debug_constant_white_probes))
							state.gi.is_updating = true;
					}
					ImGui::Unindent();
				}
			};
			const auto draw_cloud_controls = [&]()
			{
				if (ImGui::CollapsingHeader("Cloud System"))
				{
					ImGui::Text("Active: %s", state.clouds.active ? "yes" : "no");
					ImGui::Text("Active layers: %d / %d",
						state.clouds.active_layer_count, MAX_CLOUD_LAYERS);
					ImGui::Text("Temporal history: %s",
						CloudPass::pass.history_valid ? "valid" : "invalid");
					ImGui::Text("Procedural caches: %s",
						CloudPass::pass.caches_generated ? "ready" : "pending");
					ImGui::Checkbox("Apply Cloud Shadows",
						&state.clouds.shadow_lighting_enabled);
					ImGui::Checkbox("Show Shadow Map Fullscreen",
						&state.clouds.debug_show_shadow_map_fullscreen);
					if (state.scene.active_cloud_controller_id.has_value()
						&& state.scene.objects.contains(*state.scene.active_cloud_controller_id))
					{
						const Object& controller = state.scene.objects.at(
							*state.scene.active_cloud_controller_id);
						const f32 shadow_extent = controller.cloud_system.shadow_extent_m;
						const bool sun_casts_shadows = controller.light.sun.cast_shadows;
						ImGui::Text("Cloud shadows: %s  Extent: %.1f km  Texel: %.1f m",
							controller.cloud_system.shadow_enabled && sun_casts_shadows
								&& state.clouds.shadow_lighting_enabled
								? "enabled" : "disabled",
							shadow_extent * 0.001f, shadow_extent / 512.0f);
						ImGui::TextDisabled(
							"Cloud transmittance attenuates direct atmosphere-Sun lighting only");
						if (shadow_extent / 512.0f > 50.0f)
							ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
								"Broad footprint: small views may receive nearly uniform shadowing");
					}
					ImGui::SeparatorText("Quality");
					bool resolution_changed = ImGui::SliderFloat(
						"Cloud Resolution Scale", &state.clouds.resolution_scale,
						0.5f, 1.0f, "%.2fx");
					bool quality_changed = resolution_changed;
					quality_changed |= ImGui::SliderInt(
						"View Steps", &state.clouds.view_steps, 12, 48);
					quality_changed |= ImGui::SliderFloat(
						"Dense Step Scale", &state.clouds.dense_step_scale,
						0.5f, 1.0f, "%.2f");
					quality_changed |= ImGui::SliderFloat(
						"Empty Step Scale", &state.clouds.empty_step_scale,
						1.0f, 4.0f, "%.2f");
					quality_changed |= ImGui::SliderInt(
						"Sun Cone Samples", &state.clouds.sun_cone_samples, 1, 8);
					quality_changed |= ImGui::SliderFloat(
						"History Weight", &state.clouds.history_weight,
						0.0f, 0.98f, "%.2f");
					quality_changed |= ImGui::SliderFloat(
						"Depth Rejection", &state.clouds.depth_rejection,
						0.01f, 0.5f, "%.3f");
					quality_changed |= ImGui::SliderFloat(
						"Low-Density Edge Fade", &state.clouds.low_density_edge_fade,
						0.0f, 0.2f, "%.3f");
					quality_changed |= ImGui::SliderFloat(
						"Minimum Density", &state.clouds.minimum_density,
						0.0f, 0.02f, "%.4f");
					quality_changed |= ImGui::SliderFloat(
						"History Clip Sigma", &state.clouds.history_clip_sigma,
						0.25f, 4.0f, "%.2f");
					quality_changed |= ImGui::SliderFloat(
						"Opacity Rejection", &state.clouds.opacity_rejection,
						0.01f, 1.0f, "%.2f");
					ImGui::TextDisabled(
						"Edge Fade/Minimum Density trim wisps; Clip Sigma/Opacity Rejection tune stability");
					if (ImGui::Button("Reset Cloud Quality"))
					{
						state.clouds.resolution_scale = State::CloudState::DEFAULT_RESOLUTION_SCALE;
						state.clouds.view_steps = State::CloudState::DEFAULT_VIEW_STEPS;
						state.clouds.dense_step_scale = State::CloudState::DEFAULT_DENSE_STEP_SCALE;
						state.clouds.empty_step_scale = State::CloudState::DEFAULT_EMPTY_STEP_SCALE;
						state.clouds.sun_cone_samples = State::CloudState::DEFAULT_SUN_CONE_SAMPLES;
						state.clouds.history_weight = State::CloudState::DEFAULT_HISTORY_WEIGHT;
						state.clouds.depth_rejection = State::CloudState::DEFAULT_DEPTH_REJECTION;
						state.clouds.low_density_edge_fade =
							State::CloudState::DEFAULT_LOW_DENSITY_EDGE_FADE;
						state.clouds.minimum_density = State::CloudState::DEFAULT_MINIMUM_DENSITY;
						state.clouds.history_clip_sigma = State::CloudState::DEFAULT_HISTORY_CLIP_SIGMA;
						state.clouds.opacity_rejection = State::CloudState::DEFAULT_OPACITY_REJECTION;
						resolution_changed = true;
						quality_changed = true;
					}
					if (resolution_changed)
						state.window.render_resolution_dirty = true;
					if (quality_changed)
						state.clouds.history_reset_requested = true;
					if (state.clouds.layer_budget_warning)
						ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
							"3-4 layers may exceed the 4 ms target");
					ImGui::TextDisabled(
						"Profiler scopes: shadow, ray march, temporal, composite");
				}
			};

			// Keep the controls in the same order as their frame passes.
			draw_tessellation_controls();
			draw_shadow_map_controls();
			draw_ssao_controls();
			draw_screen_space_shadow_controls();
			draw_lighting_controls();
			draw_cloud_controls();
			draw_fog_controls();
			draw_dof_controls();
			draw_wireframe_controls();
			draw_temporal_aa_controls();
			draw_bloom_controls();
			draw_tonemapping_controls();
			draw_fxaa_controls();
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
			for (i32 mip = 0; mip < gi_scene.lighting_capture.desc.specular_mip_count; ++mip)
			{
				char label[64];
				snprintf(label, sizeof(label), "Specular Atlas: Mip %d", mip);
				draw_texture(
					frame_data.linear_sampler, label, gi_scene.lighting_capture.specular_atlas,
					256.0f, gi_scene.lighting_capture.specular_atlas.mip_views[mip]
				);
			}
			draw_texture(frame_data.linear_sampler, "Split-Sum BRDF LUT", gi_scene.lighting_capture.brdf_lut, 256.0f);
			if (bruneton_atmosphere_pass.has_precomputed)
			{
				draw_texture(frame_data.linear_sampler, "Atmosphere: Transmittance",
					bruneton_atmosphere_pass.transmittance_pass.get_color_output(0), 256.0f);
				draw_texture(frame_data.linear_sampler, "Atmosphere: Irradiance",
					bruneton_atmosphere_pass.irradiance_pass.get_color_output(1), 256.0f);
				GpuImage& scattering = bruneton_atmosphere_pass.scattering_pass.get_color_output(2);
				draw_texture(frame_data.linear_sampler, "Atmosphere: Scattering Ground Layer",
					scattering, 256.0f, scattering.layer_views[0]);
			}
			if (ImGui::TreeNode("Cloud System Debug Views"))
			{
				ImGui::Text("Active layers: %d  Temporal: %s",
					state.clouds.active_layer_count,
					CloudPass::pass.history_valid ? "valid" : "invalid");
				draw_texture(frame_data.linear_sampler, "Current density / radiance",
					get_render_pass(ERenderPass::CloudRaymarch).get_color_output(0), 256.0f);
				draw_texture(frame_data.linear_sampler, "Weighted depth / opacity / wind / geometry mask",
					get_render_pass(ERenderPass::CloudRaymarch).get_color_output(1), 256.0f);
				draw_texture(frame_data.linear_sampler, "Temporal radiance / transmittance",
					get_render_pass_entry(ERenderPass::CloudTemporal).final_pass().get_color_output(0), 256.0f);
				draw_texture(frame_data.linear_sampler, "Temporal weighted depth / validity",
					get_render_pass_entry(ERenderPass::CloudTemporal).final_pass().get_color_output(1), 256.0f);
				draw_texture(frame_data.linear_sampler, "Reactive mask",
					get_render_pass(ERenderPass::CloudComposite).get_color_output(2), 256.0f);
				draw_texture(frame_data.linear_sampler, "Shadow transmittance",
					get_render_pass(ERenderPass::CloudShadow).get_color_output(0), 256.0f);
				if (CloudPass::pass.caches_generated)
				{
					draw_texture(frame_data.linear_sampler, "Base shape slice 0",
						CloudPass::pass.base_shape, 256.0f, CloudPass::pass.base_shape.layer_views[0]);
					draw_texture(frame_data.linear_sampler, "Erosion slice 0",
						CloudPass::pass.erosion, 256.0f, CloudPass::pass.erosion.layer_views[0]);
					for (i32 layer_index = 0;
						layer_index < MIN(state.clouds.active_layer_count, MAX_CLOUD_LAYERS);
						++layer_index)
					{
						char label[64];
						snprintf(label, sizeof(label), "Weather field layer %d", layer_index + 1);
						draw_texture(frame_data.linear_sampler, label, CloudPass::pass.weather,
							256.0f, CloudPass::pass.weather.layer_views[layer_index]);
					}
				}
				ImGui::TreePop();
			}
			if (state.bloom.enable && state.bloom.intensity > 0.0f)
			{
				GpuImage& bloom_pyramid = BloomPass::get_pyramid();
				for (i32 mip = 0; mip < BloomPass::get_effective_mip_count(); ++mip)
				{
					char label[96];
					snprintf(
						label,
						sizeof(label),
						"Bloom Mip %d: %u x %u",
						mip,
						BloomPass::mip_extent(bloom_pyramid.extent.width, (u32)mip),
						BloomPass::mip_extent(bloom_pyramid.extent.height, (u32)mip));
					draw_texture(
						frame_data.linear_sampler,
						label,
						bloom_pyramid,
						256.0f,
						BloomPass::mip_view(mip));
				}
			}
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

	inline void draw_local_tonemapping_debug(
		const State& state,
		VkImageView final_tonemapped_view)
	{
		if (!state.debug_ui.show_local_tonemapping_debug
			|| !state.tonemapping.local_enabled)
		{
			return;
		}
		const TonemappingDebugViewData& debug =
			tonemapping_pass_get_debug_view_data();
		if (!debug.ready || final_tonemapped_view == VK_NULL_HANDLE)
		{
			return;
		}

		ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImDrawList* draw_list = ImGui::GetBackgroundDrawList(viewport);
		const ImVec2 origin = viewport->WorkPos;
		const ImVec2 available(
			MIN(viewport->WorkSize.x, (f32)state.window.width),
			MIN(viewport->WorkSize.y, (f32)state.window.height));
		const f32 margin = 10.0f;
		const f32 gap = 6.0f;
		const f32 padding = 5.0f;
		const f32 label_height = 28.0f;
		const f32 label_font_size = 9.0f;
		const f32 section_height = 20.0f;
		const f32 overview_height = CLAMP(available.y * 0.12f, 70.0f, 120.0f);
		const f32 aspect = debug.base_height > 0
			? (f32)debug.base_width / (f32)debug.base_height
			: 16.0f / 9.0f;
		const f32 max_card_width =
			(available.x - 2.0f * margin - 3.0f * gap) / 4.0f;
		const f32 max_image_height =
			(available.y - 2.0f * margin - section_height - overview_height
				- 5.0f * label_height - 6.0f * gap - 10.0f * padding) / 5.0f;
		const f32 image_width = MAX(48.0f, MIN(
			max_card_width - 2.0f * padding,
			MAX(32.0f, max_image_height) * aspect));
		const f32 image_height = image_width / MAX(aspect, 0.01f);
		const f32 card_width = image_width + 2.0f * padding;
		const f32 card_height = image_height + label_height + 2.0f * padding;
		const f32 atlas_width = 4.0f * card_width + 3.0f * gap;
		const f32 start_x = origin.x + MAX(margin, (available.x - atlas_width) * 0.5f);
		const f32 start_y = origin.y + margin + section_height;
		const ImU32 card_color = IM_COL32(8, 10, 14, 205);
		const ImU32 border_color = IM_COL32(160, 170, 185, 105);
		const ImU32 text_color = IM_COL32_WHITE;

		char header[192];
		snprintf(
			header,
			sizeof(header),
			"LOCAL TONEMAPPING  |  selected mip %d  |  reconstruction %d -> %d",
			debug.selected_full_mip,
			debug.coarsest_full_mip,
			debug.reconstruction_full_mip);
		draw_list->AddText(ImVec2(start_x, origin.y + margin), text_color, header);

		const i32 selected_internal_mip = debug.selected_full_mip - 1;
		const i32 reconstruction_internal_mip = debug.reconstruction_full_mip - 1;
		const u32 selected_width = tonemapping_local_mip_extent(
			debug.base_width, (u32)selected_internal_mip);
		const u32 selected_height = tonemapping_local_mip_extent(
			debug.base_height, (u32)selected_internal_mip);
		const u32 transfer_width = tonemapping_local_mip_extent(
			debug.base_width, (u32)reconstruction_internal_mip);
		const u32 transfer_height = tonemapping_local_mip_extent(
			debug.base_height, (u32)reconstruction_internal_mip);

		const auto draw_card = [&](i32 row, i32 column, VkImageView view, const char* label,
			const char* unavailable_text = nullptr)
		{
			const ImVec2 top_left(
				start_x + (f32)column * (card_width + gap),
				start_y + (f32)row * (card_height + gap));
			const ImVec2 bottom_right(top_left.x + card_width, top_left.y + card_height);
			draw_list->AddRectFilled(top_left, bottom_right, card_color, 3.0f);
			draw_list->AddRect(top_left, bottom_right, border_color, 3.0f);
			draw_list->AddText(
				ImGui::GetFont(),
				label_font_size,
				ImVec2(top_left.x + padding, top_left.y + padding),
				text_color,
				label,
				nullptr,
				image_width);
			const ImVec2 image_min(
				top_left.x + padding,
				top_left.y + padding + label_height);
			const ImVec2 image_max(image_min.x + image_width, image_min.y + image_height);
			if (view != VK_NULL_HANDLE)
			{
				draw_list->AddImage(texture(frame_data.linear_sampler, view), image_min, image_max);
			}
			else if (unavailable_text)
			{
				const ImVec2 text_size = ImGui::CalcTextSize(unavailable_text);
				draw_list->AddText(
					ImVec2(
						image_min.x + MAX(0.0f, (image_width - text_size.x) * 0.5f),
						image_min.y + MAX(0.0f, (image_height - text_size.y) * 0.5f)),
					IM_COL32(190, 195, 205, 255),
					unavailable_text);
			}
		};

		char labels[18][160] = {};
		snprintf(labels[0], sizeof(labels[0]), "Highlight lightness  -%.2f EV  |  mip %d  %ux%u",
			state.tonemapping.local_highlight_recovery, debug.selected_full_mip,
			selected_width, selected_height);
		snprintf(labels[1], sizeof(labels[1]), "Normal lightness  0 EV  |  mip %d  %ux%u",
			debug.selected_full_mip, selected_width, selected_height);
		snprintf(labels[2], sizeof(labels[2]), "Shadow lightness  +%.2f EV  |  mip %d  %ux%u",
			state.tonemapping.local_shadow_recovery, debug.selected_full_mip,
			selected_width, selected_height);
		snprintf(labels[3], sizeof(labels[3]), "Selected reconstruction  |  mip %d  %ux%u",
			debug.selected_full_mip, selected_width, selected_height);
		snprintf(labels[4], sizeof(labels[4]), "Highlight weight  |  mip %d  %ux%u",
			debug.selected_full_mip, selected_width, selected_height);
		snprintf(labels[5], sizeof(labels[5]), "Normal weight  |  mip %d  %ux%u",
			debug.selected_full_mip, selected_width, selected_height);
		snprintf(labels[6], sizeof(labels[6]), "Shadow weight  |  mip %d  %ux%u",
			debug.selected_full_mip, selected_width, selected_height);
		snprintf(labels[7], sizeof(labels[7]), "Transfer guide (normal)  |  mip %d  %ux%u",
			debug.reconstruction_full_mip, transfer_width, transfer_height);
		snprintf(labels[8], sizeof(labels[8]), "Highlight Laplacian  |  -1 black, 0 gray, +1 white");
		snprintf(labels[9], sizeof(labels[9]), "Normal Laplacian  |  -1 black, 0 gray, +1 white");
		snprintf(labels[10], sizeof(labels[10]), "Shadow Laplacian  |  -1 black, 0 gray, +1 white");
		snprintf(labels[11], sizeof(labels[11]), "Fused transfer target  |  mip %d  %ux%u",
			debug.reconstruction_full_mip, transfer_width, transfer_height);
		snprintf(labels[12], sizeof(labels[12]), "Full-res source lightness  |  %dx%d",
			state.window.render_width, state.window.render_height);
		snprintf(labels[13], sizeof(labels[13]), "Full-res guided target  |  %dx%d",
			state.window.render_width, state.window.render_height);
		snprintf(labels[14], sizeof(labels[14]), "Final applied local EV  |  -4 black, 0 gray, +4 white");
		snprintf(labels[15], sizeof(labels[15]), "Final tonemapped output (pre-FXAA)  |  %dx%d",
			state.window.render_width, state.window.render_height);
		snprintf(labels[16], sizeof(labels[16]), "Geometry coverage  |  mip %d  %ux%u",
			debug.selected_full_mip, selected_width, selected_height);
		snprintf(labels[17], sizeof(labels[17]), "Boundary local strength  |  sky and silhouettes are black");

		for (i32 channel = 0; channel < 3; ++channel)
		{
			draw_card(0, channel, debug.exposure[channel], labels[channel]);
			draw_card(1, channel, debug.weights[channel], labels[4 + channel]);
			draw_card(
				2,
				channel,
				debug.laplacians[channel],
				labels[8 + channel],
				"Coarsest Gaussian - no Laplacian");
		}
		draw_card(0, 3, debug.selected_reconstruction, labels[3]);
		draw_card(1, 3, debug.transfer_guide, labels[7]);
		draw_card(2, 3, debug.transfer_fused, labels[11]);
		draw_card(3, 0, debug.guided[0], labels[12]);
		draw_card(3, 1, debug.guided[1], labels[13]);
		draw_card(3, 2, debug.guided[2], labels[14]);
		draw_card(3, 3, final_tonemapped_view, labels[15]);
		draw_card(4, 0, debug.geometry_coverage, labels[16]);
		draw_card(4, 1, debug.boundary_suppression, labels[17]);

		if (debug.reconstruction_pyramid)
		{
			const i32 overview_count =
				debug.coarsest_full_mip - debug.reconstruction_full_mip + 1;
			const f32 overview_top =
				start_y + 5.0f * (card_height + gap) + 2.0f;
			const f32 overview_label_height = 13.0f;
			const f32 overview_image_width = MIN(
				card_width,
				(available.x - 2.0f * margin - (f32)(overview_count - 1) * gap)
					/ (f32)MAX(overview_count, 1));
			const f32 overview_image_height = MIN(
				overview_height - overview_label_height - 2.0f * padding,
				overview_image_width / MAX(aspect, 0.01f));
			for (i32 index = 0; index < overview_count; ++index)
			{
				const i32 full_mip = debug.coarsest_full_mip - index;
				const u32 mip = (u32)(full_mip - 1);
				const u32 width = tonemapping_local_mip_extent(debug.base_width, mip);
				const u32 height = tonemapping_local_mip_extent(debug.base_height, mip);
				const ImVec2 top_left(
					origin.x + margin + (f32)index * (overview_image_width + gap),
					overview_top);
				const ImVec2 bottom_right(
					top_left.x + overview_image_width,
					top_left.y + overview_label_height + overview_image_height + padding);
				draw_list->AddRectFilled(top_left, bottom_right, card_color, 3.0f);
				draw_list->AddRect(top_left, bottom_right, border_color, 3.0f);
				char overview_label[96];
				snprintf(overview_label, sizeof(overview_label),
					"Recon M%d  %ux%u", full_mip, width, height);
				draw_list->AddText(
					ImGui::GetFont(),
					9.0f,
					ImVec2(top_left.x + padding, top_left.y + 2.0f),
					text_color,
					overview_label,
					nullptr,
					MAX(overview_image_width - 2.0f * padding, 1.0f));
				draw_list->AddImage(
					texture(
						frame_data.linear_sampler,
						tonemapping_mip_view(
							*debug.reconstruction_pyramid, mip)),
					ImVec2(top_left.x, top_left.y + overview_label_height),
					ImVec2(
						top_left.x + overview_image_width,
						top_left.y + overview_label_height + overview_image_height));
			}
		}
	}

	inline void render(VulkanContext* ctx, f32 in_paper_white_scale)
	{
		ImGui::Render();

		// ImGui style colors are authored in display-space sRGB. Convert the
		// packed vertex tint to linear before the Vulkan backend uploads it, then
		// place UI white at the active output's paper-white level.
		// Texture RGB remains untouched (image vertices normally use a white
		// tint), and alpha stays linear for correct attachment blending.
		ImDrawData* draw_data = ImGui::GetDrawData();
		for (ImDrawList* draw_list : draw_data->CmdLists)
		{
			for (ImDrawVert& vertex : draw_list->VtxBuffer)
			{
				const auto linearize_channel = [in_paper_white_scale](u32 in_channel) -> u32
				{
					const f32 srgb = (f32) in_channel / 255.0f;
					const f32 linear = srgb <= 0.04045f
						? srgb / 12.92f
						: powf((srgb + 0.055f) / 1.055f, 2.4f);
					return (u32) (CLAMP(linear * in_paper_white_scale, 0.0f, 1.0f) * 255.0f + 0.5f);
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

		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		ImGui_ImplVulkan_RenderDrawData(draw_data, command_buffer);
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
	inline void draw_local_tonemapping_debug(const State&, VkImageView) {}
	inline void render(VulkanContext*, f32) {}
	inline void shutdown() {}
}

#endif
