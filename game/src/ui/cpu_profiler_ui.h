#pragma once

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/timings.h"
#include "imgui/imgui.h"
#include "state/state.h"

#define stm_diff(in_new_ticks, in_old_ticks) timings_ticks_diff((in_new_ticks), (in_old_ticks))
#define stm_ms(in_ticks) timings_ticks_to_ms((in_ticks))

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
static constexpr f32 PROFILER_ROW_HEIGHT = 24.0f;
static constexpr f32 PROFILER_BOX_HEIGHT = 20.0f;
static constexpr f32 PROFILER_HEADER_HEIGHT = 22.0f;
static constexpr f32 PROFILER_FRAME_GAP = 0.0f;
static constexpr f32 PROFILER_MIN_FRAME_WIDTH = 220.0f;
static constexpr f32 PROFILER_MIN_BOX_WIDTH = 3.0f;

struct ProfilerTooltip
{
	bool active = false;
	char frame[64] = {};
	char context[CPU_TIMINGS_MAX_NAME_LENGTH] = {};
	char name[256] = {};
	char percent_label[64] = {};
	char details[1024] = {};
	f64 elapsed_ms = 0.0;
	f64 percent = 0.0;
	f64 start_ms = 0.0;
	f64 end_ms = 0.0;
	i32 depth = -1;
	i32 draw_order = -1;
	bool show_interval = false;
};

static bool profiler_rect_hovered(const ImVec2& rect_min, const ImVec2& rect_max)
{
	return ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
		ImGui::IsMouseHoveringRect(rect_min, rect_max, true);
}

static void profiler_consider_tooltip(
	ProfilerTooltip& tooltip,
	bool in_rect_hovered,
	const char* in_frame,
	const char* in_context,
	const char* in_name,
	f64 in_elapsed_ms,
	f64 in_percent,
	const char* in_percent_label,
	i32 in_depth,
	i32 in_draw_order,
	f64 in_start_ms = 0.0,
	f64 in_end_ms = 0.0,
	bool in_show_interval = false,
	const char* in_details = nullptr)
{
	if (!in_rect_hovered)
	{
		return;
	}

	const bool is_better_match =
		!tooltip.active ||
		in_depth > tooltip.depth ||
		(in_depth == tooltip.depth && in_draw_order > tooltip.draw_order);
	if (!is_better_match)
	{
		return;
	}

	tooltip.active = true;
	snprintf(tooltip.frame, sizeof(tooltip.frame), "%s", in_frame ? in_frame : "");
	snprintf(tooltip.context, sizeof(tooltip.context), "%s", in_context ? in_context : "");
	snprintf(tooltip.name, sizeof(tooltip.name), "%s", in_name ? in_name : "(unnamed)");
	snprintf(tooltip.percent_label, sizeof(tooltip.percent_label), "%s", in_percent_label ? in_percent_label : "% of frame");
	snprintf(tooltip.details, sizeof(tooltip.details), "%s", in_details ? in_details : "");
	tooltip.elapsed_ms = in_elapsed_ms;
	tooltip.percent = in_percent;
	tooltip.start_ms = in_start_ms;
	tooltip.end_ms = in_end_ms;
	tooltip.depth = in_depth;
	tooltip.draw_order = in_draw_order;
	tooltip.show_interval = in_show_interval;
}

static void profiler_draw_tooltip(const ProfilerTooltip& tooltip)
{
	if (!tooltip.active)
	{
		return;
	}

	ImGui::BeginTooltip();
	ImGui::Text("%s", tooltip.frame);
	if (tooltip.context[0])
	{
		ImGui::Text("%s", tooltip.context);
	}
	ImGui::Text("%s", tooltip.name);
	if (tooltip.details[0])
	{
		ImGui::Separator();
		ImGui::TextUnformatted(tooltip.details);
		ImGui::Separator();
	}
	if (tooltip.show_interval)
	{
		ImGui::Text("Start: %.3f ms", tooltip.start_ms);
		ImGui::Text("End: %.3f ms", tooltip.end_ms);
	}
	ImGui::Text("%.3f ms", tooltip.elapsed_ms);
	ImGui::Text("%.2f%s", tooltip.percent, tooltip.percent_label);
	ImGui::EndTooltip();
}

static void profiler_draw_clipped_label(
	ImDrawList* draw_list,
	const ImVec2& rect_min,
	const ImVec2& rect_max,
	ImU32 text_color,
	const char* label)
{
	if (!draw_list || !label || rect_max.x <= rect_min.x || rect_max.y <= rect_min.y)
	{
		return;
	}

	const ImVec2 text_size = ImGui::CalcTextSize(label);
	const ImVec2 text_pos = ImVec2(rect_min.x + 4.0f, rect_min.y + (PROFILER_BOX_HEIGHT - text_size.y) * 0.5f);
	ImGui::PushClipRect(rect_min, rect_max, true);
	draw_list->AddText(text_pos, text_color, label);
	ImGui::PopClipRect();
}

static f64 cpu_profiler_ms_from_root(const CpuTimingEvent& in_root_event, u64 in_ticks)
{
	if (in_ticks <= in_root_event.start_ticks)
	{
		return 0.0;
	}

	return stm_ms(stm_diff(in_ticks, in_root_event.start_ticks));
}

static f32 profiler_graph_width(i32 in_display_frame_count, f32 in_base_frame_width)
{
	const f32 frame_width = in_base_frame_width * state.debug_ui.profiler_zoom;
	return frame_width * (f32)in_display_frame_count + PROFILER_FRAME_GAP * (f32)(in_display_frame_count - 1);
}

static void profiler_apply_shared_zoom_and_scroll(i32 in_display_frame_count, f32 in_base_frame_width)
{
	const bool graph_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
	const bool graph_dragging = graph_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
	const f32 current_scroll_x = ImGui::GetScrollX();
	if (graph_dragging && std::abs(current_scroll_x - state.debug_ui.profiler_scroll_x) > 0.5f)
	{
		state.debug_ui.profiler_scroll_x = current_scroll_x;
	}
	else
	{
		ImGui::SetScrollX(state.debug_ui.profiler_scroll_x);
	}

	const f32 old_graph_width = profiler_graph_width(in_display_frame_count, in_base_frame_width);
	if (graph_hovered && ImGui::GetIO().KeyCtrl)
	{
		const f32 mouse_wheel = ImGui::GetIO().MouseWheel;
		if (mouse_wheel != 0.0f)
		{
			const f32 old_scroll_x = state.debug_ui.profiler_scroll_x;
			const f32 mouse_x_in_window = ImGui::GetIO().MousePos.x - ImGui::GetWindowPos().x;
			const f32 mouse_content_x = old_scroll_x + mouse_x_in_window;
			const f32 mouse_content_t = old_graph_width > 0.0f
				? CLAMP(mouse_content_x / old_graph_width, 0.0f, 1.0f)
				: 0.0f;

			const f32 zoom_multiplier = (f32)std::pow(1.15f, mouse_wheel);
			state.debug_ui.profiler_zoom = CLAMP(state.debug_ui.profiler_zoom * zoom_multiplier, 0.25f, 128.0f);

			const f32 new_graph_width = profiler_graph_width(in_display_frame_count, in_base_frame_width);
			state.debug_ui.profiler_scroll_x = std::max(0.0f, mouse_content_t * new_graph_width - mouse_x_in_window);
			ImGui::SetScrollX(state.debug_ui.profiler_scroll_x);
		}
	}
}

static void profiler_capture_shared_scroll()
{
	const bool graph_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
	const bool graph_dragging = graph_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
	if (graph_dragging || graph_hovered)
	{
		const f32 current_scroll_x = ImGui::GetScrollX();
		if (std::abs(current_scroll_x - state.debug_ui.profiler_scroll_x) > 0.5f)
		{
			state.debug_ui.profiler_scroll_x = current_scroll_x;
		}
	}
}

static void draw_cpu_profiler_flamegraph(i32 in_display_frame_count, f32 in_base_frame_width)
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

	const f32 graph_height = PROFILER_HEADER_HEIGHT + (max_depth + 1) * PROFILER_ROW_HEIGHT + 8.0f;
	const f32 child_height = std::max(180.0f, std::min(420.0f, graph_height + 12.0f));

	ImGui::BeginChild("##CpuProfilerGraph", ImVec2(0.0f, child_height), true, ImGuiWindowFlags_HorizontalScrollbar);
	profiler_apply_shared_zoom_and_scroll(in_display_frame_count, in_base_frame_width);

	const f32 frame_width = in_base_frame_width * state.debug_ui.profiler_zoom;
	const f32 graph_width = profiler_graph_width(in_display_frame_count, in_base_frame_width);
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
	ProfilerTooltip hovered_tooltip = {};
	i32 tooltip_draw_order = 0;
	for (i32 display_frame_index = 0; display_frame_index < in_display_frame_count; ++display_frame_index)
	{
		const i32 frame_source_index = in_display_frame_count - 1 - display_frame_index;
		const i64 frame_id = cpu_timings_get_display_frame_index(state.debug_ui.freeze_profiler, frame_source_index);
		const StretchyBuffer<CpuTimingEvent>& events = cpu_timings_get_display_frame(state.debug_ui.freeze_profiler, frame_source_index);
		if (events.length() == 0)
		{
			continue;
		}

		const CpuTimingEvent& root_event = events[0];
		const f64 frame_elapsed_ms = std::max(root_event.elapsed_ms, 0.0001);
		const f32 frame_x = graph_origin.x + (frame_width + PROFILER_FRAME_GAP) * (f32)display_frame_index;
		const f32 frame_y = graph_origin.y + PROFILER_HEADER_HEIGHT;

		char frame_label[64];
		snprintf(frame_label, sizeof(frame_label), "CPU Frame %lld: %.3f ms", (long long)frame_id, root_event.elapsed_ms);
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
			const f32 x1 = std::min(frame_x + frame_width, x0 + std::max(raw_w, PROFILER_MIN_BOX_WIDTH));
			const f32 y0 = frame_y + in_depth * PROFILER_ROW_HEIGHT + 2.0f;
			const f32 y1 = y0 + PROFILER_BOX_HEIGHT;
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
			profiler_draw_clipped_label(draw_list, rect_min, rect_max, in_text_color, label);

			const bool rect_hovered = profiler_rect_hovered(rect_min, rect_max);
			char tooltip_frame[64];
			snprintf(tooltip_frame, sizeof(tooltip_frame), "Frame %lld", (long long)frame_id);
			profiler_consider_tooltip(
				hovered_tooltip,
				rect_hovered,
				tooltip_frame,
				in_tooltip_context,
				in_name,
				in_elapsed_ms,
				(in_elapsed_ms / frame_elapsed_ms) * 100.0,
				"% of frame",
				in_depth,
				tooltip_draw_order++
			);

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
	profiler_draw_tooltip(hovered_tooltip);

	ImGui::SetCursorScreenPos(graph_origin);
	ImGui::Dummy(ImVec2(graph_width, graph_height));
	profiler_capture_shared_scroll();
	ImGui::EndChild();
}

static bool gpu_profiler_is_root_event(const GpuTimingEvent& event)
{
	return event.parent_index < 0 && event.depth == 0;
}

static bool gpu_profiler_is_cpu_marker(const GpuTimingEvent& event)
{
	return event.timestamp_source == GpuTimingTimestampSource::CpuOnlyMarker;
}

static bool gpu_profiler_event_is_drawable(const GpuTimingEvent& event)
{
	return event.valid && !gpu_profiler_is_root_event(event) && (event.elapsed_ms > 0.0 || gpu_profiler_is_cpu_marker(event));
}

static ImU32 gpu_profiler_event_color(const GpuTimingEvent& event)
{
	switch (event.timestamp_confidence)
	{
		case GpuTimingTimestampConfidence::Authoritative:
			return IM_COL32(0x3B, 0x92, 0xC8, 255);
		case GpuTimingTimestampConfidence::Sampled:
			return IM_COL32(0x4B, 0xA7, 0xA0, 255);
		case GpuTimingTimestampConfidence::Approximate:
			return IM_COL32(0xB8, 0x9B, 0x48, 255);
		case GpuTimingTimestampConfidence::AdjustedForOrder:
			return IM_COL32(0x8C, 0x7A, 0xB8, 255);
		case GpuTimingTimestampConfidence::Unavailable:
		default:
			return IM_COL32(0x76, 0x7D, 0x86, 255);
	}
}

struct GpuProfilerTimelineRange
{
	f64 display_start_ms = 0.0;
	f64 display_end_ms = 0.0;
	i32 row_index = -1;
	bool adjusted_for_order = false;
};

static bool gpu_profiler_same_ordered_stream(const GpuTimingEvent& a, const GpuTimingEvent& b)
{
	return a.command_buffer_id >= 0 &&
		b.command_buffer_id >= 0 &&
		a.command_buffer_id == b.command_buffer_id &&
		a.lane_index == b.lane_index;
}

static f64 gpu_profiler_ms_between_ticks(u64 in_start_ticks, u64 in_end_ticks)
{
	if (in_start_ticks == 0 || in_end_ticks == 0 || in_end_ticks < in_start_ticks)
	{
		return -1.0;
	}
	return stm_ms(stm_diff(in_end_ticks, in_start_ticks));
}

static void gpu_profiler_format_optional_duration(
	char*& out_cursor,
	size_t& inout_remaining,
	const char* in_label,
	f64 in_duration_ms)
{
	if (!out_cursor || inout_remaining == 0 || in_duration_ms < 0.0)
	{
		return;
	}

	const int written = snprintf(out_cursor, inout_remaining, "%s: %.3f ms\n", in_label, in_duration_ms);
	if (written <= 0)
	{
		return;
	}
	const size_t clamped_written = std::min((size_t)written, inout_remaining - 1);
	out_cursor += clamped_written;
	inout_remaining -= clamped_written;
}

static void gpu_profiler_build_event_details(const GpuTimingEvent& event, char* out_details, size_t in_details_size)
{
	if (!out_details || in_details_size == 0)
	{
		return;
	}

	char* cursor = out_details;
	size_t remaining = in_details_size;
	const bool is_cpu_marker = gpu_profiler_is_cpu_marker(event);
	int header_written = 0;
	if (is_cpu_marker)
	{
		header_written = snprintf(
			cursor,
			remaining,
			"Type: %s\nSource: %s\nConfidence: %s\nGPU interval: unavailable\nDisplayed duration: CPU encode marker\nSubmission: %lld\nCommand buffer: %lld\nOrder: %d\n",
			gpu_timing_event_type_name(event.type),
			gpu_timing_timestamp_source_name(event.timestamp_source),
			gpu_timing_timestamp_confidence_name(event.timestamp_confidence),
			(long long)event.submission_id,
			(long long)event.command_buffer_id,
			event.command_order_index
		);
	}
	else
	{
		header_written = snprintf(
			cursor,
			remaining,
			"Type: %s\nSource: %s\nConfidence: %s\nRaw interval: %.3f - %.3f ms\nSubmission: %lld\nCommand buffer: %lld\nOrder: %d\n",
			gpu_timing_event_type_name(event.type),
			gpu_timing_timestamp_source_name(event.timestamp_source),
			gpu_timing_timestamp_confidence_name(event.timestamp_confidence),
			event.start_offset_ms,
			event.start_offset_ms + event.elapsed_ms,
			(long long)event.submission_id,
			(long long)event.command_buffer_id,
			event.command_order_index
		);
	}
	if (header_written > 0)
	{
		const size_t clamped_written = std::min((size_t)header_written, remaining - 1);
		cursor += clamped_written;
		remaining -= clamped_written;
	}

	gpu_profiler_format_optional_duration(
		cursor,
		remaining,
		"CPU encode",
		gpu_profiler_ms_between_ticks(event.cpu_encode_start_ticks, event.cpu_encode_end_ticks)
	);
	gpu_profiler_format_optional_duration(
		cursor,
		remaining,
		"CPU submit -> scheduled",
		gpu_profiler_ms_between_ticks(event.cpu_submit_ticks, event.cpu_scheduled_ticks)
	);
	gpu_profiler_format_optional_duration(
		cursor,
		remaining,
		"CPU scheduled -> completed",
		gpu_profiler_ms_between_ticks(event.cpu_scheduled_ticks, event.cpu_completed_ticks)
	);
	gpu_profiler_format_optional_duration(
		cursor,
		remaining,
		"CPU submit -> completed",
		gpu_profiler_ms_between_ticks(event.cpu_submit_ticks, event.cpu_completed_ticks)
	);

	if (event.reads[0] && remaining > 1)
	{
		const int written = snprintf(cursor, remaining, "Reads from: %s\n", event.reads);
		if (written > 0)
		{
			const size_t clamped_written = std::min((size_t)written, remaining - 1);
			cursor += clamped_written;
			remaining -= clamped_written;
		}
	}
	if (event.writes[0] && remaining > 1)
	{
		const int written = snprintf(cursor, remaining, "Writes to: %s\n", event.writes);
		if (written > 0)
		{
			const size_t clamped_written = std::min((size_t)written, remaining - 1);
			cursor += clamped_written;
			remaining -= clamped_written;
		}
	}
}

static i32 gpu_profiler_build_timeline_ranges(const GpuTimingFrame& gpu_frame, StretchyBuffer<GpuProfilerTimelineRange>& out_ranges)
{
	out_ranges.reset();
	for (i32 event_index = 0; event_index < (i32)gpu_frame.events.length(); ++event_index)
	{
		const GpuTimingEvent& event = gpu_frame.events[event_index];
		GpuProfilerTimelineRange range = {};
		range.display_start_ms = event.start_offset_ms;
		range.display_end_ms = event.start_offset_ms + std::max(event.elapsed_ms, gpu_profiler_is_cpu_marker(event) ? 0.001 : 0.0);
		out_ranges.add(range);
	}

	StretchyBuffer<i32> event_indices;
	for (i32 event_index = 0; event_index < (i32)gpu_frame.events.length(); ++event_index)
	{
		const GpuTimingEvent& event = gpu_frame.events[event_index];
		if (gpu_profiler_event_is_drawable(event))
		{
			event_indices.add(event_index);
		}
	}

	std::sort(
		event_indices.begin(),
		event_indices.end(),
		[&](const i32 a, const i32 b)
		{
			const GpuTimingEvent& event_a = gpu_frame.events[a];
			const GpuTimingEvent& event_b = gpu_frame.events[b];
			if (event_a.command_buffer_id != event_b.command_buffer_id)
			{
				return event_a.command_buffer_id < event_b.command_buffer_id;
			}
			if (event_a.lane_index != event_b.lane_index)
			{
				return event_a.lane_index < event_b.lane_index;
			}
			if (event_a.command_order_index != event_b.command_order_index)
			{
				return event_a.command_order_index < event_b.command_order_index;
			}
			return event_a.start_offset_ms < event_b.start_offset_ms;
		}
	);

	i32 previous_event_index = -1;
	f64 ordered_stream_end_ms = 0.0;
	for (const i32 event_index : event_indices)
	{
		const GpuTimingEvent& event = gpu_frame.events[event_index];
		GpuProfilerTimelineRange& range = out_ranges[event_index];
		if (
			previous_event_index >= 0 &&
			gpu_profiler_same_ordered_stream(gpu_frame.events[previous_event_index], event)
		)
		{
			if (range.display_start_ms < ordered_stream_end_ms)
			{
				range.display_start_ms = ordered_stream_end_ms;
				range.display_end_ms = range.display_start_ms + event.elapsed_ms;
				range.adjusted_for_order = true;
			}
		}
		else
		{
			ordered_stream_end_ms = range.display_end_ms;
		}

		ordered_stream_end_ms = std::max(ordered_stream_end_ms, range.display_end_ms);
		previous_event_index = event_index;
	}

	std::sort(
		event_indices.begin(),
		event_indices.end(),
		[&](const i32 a, const i32 b)
		{
			const GpuProfilerTimelineRange& range_a = out_ranges[a];
			const GpuProfilerTimelineRange& range_b = out_ranges[b];
			if (range_a.display_start_ms == range_b.display_start_ms)
			{
				return (range_a.display_end_ms - range_a.display_start_ms) > (range_b.display_end_ms - range_b.display_start_ms);
			}
			return range_a.display_start_ms < range_b.display_start_ms;
		}
	);

	StretchyBuffer<f64> row_end_times;
	for (const i32 event_index : event_indices)
	{
		GpuProfilerTimelineRange& range = out_ranges[event_index];
		const f64 event_end_ms = range.display_end_ms;
		i32 row_index = -1;
		for (i32 candidate_row = 0; candidate_row < (i32)row_end_times.length(); ++candidate_row)
		{
			if (range.display_start_ms >= row_end_times[candidate_row])
			{
				row_index = candidate_row;
				break;
			}
		}

		if (row_index < 0)
		{
			row_index = (i32)row_end_times.length();
			row_end_times.add(event_end_ms);
		}
		else
		{
			row_end_times[row_index] = event_end_ms;
		}

		range.row_index = row_index;
	}

	return std::max((i32)row_end_times.length(), 1);
}

static void draw_gpu_profiler_timeline(i32 in_display_frame_count, f32 in_base_frame_width)
{
	const bool gpu_timings_available = gpu_timings_are_available();
	char gpu_timings_unavailable_reason[CPU_TIMINGS_MAX_NAME_LENGTH] = {};
	char gpu_timings_status_message[GPU_TIMINGS_MAX_DEPENDENCY_TEXT_LENGTH] = {};
	if (!gpu_timings_available)
	{
		gpu_timings_copy_unavailable_reason(gpu_timings_unavailable_reason, sizeof(gpu_timings_unavailable_reason));
	}
	gpu_timings_copy_status_message(gpu_timings_status_message, sizeof(gpu_timings_status_message));

	i32 max_row_count = 1;
	for (i32 display_frame_index = 0; display_frame_index < in_display_frame_count; ++display_frame_index)
	{
		const i32 frame_source_index = in_display_frame_count - 1 - display_frame_index;
		GpuTimingFrame gpu_frame = {};
		if (!gpu_timings_copy_display_frame(state.debug_ui.freeze_profiler, frame_source_index, gpu_frame))
		{
			continue;
		}

		StretchyBuffer<GpuProfilerTimelineRange> event_ranges;
		max_row_count = std::max(max_row_count, gpu_profiler_build_timeline_ranges(gpu_frame, event_ranges));
	}

	const f32 graph_height = PROFILER_HEADER_HEIGHT + max_row_count * PROFILER_ROW_HEIGHT + 8.0f;
	const f32 child_height = std::max(96.0f, std::min(220.0f, graph_height + 12.0f));

	ImGui::Text("GPU Timeline");
	if (gpu_timings_status_message[0])
	{
		ImGui::SameLine();
		ImGui::TextDisabled("%s", gpu_timings_status_message);
	}
	ImGui::BeginChild("##GpuProfilerTimeline", ImVec2(0.0f, child_height), true, ImGuiWindowFlags_HorizontalScrollbar);
	profiler_apply_shared_zoom_and_scroll(in_display_frame_count, in_base_frame_width);

	const f32 frame_width = in_base_frame_width * state.debug_ui.profiler_zoom;
	const f32 graph_width = profiler_graph_width(in_display_frame_count, in_base_frame_width);
	const ImVec2 graph_origin = ImGui::GetCursorScreenPos();
	const ImVec2 graph_max = ImVec2(graph_origin.x + graph_width, graph_origin.y + graph_height);
	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	draw_list->AddRectFilled(
		graph_origin,
		graph_max,
		IM_COL32(22, 28, 28, 255)
	);
	draw_list->AddRect(
		graph_origin,
		graph_max,
		IM_COL32(70, 82, 82, 255)
	);

	ImGui::PushClipRect(graph_origin, graph_max, true);
	ProfilerTooltip hovered_tooltip = {};
	i32 tooltip_draw_order = 0;
	for (i32 display_frame_index = 0; display_frame_index < in_display_frame_count; ++display_frame_index)
	{
		const i32 frame_source_index = in_display_frame_count - 1 - display_frame_index;
		const i64 cpu_frame_index = cpu_timings_get_display_frame_index(state.debug_ui.freeze_profiler, frame_source_index);

		GpuTimingFrame gpu_frame = {};
		const bool has_gpu_frame = gpu_timings_copy_display_frame(state.debug_ui.freeze_profiler, frame_source_index, gpu_frame);
		StretchyBuffer<GpuProfilerTimelineRange> event_ranges;
		gpu_profiler_build_timeline_ranges(gpu_frame, event_ranges);

		f64 gpu_frame_elapsed_ms = 0.0001;
		const GpuTimingEvent* root_event = nullptr;
		for (const GpuTimingEvent& event : gpu_frame.events)
		{
			if (event.valid)
			{
				if (!root_event && gpu_profiler_is_root_event(event))
				{
					root_event = &event;
				}
				gpu_frame_elapsed_ms = std::max(gpu_frame_elapsed_ms, event.start_offset_ms + event.elapsed_ms);
			}
		}
		for (const GpuProfilerTimelineRange& range : event_ranges)
		{
			gpu_frame_elapsed_ms = std::max(gpu_frame_elapsed_ms, range.display_end_ms);
		}
		if (root_event)
		{
			gpu_frame_elapsed_ms = std::max(gpu_frame_elapsed_ms, root_event->elapsed_ms);
		}
		const f64 frame_elapsed_ms = std::max(gpu_frame_elapsed_ms, 0.0001);
		const f32 frame_x = graph_origin.x + (frame_width + PROFILER_FRAME_GAP) * (f32)display_frame_index;
		const f32 frame_y = graph_origin.y + PROFILER_HEADER_HEIGHT;

		char frame_label[96];
		if (has_gpu_frame && root_event)
		{
			snprintf(frame_label, sizeof(frame_label), "GPU Frame %lld: %.3f ms", (long long)cpu_frame_index, root_event->elapsed_ms);
		}
		else if (has_gpu_frame && gpu_frame.events.length() > 0)
		{
			snprintf(frame_label, sizeof(frame_label), "GPU Frame %lld: %.3f ms", (long long)cpu_frame_index, frame_elapsed_ms);
		}
		else if (!gpu_timings_available)
		{
			snprintf(frame_label, sizeof(frame_label), "GPU Frame %lld: unavailable", (long long)cpu_frame_index);
		}
		else
		{
			snprintf(frame_label, sizeof(frame_label), "GPU Frame %lld: pending", (long long)cpu_frame_index);
		}
		draw_list->AddText(ImVec2(frame_x + 4.0f, graph_origin.y + 3.0f), IM_COL32(230, 238, 238, 255), frame_label);

		if (display_frame_index > 0)
		{
			draw_list->AddLine(
				ImVec2(frame_x, graph_origin.y),
				ImVec2(frame_x, graph_origin.y + graph_height),
				IM_COL32(70, 82, 82, 255)
			);
		}

		draw_list->AddRectFilled(
			ImVec2(frame_x, frame_y),
			ImVec2(frame_x + frame_width, frame_y + max_row_count * PROFILER_ROW_HEIGHT),
			IM_COL32(30, 42, 42, 95)
		);
		for (i32 row_index = 0; row_index < max_row_count; ++row_index)
		{
			const f32 lane_y = frame_y + row_index * PROFILER_ROW_HEIGHT;
			draw_list->AddLine(
				ImVec2(frame_x, lane_y),
				ImVec2(frame_x + frame_width, lane_y),
				IM_COL32(50, 62, 62, 180)
			);
		}

		if (!has_gpu_frame || gpu_frame.events.length() == 0)
		{
			const char* message = gpu_timings_available
				? "GPU timing pending"
				: (gpu_timings_unavailable_reason[0] ? gpu_timings_unavailable_reason : "GPU timing unavailable");
			draw_list->AddText(
				ImVec2(frame_x + 4.0f, frame_y + 4.0f),
				IM_COL32(150, 168, 168, 255),
				message
			);
			continue;
		}

		for (i32 event_index = 0; event_index < (i32)gpu_frame.events.length(); ++event_index)
		{
			const GpuTimingEvent& event = gpu_frame.events[event_index];
			if (!gpu_profiler_event_is_drawable(event))
			{
				continue;
			}

			const GpuProfilerTimelineRange& range = event_ranges[event_index];
			const i32 row_index = range.row_index >= 0
				? range.row_index
				: 0;
			const f64 display_elapsed_ms = std::max(range.display_end_ms - range.display_start_ms, 0.0);
			const f32 raw_x = (f32)(range.display_start_ms / frame_elapsed_ms) * frame_width;
			const f32 raw_w = (f32)(display_elapsed_ms / frame_elapsed_ms) * frame_width;
			const f32 x0 = frame_x + CLAMP(raw_x, 0.0f, frame_width);
			const f32 x1 = std::min(frame_x + frame_width, x0 + std::max(raw_w, PROFILER_MIN_BOX_WIDTH));
			const f32 y0 = frame_y + row_index * PROFILER_ROW_HEIGHT + 2.0f;
			const f32 y1 = y0 + PROFILER_BOX_HEIGHT;
			if (x1 <= x0 || y1 <= y0)
			{
				continue;
			}

			const ImVec2 rect_min = ImVec2(x0, y0);
			const ImVec2 rect_max = ImVec2(x1, y1);
			draw_list->AddRectFilled(rect_min, rect_max, gpu_profiler_event_color(event), 2.0f);
			draw_list->AddRect(rect_min, rect_max, IM_COL32(12, 18, 18, 190), 2.0f);

			char label[256];
			snprintf(label, sizeof(label), "%s %.3f ms", event.name, event.elapsed_ms);
			profiler_draw_clipped_label(draw_list, rect_min, rect_max, IM_COL32(255, 255, 255, 255), label);

			const bool rect_hovered = profiler_rect_hovered(rect_min, rect_max);
			char tooltip_frame[64];
			snprintf(tooltip_frame, sizeof(tooltip_frame), "Frame %lld", (long long)cpu_frame_index);
			char tooltip_details[1024];
			gpu_profiler_build_event_details(event, tooltip_details, sizeof(tooltip_details));
			if (range.adjusted_for_order)
			{
				const size_t detail_length = strlen(tooltip_details);
				if (detail_length < sizeof(tooltip_details) - 1)
				{
					snprintf(
						tooltip_details + detail_length,
						sizeof(tooltip_details) - detail_length,
						"Display interval: %.3f - %.3f ms\nDraw adjusted to preserve command-buffer order\n",
						range.display_start_ms,
						range.display_end_ms
					);
				}
			}
			profiler_consider_tooltip(
				hovered_tooltip,
				rect_hovered,
				tooltip_frame,
				nullptr,
				event.name,
				event.elapsed_ms,
				(event.elapsed_ms / frame_elapsed_ms) * 100.0,
				"% of GPU frame",
				row_index,
				tooltip_draw_order++,
				event.start_offset_ms,
				event.start_offset_ms + event.elapsed_ms,
				true,
				tooltip_details
			);
		}
	}
	ImGui::PopClipRect();
	profiler_draw_tooltip(hovered_tooltip);

	ImGui::SetCursorScreenPos(graph_origin);
	ImGui::Dummy(ImVec2(graph_width, graph_height));
	profiler_capture_shared_scroll();
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
		state.debug_ui.profiler_scroll_x = 0.0f;
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
	const f32 visible_width = std::max(1.0f, ImGui::GetContentRegionAvail().x - 8.0f);
	const f32 base_frame_width = std::max(
		PROFILER_MIN_FRAME_WIDTH,
		(visible_width - PROFILER_FRAME_GAP * (f32)(display_frame_count - 1)) / (f32)display_frame_count
	);
	draw_cpu_profiler_flamegraph(display_frame_count, base_frame_width);
	draw_gpu_profiler_timeline(display_frame_count, base_frame_width);

	ImGui::End();
}

#undef stm_ms
#undef stm_diff

#else

static void draw_cpu_profiler_window() {}

#endif
