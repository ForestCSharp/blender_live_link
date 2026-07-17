#pragma once

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI

#include <algorithm>
#include <cstddef>

#include "imgui/imgui.h"
#include "state/state.h"

static void stats_ui_cell_label(const char* label)
{
	ImGui::TableNextColumn();
	ImGui::TextDisabled("%s", label);
}

static void stats_ui_cell_i32(const char* label, i32 value)
{
	stats_ui_cell_label(label);
	ImGui::TableNextColumn();
	ImGui::Text("%i", value);
}

static void stats_ui_cell_u64(const char* label, u64 value)
{
	stats_ui_cell_label(label);
	ImGui::TableNextColumn();
	ImGui::Text("%llu", (unsigned long long)value);
}

static void stats_ui_cell_seconds(const char* label, f64 value)
{
	stats_ui_cell_label(label);
	ImGui::TableNextColumn();
	ImGui::Text("%.6f s", value);
}

static void stats_ui_cell_size(const char* label, size_t value)
{
	stats_ui_cell_label(label);
	ImGui::TableNextColumn();
	ImGui::Text("%zu", value);
}

static void stats_ui_cell_bool(const char* label, bool value)
{
	stats_ui_cell_label(label);
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(value ? "true" : "false");
}

static void stats_ui_table_columns()
{
	ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed);
	ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed);
}

static void draw_stats_ui(State& state)
{
	const auto& previous = state.data_oriented.previous_frame;
	const ImGuiTableFlags stats_table_flags =
		ImGuiTableFlags_SizingStretchProp |
		ImGuiTableFlags_RowBg |
		ImGuiTableFlags_BordersInnerV |
		ImGuiTableFlags_NoSavedSettings;

	if (ImGui::CollapsingHeader("Update Stats", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const i32 import_history_count = (i32)state.data_oriented.import_history.length();
		if (import_history_count > 0)
		{
			state.data_oriented.selected_import_history_index = CLAMP(
				state.data_oriented.selected_import_history_index,
				0,
				import_history_count - 1
			);

			ImGui::SetNextItemWidth(180.0f);
			ImGui::SliderInt(
				"Update",
				&state.data_oriented.selected_import_history_index,
				0,
				import_history_count - 1
			);
		}
		else
		{
			state.data_oriented.selected_import_history_index = -1;
			ImGui::TextDisabled("No live-link imports received");
		}

		const auto& import = import_history_count > 0
			? state.data_oriented.import_history[state.data_oriented.selected_import_history_index]
			: state.data_oriented.last_import;

		ImGui::TextDisabled("Live-link import");
		if (ImGui::BeginTable("##LiveLinkImportStats", 4, stats_table_flags))
		{
			stats_ui_table_columns();

			ImGui::TableNextRow();
			stats_ui_cell_u64("Update", import.update_index);
			stats_ui_cell_u64("Bytes", import.byte_count);

			ImGui::TableNextRow();
			stats_ui_cell_seconds("Generation Time", import.generation_seconds);
			stats_ui_cell_bool("Reset", import.reset);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Objects", import.object_count);
			stats_ui_cell_i32("Deleted", import.deleted_object_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Meshes", import.mesh_count);
			stats_ui_cell_i32("Skinned Meshes", import.skinned_mesh_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Vertices", import.mesh_vertex_count);
			stats_ui_cell_i32("Indices", import.mesh_index_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Lights", import.light_count);
			stats_ui_cell_i32("Armatures", import.armature_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Animations", import.animation_count);
			stats_ui_cell_i32("Matrices", import.animation_matrix_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Materials", import.material_count);
			stats_ui_cell_i32("Images", import.image_count);

			ImGui::TableNextRow();
			stats_ui_cell_u64("Image Bytes", import.image_byte_count);
			stats_ui_cell_i32("Malformed", import.malformed_object_count);
			ImGui::EndTable();
		}

		ImGui::Spacing();
		ImGui::TextDisabled("Last completed frame");
		if (ImGui::BeginTable("##FrameUpdateStats", 4, stats_table_flags))
		{
			stats_ui_table_columns();

			ImGui::TableNextRow();
			stats_ui_cell_i32("Updated Objects", previous.live_link_updated_objects);
			stats_ui_cell_i32("Deleted Objects", previous.live_link_deleted_objects);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Reset Events", previous.live_link_reset_count);
			stats_ui_cell_i32("Object Scans", previous.object_update_scan_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Storage Updates", previous.object_update_storage_updates);
			stats_ui_cell_i32("Mesh Dirty", previous.object_update_mesh_dirty_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Armature Candidates", previous.animation_armature_candidates);
			stats_ui_cell_i32("Armatures Updated", previous.animation_armatures_updated);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Skin Candidates", previous.animation_skinned_mesh_candidates);
			stats_ui_cell_i32("Skin Matrix Uploads", previous.animation_skin_matrix_uploads);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Lighting Processed", previous.lighting_processed_count);
			stats_ui_cell_i32("Lighting Candidates", previous.lighting_candidate_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Skinning Updated", previous.gpu_skinning_updated_count);
			stats_ui_cell_i32("Skinning Candidates", previous.gpu_skinning_candidate_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Tessellation Processed", previous.tessellation_processed_count);
			stats_ui_cell_i32("Tessellation Candidates", previous.tessellation_candidate_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Draw Calls", previous.draw_calls);
			stats_ui_cell_i32("Draw Meshes", previous.draw_mesh_count);

			ImGui::EndTable();
		}
	}

	if (ImGui::CollapsingHeader("Scene Stats", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::BeginTable("##SceneIndexStats", 4, stats_table_flags))
		{
			stats_ui_table_columns();

			ImGui::TableNextRow();
			stats_ui_cell_size("Total Objects", state.scene.objects.size());
			stats_ui_cell_i32("Indexed Objects", previous.scene_object_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Mesh Objects", previous.mesh_object_count);
			stats_ui_cell_i32("Skinned Meshes", previous.skinned_mesh_object_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Light Objects", previous.light_object_count);
			stats_ui_cell_i32("Armatures", previous.armature_object_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Point Lights", state.lighting.point_lights.length());
			stats_ui_cell_i32("Spot Lights", state.lighting.spot_lights.length());

			ImGui::TableNextRow();
			stats_ui_cell_i32("Sun Lights", state.lighting.sun_lights.length());
			ImGui::EndTable();
		}

		ImGui::Spacing();
		ImGui::TextDisabled("Last completed frame culling");
		if (ImGui::BeginTable("##SceneCullingStats", 4, stats_table_flags))
		{
			stats_ui_table_columns();
			const i32 cull_count = std::max(0, previous.cull_candidate_count - previous.cull_visible_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Calls", previous.cull_calls);
			stats_ui_cell_i32("Candidates", previous.cull_candidate_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Visible", previous.cull_visible_count);
			stats_ui_cell_i32("Culled", cull_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Non-renderable", previous.cull_non_renderable_count);
			stats_ui_cell_i32("Hidden", previous.cull_visibility_count);

			ImGui::TableNextRow();
			stats_ui_cell_i32("Frustum", previous.cull_frustum_count);
			ImGui::EndTable();
		}
	}

	if (ImGui::CollapsingHeader("Vulkan / VMA", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const VulkanMemoryStats memory = vulkan_context_get_memory_stats(&state.vk);
		const VulkanMetrics& metrics = state.vk.metrics;
		if (ImGui::BeginTable("##VulkanMemoryStats", 4, stats_table_flags))
		{
			stats_ui_table_columns();
			ImGui::TableNextRow();
			stats_ui_cell_u64("Allocations", memory.allocation_count);
			stats_ui_cell_u64("Allocation Bytes", memory.allocation_bytes);
			ImGui::TableNextRow();
			stats_ui_cell_u64("Memory Blocks", memory.block_count);
			stats_ui_cell_u64("Block Bytes", memory.block_bytes);
			ImGui::TableNextRow();
			stats_ui_cell_u64("Device Usage", memory.usage_bytes);
			stats_ui_cell_u64("Device Budget", memory.budget_bytes);
			ImGui::TableNextRow();
			stats_ui_cell_u64("Draw Calls", metrics.draw_calls);
			stats_ui_cell_u64("Dispatch Calls", metrics.dispatch_calls);
			ImGui::TableNextRow();
			stats_ui_cell_u64("Descriptor Updates", metrics.descriptor_update_calls);
			stats_ui_cell_u64("Descriptors Written", metrics.descriptors_written);
			ImGui::TableNextRow();
			stats_ui_cell_u64("Uploaded Bytes", metrics.upload_bytes);
			stats_ui_cell_u64("Immediate Submits", metrics.immediate_submit_count);
			ImGui::TableNextRow();
			stats_ui_cell_u64("Queue Idle Waits", metrics.queue_wait_idle_count);
			stats_ui_cell_u64("Device Idle Waits", metrics.device_wait_idle_count);
			ImGui::EndTable();
		}
		ImGui::TextDisabled("Pipelines: %llu created in %.3f ms | cache hash: %016llx",
			(unsigned long long)metrics.pipeline_count,
			metrics.pipeline_creation_ms,
			(unsigned long long)state.vk.shader_build_hash);
		ImGui::TextDisabled("GPU: %s | queues G:%u P:%u | present: %s | screenshots: %s",
			state.vk.physical_device_properties.deviceName,
			state.vk.graphics_queue_family_index,
			state.vk.present_queue_family_index,
			vulkan_present_mode_name(state.vk.present_mode),
			state.vk.screenshot_supported ? "yes" : "no");
		ImGui::TextDisabled("Formats: scene %i | depth %i | G-buffer %i | shadow %i | SSAO %i",
			(i32)state.vk.capabilities.scene_color_format,
			(i32)state.vk.capabilities.scene_depth_format,
			(i32)state.vk.capabilities.gbuffer_format,
			(i32)state.vk.capabilities.shadow_moments_format,
			(i32)state.vk.capabilities.ssao_format);
	}
}

#endif // defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
