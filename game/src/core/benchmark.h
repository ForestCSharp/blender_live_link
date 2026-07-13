#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/timings.h"
#include "render/vulkan_context.h"

struct BenchmarkNamedSamples
{
	std::string name;
	std::vector<f64> values;
};

struct BenchmarkState
{
	bool enabled = false;
	bool finalized = false;
	u64 warmup_frames = 300;
	u64 measured_frames = 1000;
	u64 rendered_frames = 0;
	i64 first_measured_cpu_frame = -1;
	i64 last_cpu_frame_collected = -1;
	i64 last_gpu_frame_collected = -1;
	std::string output_path = "benchmark.json";
	std::vector<f64> wall_frame_ms;
	std::vector<f64> cpu_frame_ms;
	std::vector<f64> gpu_frame_ms;
	std::vector<BenchmarkNamedSamples> gpu_pass_ms;
	VulkanMetrics metrics_start = {};

	void configure(u64 in_warmup_frames, u64 in_measured_frames, const std::string& in_output_path)
	{
		enabled = true;
		warmup_frames = in_warmup_frames;
		measured_frames = MAX(in_measured_frames, 1ull);
		output_path = in_output_path.empty() ? "benchmark.json" : in_output_path;
		printf("Benchmark: %llu warmup + %llu measured frames -> %s\n",
			(unsigned long long)warmup_frames,
			(unsigned long long)measured_frames,
			output_path.c_str());
	}

	void begin(VulkanContext* ctx)
	{
		if (enabled && warmup_frames == 0)
		{
			metrics_start = ctx->metrics;
			first_measured_cpu_frame = 0;
		}
	}

	BenchmarkNamedSamples& pass_samples(const char* in_name)
	{
		for (BenchmarkNamedSamples& samples : gpu_pass_ms)
		{
			if (samples.name == in_name) return samples;
		}
		gpu_pass_ms.push_back({ .name = in_name ? in_name : "(unnamed)" });
		return gpu_pass_ms.back();
	}

	void after_frame(f64 in_wall_frame_ms, VulkanContext* ctx)
	{
		if (!enabled || finalized) return;
		++rendered_frames;

		i64 latest_cpu_frame = -1;
		f64 latest_cpu_ms = 0.0;
		const bool has_cpu_frame = cpu_timings_get_latest_frame(latest_cpu_frame, latest_cpu_ms);
		if (rendered_frames == warmup_frames)
		{
			metrics_start = ctx->metrics;
			first_measured_cpu_frame = has_cpu_frame ? latest_cpu_frame + 1 : (i64)warmup_frames;
		}

		if (rendered_frames > warmup_frames && rendered_frames <= warmup_frames + measured_frames)
		{
			wall_frame_ms.push_back(in_wall_frame_ms);
			if (has_cpu_frame && latest_cpu_frame != last_cpu_frame_collected)
			{
				cpu_frame_ms.push_back(latest_cpu_ms);
				last_cpu_frame_collected = latest_cpu_frame;
			}
		}

		i64 latest_gpu_frame = -1;
		f64 latest_gpu_ms = 0.0;
		if (
			first_measured_cpu_frame >= 0 &&
			gpu_timings_get_latest_completed_frame(latest_gpu_frame, latest_gpu_ms) &&
			latest_gpu_frame != last_gpu_frame_collected &&
			latest_gpu_frame >= first_measured_cpu_frame &&
			latest_gpu_frame < first_measured_cpu_frame + (i64)measured_frames
		)
		{
			gpu_frame_ms.push_back(latest_gpu_ms);
			last_gpu_frame_collected = latest_gpu_frame;
			#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
			GpuTimingFrame frame;
			if (gpu_timings_copy_history_frame(latest_gpu_frame, frame))
			{
				for (const GpuTimingEvent& event : frame.events)
				{
					if (event.valid && event.type != GpuTimingEventType::Frame && event.elapsed_ms >= 0.0)
					{
						pass_samples(event.name).values.push_back(event.elapsed_ms);
					}
				}
			}
			#endif
		}
	}

	bool should_exit() const
	{
		return enabled && rendered_frames >= warmup_frames + measured_frames + MAX_FRAMES_IN_FLIGHT + 1;
	}
};

inline f64 benchmark_percentile(const std::vector<f64>& in_values, f64 in_percentile)
{
	if (in_values.empty()) return 0.0;
	std::vector<f64> sorted = in_values;
	std::sort(sorted.begin(), sorted.end());
	const f64 position = CLAMP(in_percentile, 0.0, 1.0) * (f64)(sorted.size() - 1);
	const size_t lower = (size_t)position;
	const size_t upper = MIN(lower + 1, sorted.size() - 1);
	const f64 fraction = position - (f64)lower;
	return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

inline void benchmark_write_json_string(FILE* in_file, const std::string& in_value)
{
	fputc('"', in_file);
	for (char character : in_value)
	{
		if (character == '"' || character == '\\') fputc('\\', in_file);
		fputc(character, in_file);
	}
	fputc('"', in_file);
}

inline void benchmark_write_summary(FILE* in_file, const char* in_name, const std::vector<f64>& in_values, bool in_trailing_comma)
{
	fprintf(in_file, "    \"%s\": { \"samples\": %zu, \"median_ms\": %.6f, \"p95_ms\": %.6f }%s\n",
		in_name, in_values.size(), benchmark_percentile(in_values, 0.5), benchmark_percentile(in_values, 0.95),
		in_trailing_comma ? "," : "");
}

inline bool benchmark_finalize(BenchmarkState& state, VulkanContext* ctx)
{
	if (!state.enabled || state.finalized) return true;
	state.finalized = true;
	const VulkanMetrics& end = ctx->metrics;
	const VulkanMemoryStats memory = vulkan_context_get_memory_stats(ctx);
	FILE* output = fopen(state.output_path.c_str(), "wb");
	if (!output)
	{
		printf("Failed to write benchmark output: %s\n", state.output_path.c_str());
		return false;
	}
	fprintf(output, "{\n");
	fprintf(output, "  \"build_config\": \"%s\",\n", GAME_BUILD_CONFIG_NAME);
	fprintf(output, "  \"device\": "); benchmark_write_json_string(output, ctx->physical_device_properties.deviceName); fprintf(output, ",\n");
	fprintf(output, "  \"warmup_frames\": %llu,\n", (unsigned long long)state.warmup_frames);
	fprintf(output, "  \"measured_frames\": %llu,\n", (unsigned long long)state.measured_frames);
	fprintf(output, "  \"timings\": {\n");
	benchmark_write_summary(output, "wall_frame", state.wall_frame_ms, true);
	benchmark_write_summary(output, "cpu_frame", state.cpu_frame_ms, true);
	benchmark_write_summary(output, "gpu_frame", state.gpu_frame_ms, false);
	fprintf(output, "  },\n");
	fprintf(output, "  \"gpu_passes\": {\n");
	for (size_t pass_index = 0; pass_index < state.gpu_pass_ms.size(); ++pass_index)
	{
		const BenchmarkNamedSamples& pass = state.gpu_pass_ms[pass_index];
		fprintf(output, "    "); benchmark_write_json_string(output, pass.name);
		fprintf(output, ": { \"samples\": %zu, \"median_ms\": %.6f, \"p95_ms\": %.6f }%s\n",
			pass.values.size(), benchmark_percentile(pass.values, 0.5), benchmark_percentile(pass.values, 0.95),
			pass_index + 1 < state.gpu_pass_ms.size() ? "," : "");
	}
	fprintf(output, "  },\n");
	fprintf(output, "  \"commands\": { \"draws\": %llu, \"dispatches\": %llu, \"descriptor_update_calls\": %llu, \"descriptor_writes\": %llu, \"descriptors_written\": %llu },\n",
		(unsigned long long)(end.draw_calls - state.metrics_start.draw_calls),
		(unsigned long long)(end.dispatch_calls - state.metrics_start.dispatch_calls),
		(unsigned long long)(end.descriptor_update_calls - state.metrics_start.descriptor_update_calls),
		(unsigned long long)(end.descriptor_writes - state.metrics_start.descriptor_writes),
		(unsigned long long)(end.descriptors_written - state.metrics_start.descriptors_written));
	fprintf(output, "  \"uploads\": { \"bytes\": %llu, \"immediate_submits\": %llu },\n",
		(unsigned long long)(end.upload_bytes - state.metrics_start.upload_bytes),
		(unsigned long long)(end.immediate_submit_count - state.metrics_start.immediate_submit_count));
	fprintf(output, "  \"idle_waits\": { \"queue\": %llu, \"device\": %llu },\n",
		(unsigned long long)(end.queue_wait_idle_count - state.metrics_start.queue_wait_idle_count),
		(unsigned long long)(end.device_wait_idle_count - state.metrics_start.device_wait_idle_count));
	fprintf(output, "  \"pipelines\": { \"count\": %llu, \"creation_ms\": %.6f },\n",
		(unsigned long long)end.pipeline_count, end.pipeline_creation_ms);
	fprintf(output, "  \"vma\": { \"allocations\": %llu, \"allocation_bytes\": %llu, \"blocks\": %llu, \"block_bytes\": %llu, \"device_usage_bytes\": %llu, \"device_budget_bytes\": %llu }\n",
		(unsigned long long)memory.allocation_count, (unsigned long long)memory.allocation_bytes,
		(unsigned long long)memory.block_count, (unsigned long long)memory.block_bytes,
		(unsigned long long)memory.usage_bytes, (unsigned long long)memory.budget_bytes);
	fprintf(output, "}\n");
	fclose(output);
	printf("Benchmark complete: CPU median %.3fms p95 %.3fms | GPU median %.3fms p95 %.3fms | %s\n",
		benchmark_percentile(state.cpu_frame_ms, 0.5), benchmark_percentile(state.cpu_frame_ms, 0.95),
		benchmark_percentile(state.gpu_frame_ms, 0.5), benchmark_percentile(state.gpu_frame_ms, 0.95),
		state.output_path.c_str());
	return true;
}
