#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "GLFW/glfw3.h"

#include "core/runtime_config.h"
#include "render/passes/gi/gi.h"
#include "state/state.h"

class AutomatedScreenshot
{
public:
	// in_benchmark_owns_exit is true when --benchmark-frames is driving the run.
	// The benchmark then decides when to quit; closing the window the moment the
	// capture lands would truncate its measurement.
	bool configure(State& in_state, bool in_benchmark_owns_exit)
	{
		const RuntimeConfig::Config& config = RuntimeConfig::get();
		if (!config.screenshot_path || config.screenshot_path->empty())
		{
			return true;
		}

		const f64 configured_timeout_seconds = config.screenshot_timeout_seconds;
		if (!std::isfinite(configured_timeout_seconds) || configured_timeout_seconds <= 0.0)
		{
			printf(
				"Invalid GAME2_SCREENSHOT_TIMEOUT_SECONDS: %s\n",
				config.screenshot_timeout_text ? config.screenshot_timeout_text->c_str() : ""
			);
			return false;
		}

		output_path = *config.screenshot_path;
		fixed_frame = config.screenshot_frame;
		wait_for_gi = config.screenshot_wait_for_gi;
		timeout_seconds = configured_timeout_seconds;
		started_at = glfwGetTime();
		owns_exit = !in_benchmark_owns_exit;

		// An explicit GAME2_SCREENSHOT_FRAME means "frame N, whatever is on
		// screen" - the CI smoke test wants that. Everything else wants a
		// settled image, which is what the phase machine produces.
		if (config.screenshot_frame_explicit)
		{
			phase = Phase::FixedFrame;
			printf(
				"Automated screenshot armed: %s at frame %llu (timeout %.1fs)\n",
				output_path.c_str(),
				(unsigned long long)fixed_frame,
				timeout_seconds
			);
			return true;
		}

		phase = Phase::WaitingForLiveLink;

		// A/B captures must not depend on how long GI takes to converge.
		in_state.runtime.is_simulating = false;
		in_state.debug_ui.visible = false;

		printf(
			"Automated screenshot armed: %s (timeout %.1fs, GI wait %s)\n",
			output_path.c_str(),
			timeout_seconds,
			wait_for_gi ? "on" : "off"
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

		if (phase == Phase::FixedFrame)
		{
			return;
		}

		// Blender sends an empty reset before the full sync, so "any update
		// drained" is satisfied while the scene is still in flight. Large scenes
		// then get captured empty - test_file is ~39 MB and loses that race
		// regularly. Wait for an update that actually delivered objects.
		if (phase == Phase::WaitingForLiveLink && scene_has_arrived(in_state))
		{
			printf("Automated screenshot: Live Link scene drained\n");
			phase = Phase::WaitingForGi;
		}

		// GI advances one probe per frame, so converging a large scene costs a
		// full pass over every probe - minutes, not seconds. Baseline captures
		// need that determinism (build.sh exports GAME2_SCREENSHOT_WAIT_FOR_GI),
		// but ad-hoc geometry checks do not, and previously had no way to opt out.
		if (phase == Phase::WaitingForGi
			&& (!wait_for_gi
				|| (!in_state.gi.layout_dirty && !in_state.gi.is_updating)))
		{
			printf("Automated screenshot: %s\n",
				wait_for_gi ? "GI update complete" : "skipping GI convergence wait");
			phase = Phase::WaitingForEvenFrame;
		}

		if (phase == Phase::WaitingForEvenFrame
			&& (in_state.vk.frame_number & 1ull) == 0)
		{
			in_state.temporal_aa.history_valid = false;
			in_state.temporal_aa.history_index = 0;
			// Cloud ray jitter uses a deterministic 16-phase sequence. Cover one
			// complete cycle so visual baselines do not capture half-converged
			// low-density or grazing-angle history.
			settle_frames_remaining = 16;
			phase = Phase::Settling;
			printf("Automated screenshot: temporal history reset; settling for 16 frames\n");
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

		if (phase == Phase::FixedFrame && in_state.vk.frame_number == fixed_frame)
		{
			in_state.vk.frame_dump_completed = false;
			in_state.vk.frame_dump_succeeded = false;
			in_state.vk.pending_frame_dump = output_path.c_str();
			phase = Phase::CaptureQueued;
			printf(
				"Automated screenshot: capturing frame %llu\n",
				(unsigned long long)in_state.vk.frame_number
			);
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

	// A failure (timeout included) always ends the run - that is the whole point
	// of the timeout. A successful capture only ends it when nothing else owns
	// the exit condition.
	bool wants_exit() const
	{
		return phase == Phase::Failed || (phase == Phase::Complete && owns_exit);
	}

	bool failed() const
	{
		return phase == Phase::Failed;
	}

private:
	// True once a drained live-link update carried at least one object. An
	// empty scene never satisfies this, so a genuinely empty capture times out
	// loudly rather than silently producing a blank baseline.
	static bool scene_has_arrived(const State& in_state)
	{
		for (const auto& import_stats : in_state.data_oriented.import_history)
		{
			if (import_stats.object_count > 0)
			{
				return true;
			}
		}
		return false;
	}

	enum class Phase
	{
		Disabled,
		FixedFrame,
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
			case Phase::FixedFrame: return "fixed-frame";
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
	bool owns_exit = true;
	f64 started_at = 0.0;
	f64 timeout_seconds = 30.0;
	// Mirrors GAME2_SCREENSHOT_WAIT_FOR_GI. Was parsed and exported but never
	// consulted, so every capture paid full GI convergence regardless.
	bool wait_for_gi = false;
	u64 fixed_frame = 60;
	i32 settle_frames_remaining = 0;
};
