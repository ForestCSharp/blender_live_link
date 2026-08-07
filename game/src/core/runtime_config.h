#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace RuntimeConfig
{
	struct Config
	{
		std::optional<long> render_scale;
		std::optional<long> shadow_placement;
		bool shadow_cascade_debug = false;
		bool hide_ui = false;
		std::optional<bool> ssao;
		std::optional<bool> dof;
		std::optional<double> dof_focus;
		std::optional<double> dof_range;
		bool dof_debug = false;
		bool wireframe = false;
		std::optional<bool> taa;
		std::optional<bool> fxaa;
		std::optional<std::string> tonemap_mode;
		std::optional<bool> local_tonemap;
		std::optional<std::string> output_mode;
		std::optional<bool> bloom;
		std::optional<double> bloom_threshold;
		std::optional<double> bloom_soft_knee;
		std::optional<double> bloom_intensity;
		std::optional<long> bloom_mips;
		std::optional<bool> tessellation;
		std::optional<long> tessellation_mode;
		std::optional<long> tessellation_factor;
		bool gi_probes = false;
		std::optional<long> gi_radiance_mode;
		std::optional<long> gi_occlusion_mode;
		std::optional<bool> gi_specular;
		bool test_resize = false;

		std::optional<std::string> screenshot_path;
		unsigned long long screenshot_frame = 60;
		bool screenshot_wait_for_gi = false;
		std::optional<std::string> screenshot_timeout_text;
		double screenshot_timeout_seconds = 600.0;

		bool force_device_local = false;
		std::optional<std::string> present_mode;
		std::optional<std::string> pipeline_cache_path;
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

	inline std::optional<double> float_value(const char* in_name)
	{
		const char* value = environment_value(in_name);
		return value ? std::optional<double>(std::strtod(value, nullptr)) : std::nullopt;
	}

	inline std::optional<bool> boolean_value(const char* in_name)
	{
		const std::optional<long> value = integer_value(in_name);
		return value ? std::optional<bool>(*value != 0) : std::nullopt;
	}

	inline bool is_set(const char* in_name)
	{
		return environment_value(in_name) != nullptr;
	}

	inline Config load_from_environment()
	{
		Config config;
		config.render_scale = integer_value("GAME2_RENDER_SCALE");
		config.shadow_placement = integer_value("GAME2_SHADOW_PLACEMENT");
		config.shadow_cascade_debug = is_set("GAME2_SHADOW_CASCADE_DEBUG");
		config.hide_ui = is_set("GAME2_HIDE_UI");
		config.ssao = boolean_value("GAME2_SSAO");
		config.dof = boolean_value("GAME2_DOF");
		config.dof_focus = float_value("GAME2_DOF_FOCUS");
		config.dof_range = float_value("GAME2_DOF_RANGE");
		config.dof_debug = is_set("GAME2_DOF_DEBUG");
		config.wireframe = is_set("GAME2_WIREFRAME");
		config.taa = boolean_value("GAME2_TAA");
		config.fxaa = boolean_value("GAME2_FXAA");
		config.tonemap_mode = string_value("GAME2_TONEMAP_MODE");
		config.local_tonemap = boolean_value("GAME2_LOCAL_TONEMAP");
		config.output_mode = string_value("GAME2_OUTPUT_MODE");
		config.bloom = boolean_value("GAME2_BLOOM");
		config.bloom_threshold = float_value("GAME2_BLOOM_THRESHOLD");
		config.bloom_soft_knee = float_value("GAME2_BLOOM_SOFT_KNEE");
		config.bloom_intensity = float_value("GAME2_BLOOM_INTENSITY");
		config.bloom_mips = integer_value("GAME2_BLOOM_MIPS");
		config.tessellation = boolean_value("GAME2_TESSELLATION");
		config.tessellation_mode = integer_value("GAME2_TESSELLATION_MODE");
		config.tessellation_factor = integer_value("GAME2_TESSELLATION_FACTOR");
		config.gi_probes = is_set("GAME2_GI_PROBES");
		config.gi_radiance_mode = integer_value("GAME2_GI_RADIANCE_MODE");
		config.gi_occlusion_mode = integer_value("GAME2_GI_OCCLUSION_MODE");
		config.gi_specular = boolean_value("GAME2_GI_SPECULAR");
		config.test_resize = is_set("GAME2_TEST_RESIZE");

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
