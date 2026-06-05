#pragma once

#include "core/types.h"

static constexpr i32 CPU_TIMINGS_MAX_NAME_LENGTH = 128;

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI

#include <algorithm>
#include <cstdio>

#include "core/stretchy_buffer.h"
#include "sokol/sokol_time.h"

static constexpr i32 CPU_TIMINGS_MAX_DISPLAY_FRAMES = 5;

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
	StretchyBuffer<CpuTimingEvent> previous_frames[CPU_TIMINGS_MAX_DISPLAY_FRAMES];
	StretchyBuffer<CpuTimingEvent> display_frames[CPU_TIMINGS_MAX_DISPLAY_FRAMES];
	StretchyBuffer<i32> active_scope_stack;
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
	event.start_ticks = stm_now();
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
	event.elapsed_ms = stm_ms(stm_diff(event.end_ticks, event.start_ticks));
	event.is_open = false;
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

	const u64 end_ticks = stm_now();

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
	for (i32 frame_index = CPU_TIMINGS_MAX_DISPLAY_FRAMES - 1; frame_index > 0; --frame_index)
	{
		timings.previous_frames[frame_index] = timings.previous_frames[frame_index - 1];
	}
	timings.previous_frames[0] = timings.current_frame;
	timings.previous_frame_count = std::min(timings.previous_frame_count + 1, CPU_TIMINGS_MAX_DISPLAY_FRAMES);
	++timings.previous_frame_generation;

	timings.active_scope_stack.reset();
	timings.frame_active = false;
}

const StretchyBuffer<CpuTimingEvent>& cpu_timings_get_previous_frame()
{
	return cpu_timings_get().previous_frame;
}

void cpu_timings_copy_previous_frames_to_display(CpuTimings& timings)
{
	timings.display_frame = timings.previous_frame;
	timings.display_frame_count = timings.previous_frame_count;
	for (i32 frame_index = 0; frame_index < timings.previous_frame_count; ++frame_index)
	{
		timings.display_frames[frame_index] = timings.previous_frames[frame_index];
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

void cpu_timings_begin_frame() {}
void cpu_timings_end_frame() {}
i32 cpu_timings_begin_scope(const char*) { return -1; }
void cpu_timings_end_scope(i32) {}
i32 cpu_timings_get_display_frame_count(bool) { return 0; }

#define CPU_TIMING_SCOPE(name)
#define CPU_TIMING_BACKEND_SCOPE(api_name, pass_name)
#define CPU_TIMING_FRAME(name)
#define CPU_TIMING_FUNCTION()

#endif
