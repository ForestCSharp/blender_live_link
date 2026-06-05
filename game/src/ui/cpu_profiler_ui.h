#pragma once

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/timings.h"
#include "imgui/imgui.h"
#include "state/state.h"

static ImU32 cpu_profiler_color_for_depth(i32 in_depth)
{
	const ImU32 colors[] = {
		IM_COL32(0x4F, 0x8A, 0xC9, 255),
		IM_COL32(0xD1, 0x7A, 0x54, 255),
		IM_COL32(0x67, 0xA8, 0x65, 255),
		IM_COL32(0xB8, 0x70, 0xB8, 255),
		IM_COL32(0xC2, 0xA0, 0x4B, 255),
		IM_COL32(0x4B, 0xA7, 0xA0, 255),
	};
	return colors[in_depth % IM_ARRAYSIZE(colors)];
}

static constexpr f64 CPU_PROFILER_MIN_UNACCOUNTED_TIME_MS = 0.002;

static f64 cpu_profiler_ms_from_root(const CpuTimingEvent& in_root_event, u64 in_ticks)
{
	if (in_ticks <= in_root_event.start_ticks)
	{
		return 0.0;
	}

	return stm_ms(stm_diff(in_ticks, in_root_event.start_ticks));
}

static void draw_cpu_profiler_flamegraph(i32 in_display_frame_count)
{
	i32 max_depth = 0;
	for (i32 display_frame_index = 0; display_frame_index < in_display_frame_count; ++display_frame_index)
	{
		const StretchyBuffer<CpuTimingEvent>& events = cpu_timings_get_display_frame(state.debug_ui.freeze_profiler, display_frame_index);
		for (const CpuTimingEvent& event : events)
		{
			max_depth = std::max(max_depth, event.depth);
		}
	}
	if (state.debug_ui.show_profiler_unaccounted && max_depth >= 0)
	{
		++max_depth;
	}

	const f32 row_height = 24.0f;
	const f32 box_height = 20.0f;
	const f32 header_height = 22.0f;
	const f32 frame_gap = 0.0f;
	const f32 min_frame_width = 220.0f;
	const f32 min_box_width = 3.0f;
	const f32 visible_width = std::max(1.0f, ImGui::GetContentRegionAvail().x - 8.0f);
	const f32 base_frame_width = std::max(
		min_frame_width,
		(visible_width - frame_gap * (f32)(in_display_frame_count - 1)) / (f32)in_display_frame_count
	);
	const f32 graph_height = header_height + (max_depth + 1) * row_height + 8.0f;
	const f32 child_height = std::max(180.0f, std::min(420.0f, graph_height + 12.0f));

	ImGui::BeginChild("##CpuProfilerGraph", ImVec2(0.0f, child_height), true, ImGuiWindowFlags_HorizontalScrollbar);

	const f32 old_frame_width = base_frame_width * state.debug_ui.profiler_zoom;
	const f32 old_graph_width = old_frame_width * (f32)in_display_frame_count + frame_gap * (f32)(in_display_frame_count - 1);
	if (ImGui::IsWindowHovered())
	{
		const f32 mouse_wheel = ImGui::GetIO().MouseWheel;
		if (mouse_wheel != 0.0f)
		{
			const f32 old_scroll_x = ImGui::GetScrollX();
			const f32 mouse_x_in_window = ImGui::GetIO().MousePos.x - ImGui::GetWindowPos().x;
			const f32 mouse_content_x = old_scroll_x + mouse_x_in_window;
			const f32 mouse_content_t = old_graph_width > 0.0f
				? CLAMP(mouse_content_x / old_graph_width, 0.0f, 1.0f)
				: 0.0f;

			const f32 zoom_multiplier = (f32)std::pow(1.15f, mouse_wheel);
			state.debug_ui.profiler_zoom = CLAMP(state.debug_ui.profiler_zoom * zoom_multiplier, 0.25f, 128.0f);

			const f32 new_frame_width = base_frame_width * state.debug_ui.profiler_zoom;
			const f32 new_graph_width = new_frame_width * (f32)in_display_frame_count + frame_gap * (f32)(in_display_frame_count - 1);
			const f32 new_scroll_x = mouse_content_t * new_graph_width - mouse_x_in_window;
			ImGui::SetScrollX(std::max(0.0f, new_scroll_x));
		}
	}

	const f32 frame_width = base_frame_width * state.debug_ui.profiler_zoom;
	const f32 graph_width = frame_width * (f32)in_display_frame_count + frame_gap * (f32)(in_display_frame_count - 1);
	const ImVec2 graph_origin = ImGui::GetCursorScreenPos();
	const ImVec2 graph_max = ImVec2(graph_origin.x + graph_width, graph_origin.y + graph_height);
	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	draw_list->AddRectFilled(
		graph_origin,
		graph_max,
		IM_COL32(24, 26, 30, 255)
	);
	draw_list->AddRect(
		graph_origin,
		graph_max,
		IM_COL32(70, 74, 82, 255)
	);

	ImGui::PushClipRect(graph_origin, graph_max, true);
	for (i32 display_frame_index = 0; display_frame_index < in_display_frame_count; ++display_frame_index)
	{
		const i32 frame_source_index = in_display_frame_count - 1 - display_frame_index;
		const i32 frame_age = frame_source_index + 1;
		const StretchyBuffer<CpuTimingEvent>& events = cpu_timings_get_display_frame(state.debug_ui.freeze_profiler, frame_source_index);
		if (events.length() == 0)
		{
			continue;
		}

		const CpuTimingEvent& root_event = events[0];
		const f64 frame_elapsed_ms = std::max(root_event.elapsed_ms, 0.0001);
		const f32 frame_x = graph_origin.x + (frame_width + frame_gap) * (f32)display_frame_index;
		const f32 frame_y = graph_origin.y + header_height;

		char frame_label[64];
		snprintf(frame_label, sizeof(frame_label), "Frame -%d: %.3f ms", frame_age, root_event.elapsed_ms);
		draw_list->AddText(ImVec2(frame_x + 4.0f, graph_origin.y + 3.0f), IM_COL32(230, 232, 238, 255), frame_label);

		if (display_frame_index > 0)
		{
			draw_list->AddLine(
				ImVec2(frame_x, graph_origin.y),
				ImVec2(frame_x, graph_origin.y + graph_height),
				IM_COL32(70, 74, 82, 255)
			);
		}

		auto draw_timing_box = [&](const char* in_name, f64 in_start_offset_ms, f64 in_elapsed_ms, i32 in_depth, ImU32 in_fill_color, ImU32 in_text_color, const char* in_tooltip_context) -> bool
		{
			if (in_elapsed_ms <= 0.0)
			{
				return false;
			}

			const f32 raw_x = (f32)(in_start_offset_ms / frame_elapsed_ms) * frame_width;
			const f32 raw_w = (f32)(in_elapsed_ms / frame_elapsed_ms) * frame_width;

			const f32 x0 = frame_x + CLAMP(raw_x, 0.0f, frame_width);
			const f32 x1 = std::min(frame_x + frame_width, x0 + std::max(raw_w, min_box_width));
			const f32 y0 = frame_y + in_depth * row_height + 2.0f;
			const f32 y1 = y0 + box_height;
			if (x1 <= x0 || y1 <= y0)
			{
				return false;
			}

			const ImVec2 rect_min = ImVec2(x0, y0);
			const ImVec2 rect_max = ImVec2(x1, y1);
			draw_list->AddRectFilled(rect_min, rect_max, in_fill_color, 2.0f);
			draw_list->AddRect(rect_min, rect_max, IM_COL32(15, 17, 20, 180), 2.0f);

			char label[256];
			snprintf(label, sizeof(label), "%s %.3f ms", in_name, in_elapsed_ms);
			const ImVec2 text_size = ImGui::CalcTextSize(label);
			const f32 rect_width = rect_max.x - rect_min.x;
			if (text_size.x + 8.0f <= rect_width)
			{
				const ImVec2 text_pos = ImVec2(rect_min.x + 4.0f, rect_min.y + (box_height - text_size.y) * 0.5f);
				draw_list->AddText(text_pos, in_text_color, label);
			}

			ImGui::SetCursorScreenPos(rect_min);
			ImGui::InvisibleButton("scope", ImVec2(rect_max.x - rect_min.x, rect_max.y - rect_min.y));
			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::Text("Frame -%d", frame_age);
				if (in_tooltip_context)
				{
					ImGui::Text("%s", in_tooltip_context);
				}
				ImGui::Text("%s", in_name);
				ImGui::Text("%.3f ms", in_elapsed_ms);
				ImGui::Text("%.2f%% of frame", (in_elapsed_ms / frame_elapsed_ms) * 100.0);
				ImGui::EndTooltip();
			}

			return true;
		};

		if (state.debug_ui.show_profiler_unaccounted)
		{
			for (i32 parent_event_index = 0; parent_event_index < (i32)events.length(); ++parent_event_index)
			{
				const CpuTimingEvent& parent_event = events[parent_event_index];
				bool has_child_events = false;
				for (i32 child_event_index = 0; child_event_index < (i32)events.length(); ++child_event_index)
				{
					if (events[child_event_index].parent_index == parent_event_index)
					{
						has_child_events = true;
						break;
					}
				}
				if (!has_child_events)
				{
					continue;
				}

				u64 gap_start_ticks = parent_event.start_ticks;
				i32 unaccounted_gap_index = 0;

				for (i32 child_event_index = 0; child_event_index < (i32)events.length(); ++child_event_index)
				{
					const CpuTimingEvent& child_event = events[child_event_index];
					if (child_event.parent_index != parent_event_index)
					{
						continue;
					}

					if (child_event.start_ticks > gap_start_ticks)
					{
						const f64 self_start_offset_ms = cpu_profiler_ms_from_root(root_event, gap_start_ticks);
						const f64 self_elapsed_ms = stm_ms(stm_diff(child_event.start_ticks, gap_start_ticks));
						if (self_elapsed_ms >= CPU_PROFILER_MIN_UNACCOUNTED_TIME_MS)
						{
							char unaccounted_name[CPU_TIMINGS_MAX_NAME_LENGTH];
							snprintf(unaccounted_name, sizeof(unaccounted_name), "[unaccounted] %s", parent_event.name);

							ImGui::PushID("unaccounted");
							ImGui::PushID(display_frame_index);
							ImGui::PushID(parent_event_index);
							ImGui::PushID(unaccounted_gap_index++);
							draw_timing_box(
								unaccounted_name,
								self_start_offset_ms,
								self_elapsed_ms,
								parent_event.depth + 1,
								IM_COL32(82, 86, 94, 210),
								IM_COL32(235, 236, 240, 255),
								"Unaccounted time"
							);
							ImGui::PopID();
							ImGui::PopID();
							ImGui::PopID();
							ImGui::PopID();
						}
					}

					gap_start_ticks = std::max(gap_start_ticks, child_event.end_ticks);
				}

				if (parent_event.end_ticks > gap_start_ticks)
				{
					const f64 self_start_offset_ms = cpu_profiler_ms_from_root(root_event, gap_start_ticks);
					const f64 self_elapsed_ms = stm_ms(stm_diff(parent_event.end_ticks, gap_start_ticks));
					if (self_elapsed_ms >= CPU_PROFILER_MIN_UNACCOUNTED_TIME_MS)
					{
						char unaccounted_name[CPU_TIMINGS_MAX_NAME_LENGTH];
						snprintf(unaccounted_name, sizeof(unaccounted_name), "[unaccounted] %s", parent_event.name);

						ImGui::PushID("unaccounted");
						ImGui::PushID(display_frame_index);
						ImGui::PushID(parent_event_index);
						ImGui::PushID(unaccounted_gap_index++);
						draw_timing_box(
							unaccounted_name,
							self_start_offset_ms,
							self_elapsed_ms,
							parent_event.depth + 1,
							IM_COL32(82, 86, 94, 210),
							IM_COL32(235, 236, 240, 255),
							"Unaccounted time"
						);
						ImGui::PopID();
						ImGui::PopID();
						ImGui::PopID();
						ImGui::PopID();
					}
				}
			}
		}

		for (i32 event_index = 0; event_index < (i32)events.length(); ++event_index)
		{
			const CpuTimingEvent& event = events[event_index];
			const f64 start_offset_ms = event_index == 0
				? 0.0
				: stm_ms(stm_diff(event.start_ticks, root_event.start_ticks));

			ImGui::PushID(display_frame_index * 10000 + event_index);
			draw_timing_box(
				event.name,
				start_offset_ms,
				event.elapsed_ms,
				event.depth,
				cpu_profiler_color_for_depth(event.depth),
				IM_COL32(255, 255, 255, 255),
				nullptr
			);
			ImGui::PopID();
		}
	}
	ImGui::PopClipRect();

	ImGui::SetCursorScreenPos(graph_origin);
	ImGui::Dummy(ImVec2(graph_width, graph_height));
	ImGui::EndChild();
}

static void draw_cpu_profiler_window()
{
	if (!state.debug_ui.show_profiler)
	{
		return;
	}

	ImGui::SetNextWindowSize(ImVec2(900.0f, 560.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("CPU Profiler", &state.debug_ui.show_profiler))
	{
		ImGui::End();
		return;
	}

	ImGui::SliderInt(
		"Num Frames to display",
		&state.debug_ui.num_profiler_frames,
		1,
		CPU_TIMINGS_MAX_DISPLAY_FRAMES,
		"%d",
		ImGuiSliderFlags_ClampOnInput
	);
	ImGui::Checkbox("Freeze", &state.debug_ui.freeze_profiler);
	ImGui::Checkbox("Show Unaccounted", &state.debug_ui.show_profiler_unaccounted);
	if (ImGui::Button("Reset Zoom"))
	{
		state.debug_ui.profiler_zoom = 1.0f;
	}
	ImGui::SameLine();
	ImGui::Text("Zoom: %.2fx", state.debug_ui.profiler_zoom);

	const i32 available_frame_count = cpu_timings_get_display_frame_count(state.debug_ui.freeze_profiler);
	if (available_frame_count == 0)
	{
		ImGui::Text("No CPU timings captured yet.");
		ImGui::End();
		return;
	}

	const i32 display_frame_count = std::min(state.debug_ui.num_profiler_frames, available_frame_count);
	draw_cpu_profiler_flamegraph(display_frame_count);

	ImGui::End();
}

#else

static void draw_cpu_profiler_window() {}

#endif
