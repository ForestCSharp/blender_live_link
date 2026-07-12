#pragma once

#include <cstddef>

#include "core/types.h"

static constexpr i32 CPU_TIMINGS_MAX_NAME_LENGTH = 128;
static constexpr i32 GPU_TIMINGS_MAX_DEPENDENCY_TEXT_LENGTH = 256;

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>

#include "core/stretchy_buffer.h"

// game_old/ uses sokol_time (stm_*); game keeps the same tick-based interface on
// std::chrono. Ticks are nanoseconds.
inline u64 timings_now_ticks()
{
	return (u64) std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()
	).count();
}

inline f64 timings_ticks_to_ms(u64 in_ticks)
{
	return (f64) in_ticks / 1000000.0;
}

inline u64 timings_ticks_diff(u64 in_new_ticks, u64 in_old_ticks)
{
	return (in_new_ticks > in_old_ticks) ? (in_new_ticks - in_old_ticks) : 0;
}

static constexpr i32 CPU_TIMINGS_MAX_DISPLAY_FRAMES = 5;
static constexpr i32 CPU_TIMINGS_MAX_HISTORY_FRAMES = CPU_TIMINGS_MAX_DISPLAY_FRAMES + 1;
static constexpr i32 GPU_TIMINGS_MAX_HISTORY_FRAMES = 32;

struct CpuTimingEvent
{
	char name[CPU_TIMINGS_MAX_NAME_LENGTH] = "(unnamed)";
	i32 depth = 0;
	i32 parent_index = -1;
	u64 start_ticks = 0;
	u64 end_ticks = 0;
	f64 elapsed_ms = 0.0;
	bool is_open = false;
};

void cpu_timing_event_set_name(CpuTimingEvent& event, const char* in_name)
{
	snprintf(event.name, sizeof(event.name), "%s", in_name ? in_name : "(unnamed)");
}

struct CpuTimings
{
	StretchyBuffer<CpuTimingEvent> current_frame;
	StretchyBuffer<CpuTimingEvent> previous_frame;
	StretchyBuffer<CpuTimingEvent> display_frame;
	StretchyBuffer<CpuTimingEvent> previous_frames[CPU_TIMINGS_MAX_HISTORY_FRAMES];
	StretchyBuffer<CpuTimingEvent> display_frames[CPU_TIMINGS_MAX_DISPLAY_FRAMES];
	i64 previous_frame_index = -1;
	i64 display_frame_index = -1;
	i64 display_latest_frame_index = -1;
	i64 previous_frame_indices[CPU_TIMINGS_MAX_HISTORY_FRAMES] = {};
	i64 display_frame_indices[CPU_TIMINGS_MAX_DISPLAY_FRAMES] = {};
	StretchyBuffer<i32> active_scope_stack;
	i64 current_frame_index = -1;
	i64 next_frame_index = 0;
	u64 previous_frame_generation = 0;
	u64 display_frame_generation = 0;
	i32 previous_frame_count = 0;
	i32 display_frame_count = 0;
	bool frame_active = false;
	bool display_is_frozen = false;
};

CpuTimings& cpu_timings_get()
{
	static CpuTimings timings;
	return timings;
}

void cpu_timings_begin_frame()
{
	CpuTimings& timings = cpu_timings_get();
	timings.current_frame.reset();
	timings.active_scope_stack.reset();
	timings.current_frame_index = timings.next_frame_index++;
	timings.frame_active = true;
}

i32 cpu_timings_begin_scope(const char* in_name)
{
	CpuTimings& timings = cpu_timings_get();
	if (!timings.frame_active)
	{
		return -1;
	}

	const i32 event_index = (i32)timings.current_frame.length();
	const i32 parent_index = timings.active_scope_stack.length() > 0
		? timings.active_scope_stack.last()
		: -1;

	CpuTimingEvent event = {};
	cpu_timing_event_set_name(event, in_name);
	event.depth = (i32)timings.active_scope_stack.length();
	event.parent_index = parent_index;
	event.start_ticks = timings_now_ticks();
	event.is_open = true;

	timings.current_frame.add(event);
	timings.active_scope_stack.add(event_index);
	return event_index;
}

void cpu_timings_close_scope(CpuTimingEvent& event, u64 in_end_ticks)
{
	if (!event.is_open)
	{
		return;
	}

	event.end_ticks = in_end_ticks;
	event.elapsed_ms = timings_ticks_to_ms(timings_ticks_diff(event.end_ticks, event.start_ticks));
	event.is_open = false;
}

void cpu_timings_record_event(const char* in_name, u64 in_start_ticks, u64 in_end_ticks)
{
	CpuTimings& timings = cpu_timings_get();
	if (!timings.frame_active || in_start_ticks == 0 || in_end_ticks < in_start_ticks)
	{
		return;
	}

	const i32 parent_index = timings.active_scope_stack.length() > 0
		? timings.active_scope_stack.last()
		: -1;

	CpuTimingEvent event = {};
	cpu_timing_event_set_name(event, in_name);
	event.depth = (i32)timings.active_scope_stack.length();
	event.parent_index = parent_index;
	event.start_ticks = in_start_ticks;
	event.end_ticks = in_end_ticks;
	event.elapsed_ms = timings_ticks_to_ms(timings_ticks_diff(event.end_ticks, event.start_ticks));
	event.is_open = false;

	timings.current_frame.add(event);
}

void cpu_timings_end_scope(i32 in_event_index)
{
	if (in_event_index < 0)
	{
		return;
	}

	CpuTimings& timings = cpu_timings_get();
	if (!timings.frame_active || !timings.current_frame.is_valid_index(in_event_index))
	{
		return;
	}

	const u64 end_ticks = timings_now_ticks();

	while (timings.active_scope_stack.length() > 0)
	{
		const i32 active_event_index = timings.active_scope_stack.last();
		if (timings.current_frame.is_valid_index(active_event_index))
		{
			cpu_timings_close_scope(timings.current_frame[active_event_index], end_ticks);
		}
		timings.active_scope_stack.pop();
		if (active_event_index == in_event_index)
		{
			return;
		}
	}

	cpu_timings_close_scope(timings.current_frame[in_event_index], end_ticks);
}

void cpu_timings_end_frame()
{
	CpuTimings& timings = cpu_timings_get();
	if (!timings.frame_active)
	{
		return;
	}

	while (timings.active_scope_stack.length() > 0)
	{
		cpu_timings_end_scope(timings.active_scope_stack.last());
	}

	timings.previous_frame = timings.current_frame;
	timings.previous_frame_index = timings.current_frame_index;
	for (i32 frame_index = CPU_TIMINGS_MAX_HISTORY_FRAMES - 1; frame_index > 0; --frame_index)
	{
		timings.previous_frames[frame_index] = timings.previous_frames[frame_index - 1];
		timings.previous_frame_indices[frame_index] = timings.previous_frame_indices[frame_index - 1];
	}
	timings.previous_frames[0] = timings.current_frame;
	timings.previous_frame_indices[0] = timings.current_frame_index;
	timings.previous_frame_count = std::min(timings.previous_frame_count + 1, CPU_TIMINGS_MAX_HISTORY_FRAMES);
	++timings.previous_frame_generation;

	timings.active_scope_stack.reset();
	timings.frame_active = false;
}

const StretchyBuffer<CpuTimingEvent>& cpu_timings_get_previous_frame()
{
	return cpu_timings_get().previous_frame;
}

i64 cpu_timings_get_current_frame_index()
{
	return cpu_timings_get().current_frame_index;
}

void cpu_timings_copy_previous_frames_to_display(CpuTimings& timings)
{
	constexpr i32 display_source_offset = 1;
	timings.display_latest_frame_index = timings.previous_frame_index;
	const i32 available_display_frame_count = std::max(0, timings.previous_frame_count - display_source_offset);
	timings.display_frame_count = std::min(available_display_frame_count, CPU_TIMINGS_MAX_DISPLAY_FRAMES);
	if (timings.display_frame_count > 0)
	{
		timings.display_frame = timings.previous_frames[display_source_offset];
		timings.display_frame_index = timings.previous_frame_indices[display_source_offset];
	}
	else
	{
		timings.display_frame = timings.previous_frame;
		timings.display_frame_index = timings.previous_frame_index;
	}

	for (i32 frame_index = 0; frame_index < timings.display_frame_count; ++frame_index)
	{
		const i32 source_frame_index = frame_index + display_source_offset;
		timings.display_frames[frame_index] = timings.previous_frames[source_frame_index];
		timings.display_frame_indices[frame_index] = timings.previous_frame_indices[source_frame_index];
	}
	timings.display_frame_generation = timings.previous_frame_generation;
}

void cpu_timings_update_display_frames(bool in_freeze)
{
	CpuTimings& timings = cpu_timings_get();
	if (in_freeze)
	{
		if (!timings.display_is_frozen)
		{
			cpu_timings_copy_previous_frames_to_display(timings);
			timings.display_is_frozen = true;
		}
		return;
	}

	timings.display_is_frozen = false;
	if (timings.display_frame_generation != timings.previous_frame_generation)
	{
		cpu_timings_copy_previous_frames_to_display(timings);
	}
}

i32 cpu_timings_get_display_frame_count(bool in_freeze)
{
	cpu_timings_update_display_frames(in_freeze);
	return cpu_timings_get().display_frame_count;
}

const StretchyBuffer<CpuTimingEvent>& cpu_timings_get_display_frame(bool in_freeze, i32 in_frame_index = 0)
{
	cpu_timings_update_display_frames(in_freeze);
	CpuTimings& timings = cpu_timings_get();
	if (timings.display_frame_count <= 0)
	{
		return timings.display_frame;
	}

	const i32 frame_index = CLAMP(in_frame_index, 0, timings.display_frame_count - 1);
	return timings.display_frames[frame_index];
}

const StretchyBuffer<CpuTimingEvent>& cpu_timings_get_display_frame(bool in_freeze)
{
	cpu_timings_update_display_frames(in_freeze);
	CpuTimings& timings = cpu_timings_get();
	if (timings.display_frame_count > 0)
	{
		return timings.display_frames[0];
	}

	return timings.display_frame;
}

u64 cpu_timings_get_display_generation(bool in_freeze)
{
	cpu_timings_update_display_frames(in_freeze);
	return cpu_timings_get().display_frame_generation;
}

i64 cpu_timings_get_display_frame_index(bool in_freeze, i32 in_frame_index = 0)
{
	cpu_timings_update_display_frames(in_freeze);
	CpuTimings& timings = cpu_timings_get();
	if (timings.display_frame_count <= 0)
	{
		return timings.display_frame_index;
	}

	const i32 frame_index = CLAMP(in_frame_index, 0, timings.display_frame_count - 1);
	return timings.display_frame_indices[frame_index];
}

i32 cpu_timings_get_display_frame_age(bool in_freeze, i32 in_frame_index = 0)
{
	const i64 display_frame_index = cpu_timings_get_display_frame_index(in_freeze, in_frame_index);
	CpuTimings& timings = cpu_timings_get();
	if (display_frame_index < 0 || timings.display_latest_frame_index < display_frame_index)
	{
		return 0;
	}

	return (i32)(timings.display_latest_frame_index - display_frame_index + 1);
}

bool cpu_timings_get_display_frame_total_ms(bool in_freeze, f64& out_ms)
{
	const StretchyBuffer<CpuTimingEvent>& events = cpu_timings_get_display_frame(in_freeze, 0);
	for (const CpuTimingEvent& event : events)
	{
		if (event.parent_index == -1 && event.elapsed_ms > 0.0)
		{
			out_ms = event.elapsed_ms;
			return true;
		}
	}

	return false;
}

bool cpu_timings_get_latest_frame_total_ms(f64& out_ms)
{
	const StretchyBuffer<CpuTimingEvent>& events = cpu_timings_get_previous_frame();
	for (const CpuTimingEvent& event : events)
	{
		if (event.parent_index == -1 && event.elapsed_ms > 0.0)
		{
			out_ms = event.elapsed_ms;
			return true;
		}
	}

	return false;
}

enum class GpuTimingEventType : i32
{
	Unknown,
	Frame,
	RenderPass,
	ComputePass,
	Scope,
};

const char* gpu_timing_event_type_name(GpuTimingEventType in_type)
{
	switch (in_type)
	{
		case GpuTimingEventType::Frame: return "Frame";
		case GpuTimingEventType::RenderPass: return "Render pass";
		case GpuTimingEventType::ComputePass: return "Compute pass";
		case GpuTimingEventType::Scope: return "Scope";
		case GpuTimingEventType::Unknown:
		default: return "Unknown";
	}
}

enum class GpuTimingTimestampSource : i32
{
	Unknown,
	CommandBuffer,
	D3D11Timestamp,
	MetalEncoderSample,
	MetalStageSample,
	CpuOnlyMarker,
};

const char* gpu_timing_timestamp_source_name(GpuTimingTimestampSource in_source)
{
	switch (in_source)
	{
		case GpuTimingTimestampSource::CommandBuffer: return "Command buffer";
		case GpuTimingTimestampSource::D3D11Timestamp: return "D3D11 timestamp";
		case GpuTimingTimestampSource::MetalEncoderSample: return "Metal encoder sample";
		case GpuTimingTimestampSource::MetalStageSample: return "Metal stage sample";
		case GpuTimingTimestampSource::CpuOnlyMarker: return "CPU marker";
		case GpuTimingTimestampSource::Unknown:
		default: return "Unknown";
	}
}

enum class GpuTimingTimestampConfidence : i32
{
	Unavailable,
	Approximate,
	Sampled,
	Authoritative,
	AdjustedForOrder,
};

const char* gpu_timing_timestamp_confidence_name(GpuTimingTimestampConfidence in_confidence)
{
	switch (in_confidence)
	{
		case GpuTimingTimestampConfidence::Approximate: return "Approximate";
		case GpuTimingTimestampConfidence::Sampled: return "Sampled";
		case GpuTimingTimestampConfidence::Authoritative: return "Authoritative";
		case GpuTimingTimestampConfidence::AdjustedForOrder: return "Adjusted for order";
		case GpuTimingTimestampConfidence::Unavailable:
		default: return "Unavailable";
	}
}

struct GpuTimingEvent
{
	char name[CPU_TIMINGS_MAX_NAME_LENGTH] = "(unnamed)";
	i32 depth = 0;
	i32 parent_index = -1;
	i32 lane_index = 0;
	GpuTimingEventType type = GpuTimingEventType::Unknown;
	GpuTimingTimestampSource timestamp_source = GpuTimingTimestampSource::Unknown;
	GpuTimingTimestampConfidence timestamp_confidence = GpuTimingTimestampConfidence::Unavailable;
	i64 submission_id = -1;
	i64 command_buffer_id = -1;
	i32 command_order_index = -1;
	f64 start_offset_ms = 0.0;
	f64 elapsed_ms = 0.0;
	u64 cpu_encode_start_ticks = 0;
	u64 cpu_encode_end_ticks = 0;
	u64 cpu_submit_ticks = 0;
	u64 cpu_scheduled_ticks = 0;
	u64 cpu_completed_ticks = 0;
	char reads[GPU_TIMINGS_MAX_DEPENDENCY_TEXT_LENGTH] = {};
	char writes[GPU_TIMINGS_MAX_DEPENDENCY_TEXT_LENGTH] = {};
	bool valid = false;
};

struct GpuTimingFrame
{
	i64 frame_index = -1;
	StretchyBuffer<GpuTimingEvent> events;
	bool valid = false;
};

void gpu_timing_event_set_name(GpuTimingEvent& event, const char* in_name)
{
	snprintf(event.name, sizeof(event.name), "%s", in_name ? in_name : "(unnamed)");
}

struct GpuTimings
{
	GpuTimingFrame history_frames[GPU_TIMINGS_MAX_HISTORY_FRAMES];
	GpuTimingFrame display_frames[CPU_TIMINGS_MAX_DISPLAY_FRAMES];
	std::mutex mutex;
	u64 completed_generation = 0;
	u64 display_cpu_generation = 0;
	u64 display_completed_generation = 0;
	i32 next_history_frame_index = 0;
	i32 display_frame_count = 0;
	bool available = false;
	char unavailable_reason[CPU_TIMINGS_MAX_NAME_LENGTH] = "GPU timings unavailable";
	char status_message[GPU_TIMINGS_MAX_DEPENDENCY_TEXT_LENGTH] = {};
	bool display_is_frozen = false;
};

GpuTimings& gpu_timings_get()
{
	static GpuTimings timings;
	return timings;
}

void gpu_timings_set_available(bool in_available, const char* in_unavailable_reason = nullptr)
{
	GpuTimings& timings = gpu_timings_get();
	std::lock_guard<std::mutex> lock(timings.mutex);
	timings.available = in_available;
	if (!in_available)
	{
		snprintf(
			timings.unavailable_reason,
			sizeof(timings.unavailable_reason),
			"%s",
			in_unavailable_reason ? in_unavailable_reason : "GPU timings unavailable"
		);
	}
	else
	{
		timings.unavailable_reason[0] = '\0';
	}
	++timings.completed_generation;
}

bool gpu_timings_are_available()
{
	GpuTimings& timings = gpu_timings_get();
	std::lock_guard<std::mutex> lock(timings.mutex);
	return timings.available;
}

void gpu_timings_copy_unavailable_reason(char* out_reason, size_t in_reason_size)
{
	if (!out_reason || in_reason_size == 0)
	{
		return;
	}

	GpuTimings& timings = gpu_timings_get();
	std::lock_guard<std::mutex> lock(timings.mutex);
	snprintf(out_reason, in_reason_size, "%s", timings.unavailable_reason);
}

void gpu_timings_set_status_message(const char* in_status_message)
{
	GpuTimings& timings = gpu_timings_get();
	std::lock_guard<std::mutex> lock(timings.mutex);
	snprintf(
		timings.status_message,
		sizeof(timings.status_message),
		"%s",
		in_status_message ? in_status_message : ""
	);
	++timings.completed_generation;
}

void gpu_timings_copy_status_message(char* out_status_message, size_t in_status_message_size)
{
	if (!out_status_message || in_status_message_size == 0)
	{
		return;
	}

	GpuTimings& timings = gpu_timings_get();
	std::lock_guard<std::mutex> lock(timings.mutex);
	snprintf(out_status_message, in_status_message_size, "%s", timings.status_message);
}

GpuTimingFrame* gpu_timings_find_history_frame_locked(GpuTimings& timings, i64 in_frame_index)
{
	for (GpuTimingFrame& frame : timings.history_frames)
	{
		if (frame.valid && frame.frame_index == in_frame_index)
		{
			return &frame;
		}
	}
	return nullptr;
}

void gpu_timings_record_completed_frame_events(i64 in_frame_index, const GpuTimingEvent* in_events, i32 in_event_count)
{
	if (in_frame_index < 0 || !in_events || in_event_count <= 0)
	{
		return;
	}

	GpuTimings& timings = gpu_timings_get();
	std::lock_guard<std::mutex> lock(timings.mutex);
	timings.available = true;
	timings.unavailable_reason[0] = '\0';

	GpuTimingFrame* frame = gpu_timings_find_history_frame_locked(timings, in_frame_index);
	if (!frame)
	{
		frame = &timings.history_frames[timings.next_history_frame_index];
		timings.next_history_frame_index = (timings.next_history_frame_index + 1) % GPU_TIMINGS_MAX_HISTORY_FRAMES;
	}

	frame->events.reset();
	frame->frame_index = in_frame_index;
	frame->valid = true;

	for (i32 event_index = 0; event_index < in_event_count; ++event_index)
	{
		frame->events.add(in_events[event_index]);
	}

	++timings.completed_generation;
}

void gpu_timings_record_completed_frame(i64 in_frame_index, f64 in_elapsed_ms)
{
	if (in_frame_index < 0 || in_elapsed_ms <= 0.0)
	{
		return;
	}

	GpuTimingEvent event = {};
	gpu_timing_event_set_name(event, "GPU Frame");
	event.depth = 0;
	event.parent_index = -1;
	event.lane_index = 0;
	event.type = GpuTimingEventType::Frame;
	event.timestamp_source = GpuTimingTimestampSource::CommandBuffer;
	event.timestamp_confidence = GpuTimingTimestampConfidence::Authoritative;
	event.start_offset_ms = 0.0;
	event.elapsed_ms = in_elapsed_ms;
	event.valid = true;
	gpu_timings_record_completed_frame_events(in_frame_index, &event, 1);
}

void gpu_timings_copy_display_frames_locked(GpuTimings& timings, bool in_freeze, u64 in_cpu_generation, u64 in_completed_generation)
{
	const i32 display_frame_count = cpu_timings_get_display_frame_count(in_freeze);
	timings.display_frame_count = display_frame_count;
	for (i32 frame_index = 0; frame_index < display_frame_count; ++frame_index)
	{
		const i64 cpu_frame_index = cpu_timings_get_display_frame_index(in_freeze, frame_index);
		GpuTimingFrame& display_frame = timings.display_frames[frame_index];
		display_frame.events.reset();
		display_frame.frame_index = cpu_frame_index;
		display_frame.valid = false;

		if (GpuTimingFrame* history_frame = gpu_timings_find_history_frame_locked(timings, cpu_frame_index))
		{
			display_frame = *history_frame;
		}
	}
	timings.display_cpu_generation = in_cpu_generation;
	timings.display_completed_generation = in_completed_generation;
}

void gpu_timings_update_display_frames(bool in_freeze)
{
	const u64 cpu_generation = cpu_timings_get_display_generation(in_freeze);

	GpuTimings& timings = gpu_timings_get();
	std::lock_guard<std::mutex> lock(timings.mutex);
	if (in_freeze)
	{
		if (!timings.display_is_frozen)
		{
			gpu_timings_copy_display_frames_locked(timings, in_freeze, cpu_generation, timings.completed_generation);
			timings.display_is_frozen = true;
		}
		return;
	}

	timings.display_is_frozen = false;
	if (
		timings.display_cpu_generation != cpu_generation ||
		timings.display_completed_generation != timings.completed_generation
	)
	{
		gpu_timings_copy_display_frames_locked(timings, in_freeze, cpu_generation, timings.completed_generation);
	}
}

bool gpu_timings_copy_display_frame(bool in_freeze, i32 in_frame_index, GpuTimingFrame& out_frame)
{
	gpu_timings_update_display_frames(in_freeze);

	GpuTimings& timings = gpu_timings_get();
	std::lock_guard<std::mutex> lock(timings.mutex);
	if (timings.display_frame_count <= 0)
	{
		out_frame.events.reset();
		out_frame.frame_index = -1;
		out_frame.valid = false;
		return false;
	}

	const i32 frame_index = CLAMP(in_frame_index, 0, timings.display_frame_count - 1);
	out_frame = timings.display_frames[frame_index];
	return out_frame.valid;
}

bool gpu_timings_get_display_frame_total_ms(bool in_freeze, f64& out_ms, bool& out_pending)
{
	out_pending = false;
	if (!gpu_timings_are_available())
	{
		return false;
	}

	gpu_timings_update_display_frames(in_freeze);

	GpuTimings& timings = gpu_timings_get();
	std::lock_guard<std::mutex> lock(timings.mutex);
	if (timings.display_frame_count <= 0)
	{
		out_pending = true;
		return false;
	}

	for (i32 frame_index = 0; frame_index < timings.display_frame_count; ++frame_index)
	{
		const GpuTimingFrame& gpu_frame = timings.display_frames[frame_index];
		if (!gpu_frame.valid)
		{
			continue;
		}

		for (const GpuTimingEvent& event : gpu_frame.events)
		{
			if (event.valid && event.type == GpuTimingEventType::Frame && event.elapsed_ms > 0.0)
			{
				out_ms = event.elapsed_ms;
				return true;
			}
		}
	}

	out_pending = true;
	return false;
}

bool gpu_timings_get_latest_completed_frame_total_ms(f64& out_ms, bool& out_pending)
{
	out_pending = false;
	if (!gpu_timings_are_available())
	{
		return false;
	}

	GpuTimings& timings = gpu_timings_get();
	std::lock_guard<std::mutex> lock(timings.mutex);

	i64 latest_frame_index = -1;
	f64 latest_frame_ms = 0.0;
	for (const GpuTimingFrame& gpu_frame : timings.history_frames)
	{
		if (!gpu_frame.valid || gpu_frame.frame_index <= latest_frame_index)
		{
			continue;
		}

		for (const GpuTimingEvent& event : gpu_frame.events)
		{
			if (event.valid && event.type == GpuTimingEventType::Frame && event.elapsed_ms > 0.0)
			{
				latest_frame_index = gpu_frame.frame_index;
				latest_frame_ms = event.elapsed_ms;
				break;
			}
		}
	}

	if (latest_frame_index >= 0)
	{
		out_ms = latest_frame_ms;
		return true;
	}

	out_pending = true;
	return false;
}

struct CpuTimingScope
{
	explicit CpuTimingScope(const char* in_name)
	{
		event_index = cpu_timings_begin_scope(in_name);
	}

	~CpuTimingScope()
	{
		cpu_timings_end_scope(event_index);
	}

	CpuTimingScope(const CpuTimingScope&) = delete;
	CpuTimingScope& operator=(const CpuTimingScope&) = delete;

private:
	i32 event_index = -1;
};

struct CpuTimingBackendScope
{
	CpuTimingBackendScope(const char* in_api_name, const char* in_pass_name)
	{
		snprintf(
			name,
			sizeof(name),
			"%s: %s",
			in_api_name ? in_api_name : "(unnamed)",
			in_pass_name ? in_pass_name : "(unnamed)"
		);
		event_index = cpu_timings_begin_scope(name);
	}

	~CpuTimingBackendScope()
	{
		cpu_timings_end_scope(event_index);
	}

	CpuTimingBackendScope(const CpuTimingBackendScope&) = delete;
	CpuTimingBackendScope& operator=(const CpuTimingBackendScope&) = delete;

private:
	char name[CPU_TIMINGS_MAX_NAME_LENGTH] = {};
	i32 event_index = -1;
};

struct CpuTimingFrameScope
{
	explicit CpuTimingFrameScope(const char* in_name)
	{
		cpu_timings_begin_frame();
		event_index = cpu_timings_begin_scope(in_name);
	}

	~CpuTimingFrameScope()
	{
		cpu_timings_end_scope(event_index);
		cpu_timings_end_frame();
	}

	CpuTimingFrameScope(const CpuTimingFrameScope&) = delete;
	CpuTimingFrameScope& operator=(const CpuTimingFrameScope&) = delete;

private:
	i32 event_index = -1;
};

#define CPU_TIMING_JOIN_INNER(a, b) a##b
#define CPU_TIMING_JOIN(a, b) CPU_TIMING_JOIN_INNER(a, b)
#define CPU_TIMING_SCOPE(name) CpuTimingScope CPU_TIMING_JOIN(cpu_timing_scope_, __LINE__)(name)
#define CPU_TIMING_BACKEND_SCOPE(api_name, pass_name) CpuTimingBackendScope CPU_TIMING_JOIN(cpu_timing_backend_scope_, __LINE__)(api_name, pass_name)
#define CPU_TIMING_FRAME(name) CpuTimingFrameScope CPU_TIMING_JOIN(cpu_timing_frame_scope_, __LINE__)(name)
#define CPU_TIMING_FUNCTION() CPU_TIMING_SCOPE(__func__)

#else

struct CpuTimingEvent {};
struct GpuTimingEvent {};
struct GpuTimingFrame {};

void cpu_timings_begin_frame() {}
void cpu_timings_end_frame() {}
i32 cpu_timings_begin_scope(const char*) { return -1; }
void cpu_timings_record_event(const char*, u64, u64) {}
void cpu_timings_end_scope(i32) {}
i32 cpu_timings_get_display_frame_count(bool) { return 0; }
i64 cpu_timings_get_current_frame_index() { return -1; }
i64 cpu_timings_get_display_frame_index(bool, i32 = 0) { return -1; }
i32 cpu_timings_get_display_frame_age(bool, i32 = 0) { return 0; }
bool cpu_timings_get_display_frame_total_ms(bool, f64&) { return false; }
bool cpu_timings_get_latest_frame_total_ms(f64&) { return false; }
void gpu_timings_set_available(bool, const char* = nullptr) {}
bool gpu_timings_are_available() { return false; }
void gpu_timings_copy_unavailable_reason(char*, size_t) {}
void gpu_timings_set_status_message(const char*) {}
void gpu_timings_copy_status_message(char*, size_t) {}
void gpu_timings_record_completed_frame_events(i64, const GpuTimingEvent*, i32) {}
void gpu_timings_record_completed_frame(i64, f64) {}
bool gpu_timings_copy_display_frame(bool, i32, GpuTimingFrame&) { return false; }
bool gpu_timings_get_display_frame_total_ms(bool, f64&, bool& out_pending) { out_pending = false; return false; }
bool gpu_timings_get_latest_completed_frame_total_ms(f64&, bool& out_pending) { out_pending = false; return false; }

#define CPU_TIMING_SCOPE(name)
#define CPU_TIMING_BACKEND_SCOPE(api_name, pass_name)
#define CPU_TIMING_FRAME(name)
#define CPU_TIMING_FUNCTION()

#endif
