#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace RuntimeConfig
{
	struct Config
	{
		std::optional<long> render_scale;
		bool hide_ui = false;
		bool wireframe = false;
		std::optional<std::string> output_mode;

		std::optional<std::string> screenshot_path;
		unsigned long long screenshot_frame = 60;
		bool screenshot_wait_for_gi = false;
		std::optional<std::string> screenshot_timeout_text;
		double screenshot_timeout_seconds = 600.0;

		bool force_device_local = false;
		std::optional<std::string> present_mode;
		std::optional<std::string> pipeline_cache_path;
		// Dumps the largest live-link payload seen (the full-scene sync) to this
		// path, so `--no-live-link -f <path>` can replay the scene without Blender.
		std::optional<std::string> live_link_capture;
		// Tessellation defaults off and has no UI-free toggle, which makes the
		// GPU-driven tessellation path impossible to exercise headlessly.
		bool tessellation = false;
		bool print_gpu_timings = false;
	};

	inline const char* environment_value(const char* in_name)
	{
		return std::getenv(in_name);
	}

	inline std::optional<std::string> string_value(const char* in_name, const char* in_alias = nullptr)
	{
		const char* value = environment_value(in_name);
		if (!value && in_alias)
		{
			value = environment_value(in_alias);
		}
		return value ? std::optional<std::string>(value) : std::nullopt;
	}

	inline std::optional<long> integer_value(const char* in_name)
	{
		const char* value = environment_value(in_name);
		return value ? std::optional<long>(std::strtol(value, nullptr, 10)) : std::nullopt;
	}

	inline bool is_set(const char* in_name)
	{
		return environment_value(in_name) != nullptr;
	}

	inline Config load_from_environment()
	{
		Config config;
		config.render_scale = integer_value("GAME2_RENDER_SCALE");
		config.hide_ui = is_set("GAME2_HIDE_UI");
		config.wireframe = is_set("GAME2_WIREFRAME");
		config.output_mode = string_value("GAME2_OUTPUT_MODE");
		config.live_link_capture = string_value("GAME2_LIVE_LINK_CAPTURE");
		config.tessellation = is_set("GAME2_TESSELLATION");

		config.screenshot_path = string_value("GAME2_SCREENSHOT");
		if (const char* screenshot_frame = environment_value("GAME2_SCREENSHOT_FRAME"))
		{
			config.screenshot_frame = std::strtoull(screenshot_frame, nullptr, 10);
		}
		config.screenshot_wait_for_gi = is_set("GAME2_SCREENSHOT_WAIT_FOR_GI");
		config.screenshot_timeout_text = string_value("GAME2_SCREENSHOT_TIMEOUT_SECONDS");
		if (config.screenshot_timeout_text)
		{
			config.screenshot_timeout_seconds = std::strtod(config.screenshot_timeout_text->c_str(), nullptr);
		}

		config.force_device_local = is_set("GAME2_FORCE_DEVICE_LOCAL");
		config.present_mode = string_value("GAME_PRESENT_MODE", "GAME2_PRESENT_MODE");
		config.pipeline_cache_path = string_value("GAME_PIPELINE_CACHE", "GAME2_PIPELINE_CACHE");
		config.print_gpu_timings = is_set("GAME2_PRINT_GPU_TIMINGS");
		return config;
	}

	inline const Config& get()
	{
		static const Config config = load_from_environment();
		return config;
	}
}
