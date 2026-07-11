#pragma once

#include <map>

#include "imgui.h"
#include "extern/imgui/backends/imgui_impl_glfw.h"
#include "extern/imgui/backends/imgui_impl_vulkan.h"

namespace ImGuiLayer
{
	inline bool initialized = false;
	inline std::map<VkImageView, VkDescriptorSet> texture_sets;

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
		init_info.MinImageCount = 2;
		init_info.ImageCount = (u32) ctx->swapchain_images.size();
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
		for (const auto& [view, set] : texture_sets)
		{
			ImGui_ImplVulkan_RemoveTexture(set);
		}
		texture_sets.clear();
	}

	inline ImTextureRef texture(VkSampler sampler, VkImageView view)
	{
		if (view == VK_NULL_HANDLE) { return ImTextureRef(); }
		auto found = texture_sets.find(view);
		if (found == texture_sets.end())
		{
			VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			found = texture_sets.emplace(view, set).first;
		}
		return ImTextureRef((ImTextureID) (uintptr_t) found->second);
	}

	inline void begin_frame()
	{
		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	}

	inline void draw_import_stats(State& state)
	{
		const auto& stats = state.debug_ui.last_import;
		if (ImGui::CollapsingHeader("Live-link Import", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Text("Update %llu  Bytes %llu  Generation %.4f ms",
				(unsigned long long) stats.update_index, (unsigned long long) stats.byte_count,
				stats.generation_seconds * 1000.0);
			ImGui::Text("Objects %d  Deleted %d  Materials %d  Images %d (%.2f MiB)",
				stats.object_count, stats.deleted_object_count, stats.material_count, stats.image_count,
				(double) stats.image_byte_count / (1024.0 * 1024.0));
			ImGui::Text("Meshes %d (%d vertices / %d indices)  Skinned %d",
				stats.mesh_count, stats.mesh_vertex_count, stats.mesh_index_count, stats.skinned_mesh_count);
			ImGui::Text("Lights %d  Armatures %d  Animations %d",
				stats.light_count, stats.armature_count, stats.animation_count);
			ImGui::Text("Reset: %s  History: %zu", stats.reset ? "true" : "false",
				state.debug_ui.import_history.length());
		}
	}

	inline void draw_profiler(State& state)
	{
		if (!state.debug_ui.show_profiler) { return; }
		ImGui::Begin("CPU / GPU Profiler", &state.debug_ui.show_profiler);
		ImGui::Checkbox("Freeze", &state.debug_ui.freeze_profiler);
		if (ImGui::BeginTable("TimingTable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
		{
			ImGui::TableSetupColumn("CPU scope"); ImGui::TableSetupColumn("ms");
			for (const CpuTimingEvent& event : cpu_timings_get_display_frame(state.debug_ui.freeze_profiler, 0))
			{
				ImGui::TableNextRow(); ImGui::TableNextColumn();
				ImGui::Indent((f32) event.depth * 10.0f); ImGui::TextUnformatted(event.name); ImGui::Unindent((f32) event.depth * 10.0f);
				ImGui::TableNextColumn(); ImGui::Text("%.3f", event.elapsed_ms);
			}
			ImGui::EndTable();
		}
		GpuTimingFrame gpu_frame;
		if (gpu_timings_copy_display_frame(state.debug_ui.freeze_profiler, 0, gpu_frame)
			&& ImGui::BeginTable("GpuTimingTable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
		{
			ImGui::TableSetupColumn("GPU scope"); ImGui::TableSetupColumn("ms");
			for (const GpuTimingEvent& event : gpu_frame.events)
			{
				ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(event.name);
				ImGui::TableNextColumn(); ImGui::Text("%.3f", event.elapsed_ms);
			}
			ImGui::EndTable();
		}
		ImGui::End();
	}

	inline void draw_texture(VkSampler sampler, const char* label, VkImageView view, f32 width = 240.0f)
	{
		ImGui::TextUnformatted(label);
		ImGui::Image(texture(sampler, view), ImVec2(width, width * 0.5625f));
	}

	inline void draw_texture_viewer(State& state, GI_Scene& gi_scene)
	{
		if (!state.debug_ui.show_texture_viewer) { return; }
		ImGui::Begin("Render Texture Viewer", &state.debug_ui.show_texture_viewer);
		RenderPass& geometry = get_render_pass(ERenderPass::Geometry);
		for (i32 output = 0; output < Render::GBUFFER_OUTPUT_COUNT; ++output)
		{
			char label[32]; snprintf(label, sizeof(label), "G-buffer %d", output);
			draw_texture(frame_data.linear_sampler, label, geometry.get_color_output(output).view);
		}
		draw_texture(frame_data.linear_sampler, "GI Lighting Atlas", gi_scene_get_octahedral_lighting_view(gi_scene));
		draw_texture(frame_data.linear_sampler, "GI Depth Atlas", gi_scene_get_octahedral_depth_view(gi_scene));
		draw_texture(frame_data.linear_sampler, "Baked Sky", sky_pass.bake_render_pass.get_color_output(0).view);
		draw_texture(frame_data.linear_sampler, "SSAO", get_render_pass(ERenderPass::SSAO_Blur).get_color_output(0).view);
		draw_texture(frame_data.linear_sampler, "Contact Shadows", get_render_pass(ERenderPass::ScreenSpaceShadows).get_color_output(0).view);
		GpuImage& shadow_image = get_render_pass(ERenderPass::ShadowDepth).get_color_output(0);
		for (i32 cascade = 0; cascade < state.shadow.num_cascades && cascade < (i32) shadow_image.layer_views.length(); ++cascade)
		{
			char label[32]; snprintf(label, sizeof(label), "Shadow Cascade %d", cascade);
			draw_texture(frame_data.linear_sampler, label, shadow_image.layer_views[cascade]);
		}
		if (state.images.items.length() > 0)
		{
			static i32 image_index = 0;
			image_index = CLAMP(image_index, 0, (i32) state.images.items.length() - 1);
			ImGui::SliderInt("Imported Image", &image_index, 0, (i32) state.images.items.length() - 1);
			draw_texture(frame_data.linear_sampler, "Imported Image", state.images.items[image_index].view);
		}
		ImGui::End();
	}

	inline void draw_controls(State& state, GI_Scene& gi_scene)
	{
		if (!state.debug_ui.visible) { return; }
		ImGui::Begin("DEBUG");
		ImGuiIO& io = ImGui::GetIO();
		state.debug_ui.fps = io.Framerate;
		state.debug_ui.frame_time_ms = io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f;
		ImGui::Text("%dx%d render %dx%d | %.1f FPS | %.2f ms",
			state.window.width, state.window.height, state.window.render_width, state.window.render_height,
			state.debug_ui.fps, state.debug_ui.frame_time_ms);
		ImGui::Checkbox("Profiler", &state.debug_ui.show_profiler);
		ImGui::SameLine(); ImGui::Checkbox("Texture Viewer", &state.debug_ui.show_texture_viewer);
		draw_import_stats(state);

		if (ImGui::CollapsingHeader("Animation"))
		{
			ImGui::Checkbox("Play", &state.animation.is_playing);
			ImGui::SliderFloat("Playback Rate", &state.animation.playback_rate, 0.0f, 4.0f);
		}
		if (ImGui::CollapsingHeader("Tessellation", ImGuiTreeNodeFlags_DefaultOpen))
		{
			bool changed = ImGui::Checkbox("Enable Tessellation", &state.tessellation.enabled);
			changed |= ImGui::Combo("Mode", (i32*) &state.tessellation.mode, ETessellationModeNames, (i32) ETessellationMode::MAX);
			changed |= ImGui::SliderInt("Fixed Factor", &state.tessellation.fixed_factor, 1, state.tessellation.max_factor);
			changed |= ImGui::SliderInt("Max Factor", &state.tessellation.max_factor, 1, (i32) Tessellation::MAX_FACTOR);
			ImGui::SliderFloat("Target Pixels / Segment", &state.tessellation.target_pixels_per_segment, 1.0f, 64.0f);
			ImGui::SliderFloat("Phong Strength", &state.tessellation.phong_strength, 0.0f, 1.0f);
			ImGui::Checkbox("Virtual Patches", &state.tessellation.virtual_patches_enabled);
			ImGui::SliderInt("Virtual Patch Depth", &state.tessellation.virtual_patch_max_depth, 0, 4);
			ImGui::SliderFloat("Bounds Padding", &state.tessellation.bounds_padding, 0.0f, 10.0f);
			ImGui::Text("Meshes %d | Patches %d | Vertices %d | Indices %d | Overflow %d",
				state.tessellation.mesh_count, state.tessellation.patch_count,
				state.tessellation.generated_vertex_count, state.tessellation.generated_index_count,
				state.tessellation.overflowed_mesh_count);
			if (changed) { state.gi.is_updating = true; }
		}
		if (ImGui::CollapsingHeader("Lighting / GI", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Checkbox("Sky", &state.sky.rendering_enable);
			ImGui::Checkbox("Direct Lighting", &state.lighting.direct_enable);
			ImGui::Checkbox("GI", &state.gi.enable);
			ImGui::Checkbox("Probe Occlusion", &state.gi.probe_occlusion);
			if (ImGui::SliderInt("Octree Depth", &state.gi.octree_depth, GI_Scene::min_octree_depth, GI_Scene::max_octree_depth))
				state.gi.layout_dirty = true;
			ImGui::Combo("Radiance Mode", (i32*) &state.gi.probe_radiance_mode, "Octahedral\0SH9\0SG9\0");
			ImGui::Combo("Occlusion Mode", (i32*) &state.gi.probe_occlusion_mode, "Chebyshev\0EVRP4\0");
			ImGui::SliderFloat("GI Intensity", &state.gi.intensity, 0.0f, 10.0f);
			ImGui::Checkbox("Render Sky To Probes", &state.gi.render_sky_to_probes);
			ImGui::Checkbox("Compute Irradiance", &state.gi.compute_irradiance);
			ImGui::Checkbox("Show Probes", &state.gi.show_probes);
			ImGui::Combo("Probe Visualization", (i32*) &state.gi.probe_vis_mode,
				"Irradiance\0SH9\0SG9\0Radial Depth\0Depth Squared\0EVRP Moment\0");
			ImGui::Checkbox("Probe Isolation", &state.gi.probe_isolation_enable);
			if (ImGui::Button("Update GI Probes")) { state.gi.is_updating = true; }
			ImGui::Text("Nodes %zu | Cells %zu | Probes %zu | Updating %s",
				gi_scene.octree_nodes.length(), gi_scene.cells.length(), gi_scene.probes.length(),
				state.gi.is_updating ? "yes" : "no");
		}
		if (ImGui::CollapsingHeader("Shadows"))
		{
			ImGui::Checkbox("Render Shadows", &state.shadow.rendering_enable);
			ImGui::Checkbox("Blur", &state.shadow.blur_enable);
			ImGui::Checkbox("Freeze", &state.shadow.depth_freeze);
			if (ImGui::Button("Recapture")) { state.shadow.force_recapture = true; }
			ImGui::SliderInt("Cascades", &state.shadow.num_cascades, 1, MAX_SHADOW_CASCADES);
			ImGui::Combo("Placement", (i32*) &state.shadow.cascade_placement_mode, "Frustum\0Centered Squares\0");
			ImGui::Checkbox("Cascade Debug", &state.shadow.debug_show_cascade_selection);
			ImGui::Checkbox("Contact Shadows", &state.shadow.screen_space.enable);
		}
		if (ImGui::CollapsingHeader("Post / Wireframe"))
		{
			ImGui::Checkbox("SSAO", &state.ssao.enable);
			ImGui::Checkbox("Fog", &state.fog.debug_active);
			ImGui::Checkbox("DOF", &state.dof.enable);
			ImGui::Checkbox("TAA", &state.temporal_aa.enable);
			ImGui::Checkbox("FXAA", &state.temporal_aa.enable_fxaa);
			ImGui::SliderFloat("Exposure", &state.tonemapping.exposure_bias, -5.0f, 5.0f);
			ImGui::Checkbox("Shaded Wireframe", &state.wireframe.shaded_wireframe);
			ImGui::SliderFloat("Wire Width", &state.wireframe.width, 0.5f, 4.0f);
			ImGui::SliderFloat("Wire Opacity", &state.wireframe.opacity, 0.0f, 1.0f);
			ImGui::ColorEdit3("Wire Color", &state.wireframe.color.X);
		}
		ImGui::End();
		draw_profiler(state);
		draw_texture_viewer(state, gi_scene);

		ImDrawList* foreground = ImGui::GetForegroundDrawList();
		ImVec2 pos(15.0f, 15.0f);
		if (!state.runtime.blender_data_loaded) { foreground->AddText(pos, IM_COL32_WHITE, "Waiting on data from Blender"); pos.y += 20.0f; }
		if (state.debug_camera.active) { foreground->AddText(pos, IM_COL32_WHITE, "Debug Camera Active"); pos.y += 20.0f; }
		if (!state.runtime.is_simulating) { foreground->AddText(pos, IM_COL32_WHITE, "Simulation Paused"); }
	}

	inline void render(VulkanContext* ctx)
	{
		ImGui::Render();
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
		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command_buffer);
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
