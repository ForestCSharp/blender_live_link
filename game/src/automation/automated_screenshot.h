#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "GLFW/glfw3.h"

#include "render/gi.h"
#include "state/state.h"

class AutomatedScreenshot
{
public:
	bool configure(State& in_state)
	{
		const char* screenshot_path = getenv("GAME2_SCREENSHOT");
		if (screenshot_path != nullptr)
		{
			fixed_frame_configured = true;
			fixed_frame_output_path = screenshot_path;
		}

		const char* screenshot_frame_text = getenv("GAME2_SCREENSHOT_FRAME");
		fixed_frame = screenshot_frame_text ? strtoull(screenshot_frame_text, nullptr, 10) : 60;

		if (getenv("GAME2_SCREENSHOT_WAIT_FOR_GI") == nullptr)
		{
			return true;
		}

		if (!fixed_frame_configured || fixed_frame_output_path.empty())
		{
			printf("Automated screenshot requires GAME2_SCREENSHOT\n");
			return false;
		}

		const char* timeout_text = getenv("GAME2_SCREENSHOT_TIMEOUT_SECONDS");
		const f64 configured_timeout_seconds = timeout_text ? strtod(timeout_text, nullptr) : 600.0;
		if (!std::isfinite(configured_timeout_seconds) || configured_timeout_seconds <= 0.0)
		{
			printf("Invalid GAME2_SCREENSHOT_TIMEOUT_SECONDS: %s\n", timeout_text ? timeout_text : "");
			return false;
		}

		phase = Phase::WaitingForLiveLink;
		output_path = fixed_frame_output_path;
		timeout_seconds = configured_timeout_seconds;
		started_at = glfwGetTime();

		// A/B captures must not depend on how long GI takes to converge.
		in_state.runtime.is_simulating = false;
		in_state.debug_ui.visible = false;

		printf(
			"Automated screenshot armed: %s (timeout %.1fs)\n",
			output_path.c_str(),
			timeout_seconds
		);
		return true;
	}

	void begin_frame(State& in_state, const GI_Scene& in_gi_scene)
	{
		if (!enabled() || finished())
		{
			return;
		}

		if (glfwGetTime() - started_at > timeout_seconds)
		{
			printf(
				"Automated screenshot timeout: imports=%zu GI dirty=%i updating=%i probe=%i/%zu\n",
				in_state.data_oriented.import_history.length(),
				in_state.gi.layout_dirty ? 1 : 0,
				in_state.gi.is_updating ? 1 : 0,
				in_gi_scene.probe_idx_to_update,
				in_gi_scene.probes.length()
			);
			fail("timeout");
			return;
		}

		if (phase == Phase::WaitingForLiveLink
			&& in_state.data_oriented.import_history.length() > 0)
		{
			printf("Automated screenshot: first Live Link update drained\n");
			phase = Phase::WaitingForGi;
		}

		if (phase == Phase::WaitingForGi
			&& !in_state.gi.layout_dirty
			&& !in_state.gi.is_updating)
		{
			printf("Automated screenshot: GI update complete\n");
			phase = Phase::WaitingForEvenFrame;
		}

		if (phase == Phase::WaitingForEvenFrame
			&& (in_state.vk.frame_number & 1ull) == 0)
		{
			in_state.temporal_aa.history_valid = false;
			in_state.temporal_aa.history_index = 0;
			settle_frames_remaining = 8;
			phase = Phase::Settling;
			printf("Automated screenshot: temporal history reset; settling for 8 frames\n");
		}
	}

	void queue_if_ready(State& in_state)
	{
		if (phase == Phase::Ready && (in_state.vk.frame_number & 1ull) == 0)
		{
			in_state.vk.frame_dump_completed = false;
			in_state.vk.frame_dump_succeeded = false;
			in_state.vk.pending_frame_dump = output_path.c_str();
			phase = Phase::CaptureQueued;
			printf(
				"Automated screenshot: capturing frame %llu\n",
				(unsigned long long)in_state.vk.frame_number
			);
			return;
		}

		if (!enabled()
			&& fixed_frame_configured
			&& in_state.vk.frame_number == fixed_frame)
		{
			in_state.vk.frame_dump_completed = false;
			in_state.vk.frame_dump_succeeded = false;
			in_state.vk.pending_frame_dump = fixed_frame_output_path.c_str();
		}
	}

	void after_frame(State& in_state)
	{
		if (phase == Phase::Settling)
		{
			settle_frames_remaining -= 1;
			if (settle_frames_remaining == 0)
			{
				phase = Phase::Ready;
				printf("Automated screenshot: settle complete\n");
			}
			return;
		}

		if (phase == Phase::CaptureQueued && in_state.vk.frame_dump_completed)
		{
			if (in_state.vk.frame_dump_succeeded)
			{
				phase = Phase::Complete;
				printf("Automated screenshot complete: %s\n", output_path.c_str());
			}
			else
			{
				fail("frame readback or file write failed");
			}
		}
	}

	void fail(const char* in_reason)
	{
		if (!enabled() || finished())
		{
			return;
		}
		printf(
			"Automated screenshot failed during %s: %s\n",
			phase_name(phase),
			in_reason
		);
		phase = Phase::Failed;
	}

	bool enabled() const
	{
		return phase != Phase::Disabled;
	}

	bool finished() const
	{
		return phase == Phase::Complete || phase == Phase::Failed;
	}

	bool failed() const
	{
		return phase == Phase::Failed;
	}

private:
	enum class Phase
	{
		Disabled,
		WaitingForLiveLink,
		WaitingForGi,
		WaitingForEvenFrame,
		Settling,
		Ready,
		CaptureQueued,
		Complete,
		Failed,
	};

	static const char* phase_name(Phase in_phase)
	{
		switch (in_phase)
		{
			case Phase::Disabled: return "disabled";
			case Phase::WaitingForLiveLink: return "waiting-for-live-link";
			case Phase::WaitingForGi: return "waiting-for-gi";
			case Phase::WaitingForEvenFrame: return "waiting-for-even-frame";
			case Phase::Settling: return "settling";
			case Phase::Ready: return "ready";
			case Phase::CaptureQueued: return "capture-queued";
			case Phase::Complete: return "complete";
			case Phase::Failed: return "failed";
		}
		return "unknown";
	}

	Phase phase = Phase::Disabled;
	std::string output_path;
	std::string fixed_frame_output_path;
	bool fixed_frame_configured = false;
	f64 started_at = 0.0;
	f64 timeout_seconds = 600.0;
	u64 fixed_frame = 60;
	i32 settle_frames_remaining = 0;
};
