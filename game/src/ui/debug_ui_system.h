#pragma once

#include "core/timings.h"
#include "state/state.h"

namespace DebugUiSystem
{
	// Samples immediate frame timings and maintains the smoothed values shown by
	// the debug UI. This stays available in non-UI builds so frame behavior and
	// timing collection remain independent of WITH_DEBUG_UI.
	void update_frame_stats(State& in_state, f32 in_delta_time)
	{
		State::DebugUiState& debug_ui = in_state.debug_ui;

		debug_ui.immediate_frame_time_ms = in_delta_time * 1000.0f;
		debug_ui.immediate_fps = in_delta_time > 0.0f ? 1.0f / in_delta_time : 0.0f;

		f64 immediate_cpu_time_ms = 0.0;
		debug_ui.immediate_cpu_time_valid = cpu_timings_get_latest_frame_total_ms(immediate_cpu_time_ms);
		if (debug_ui.immediate_cpu_time_valid)
		{
			debug_ui.immediate_cpu_time_ms = (f32) immediate_cpu_time_ms;
			debug_ui.cpu_time_sample_sum_ms += immediate_cpu_time_ms;
			debug_ui.cpu_time_sample_count += 1;
			if (!debug_ui.cpu_time_valid)
			{
				debug_ui.cpu_time_ms = (f32) immediate_cpu_time_ms;
				debug_ui.cpu_time_valid = true;
			}
		}

		f64 immediate_gpu_time_ms = 0.0;
		bool immediate_gpu_time_pending = false;
		debug_ui.immediate_gpu_time_valid = gpu_timings_get_latest_completed_frame_total_ms(
			immediate_gpu_time_ms,
			immediate_gpu_time_pending
		);
		debug_ui.immediate_gpu_time_pending =
			!debug_ui.immediate_gpu_time_valid && immediate_gpu_time_pending;
		if (debug_ui.immediate_gpu_time_valid)
		{
			debug_ui.immediate_gpu_time_ms = (f32) immediate_gpu_time_ms;
			debug_ui.gpu_time_sample_sum_ms += immediate_gpu_time_ms;
			debug_ui.gpu_time_sample_count += 1;
			if (!debug_ui.gpu_time_valid)
			{
				debug_ui.gpu_time_ms = (f32) immediate_gpu_time_ms;
				debug_ui.gpu_time_valid = true;
				debug_ui.gpu_time_pending = false;
			}
		}
		else if (debug_ui.immediate_gpu_time_pending)
		{
			debug_ui.gpu_time_pending = !debug_ui.gpu_time_valid;
		}
		else
		{
			debug_ui.gpu_time_valid = false;
			debug_ui.gpu_time_pending = false;
		}

		debug_ui.stats_sample_elapsed += in_delta_time;
		debug_ui.stats_sample_count += 1;
		if (debug_ui.fps == 0.0f && in_delta_time > 0.0f)
		{
			debug_ui.frame_time_ms = in_delta_time * 1000.0f;
			debug_ui.fps = 1.0f / in_delta_time;
		}
		if (debug_ui.stats_sample_elapsed >= 0.25)
		{
			const f64 average_delta_time = debug_ui.stats_sample_elapsed / debug_ui.stats_sample_count;
			debug_ui.frame_time_ms = (f32) (average_delta_time * 1000.0);
			debug_ui.fps = (f32) (debug_ui.stats_sample_count / debug_ui.stats_sample_elapsed);
			if (debug_ui.cpu_time_sample_count > 0)
			{
				debug_ui.cpu_time_ms = (f32) (
					debug_ui.cpu_time_sample_sum_ms / debug_ui.cpu_time_sample_count
				);
				debug_ui.cpu_time_valid = true;
			}
			if (debug_ui.gpu_time_sample_count > 0)
			{
				debug_ui.gpu_time_ms = (f32) (
					debug_ui.gpu_time_sample_sum_ms / debug_ui.gpu_time_sample_count
				);
				debug_ui.gpu_time_valid = true;
				debug_ui.gpu_time_pending = false;
			}
			else if (!debug_ui.gpu_time_valid)
			{
				debug_ui.gpu_time_pending = debug_ui.immediate_gpu_time_pending;
			}
			debug_ui.stats_sample_elapsed = 0.0;
			debug_ui.stats_sample_count = 0;
			debug_ui.cpu_time_sample_sum_ms = 0.0;
			debug_ui.cpu_time_sample_count = 0;
			debug_ui.gpu_time_sample_sum_ms = 0.0;
			debug_ui.gpu_time_sample_count = 0;
		}
	}
}
