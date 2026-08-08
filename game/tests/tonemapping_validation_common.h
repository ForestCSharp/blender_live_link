#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <vector>

#include "render/aces2_tonemapping.h"
#include "render/agx_tonemapping.h"
#include "render/gt7_tonemapping.h"
#include "tonemapping_shared.h"

namespace TonemappingValidation
{
	using GT7Tonemapping::RGB;

	struct Float4
	{
		float r, g, b, a;
	};
	static_assert(sizeof(Float4) == 16);

	inline RGB pbr_neutral(RGB color, bool match_middle_gray)
	{
		color.r = std::max(color.r, 0.0f);
		color.g = std::max(color.g, 0.0f);
		color.b = std::max(color.b, 0.0f);
		if (match_middle_gray)
		{
			constexpr float scale = 1.4139944f;
			const float safe_max = FLT_MAX / scale;
			color.r = std::min(color.r, safe_max) * scale;
			color.g = std::min(color.g, safe_max) * scale;
			color.b = std::min(color.b, safe_max) * scale;
		}
		constexpr float start_compression = 0.76f;
		constexpr float desaturation = 0.15f;
		const float x = std::min(color.r, std::min(color.g, color.b));
		const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
		color = {color.r - offset, color.g - offset, color.b - offset};
		const float peak = std::max(color.r, std::max(color.g, color.b));
		if (peak < start_compression) return color;
		constexpr float d = 1.0f - start_compression;
		const float new_peak = 1.0f - d * d / (peak + d - start_compression);
		color = color * (new_peak / peak);
		const float g = 1.0f - 1.0f / (desaturation * (peak - new_peak) + 1.0f);
		return color * (1.0f - g) + RGB{new_peak, new_peak, new_peak} * g;
	}

	inline std::vector<Float4> make_corpus()
	{
		std::vector<Float4> result;
		result.reserve(116500);
		const auto add = [&](float r, float g, float b)
		{
			result.push_back({r, g, b, 1.0f});
		};
		add(0.0f, 0.0f, 0.0f);
		add(-1.0f, -0.1f, -100.0f);
		add(-1.0f, 0.18f, 4.0f);
		add(0.18f, 0.18f, 0.18f);
		add(0.08f, 0.08f, 0.08f);
		add(0.76f, 0.76f, 0.76f);
		add(64.0f, 64.0f, 64.0f);
		add(65504.0f, 64.0f, 4.0f);
		for (float boundary : {0.0f, 0.0078125f, 0.08f, 0.76f, 1.0f, 64.0f})
		{
			add(std::max(boundary - 1e-5f, 0.0f), boundary, boundary + 1e-5f);
		}
		for (int step = 0; step <= 4096; ++step)
		{
			const float t = 64.0f * (float)step / 4096.0f;
			add(t, t, t);
		}
		for (int step = 0; step <= 1024; ++step)
		{
			const float t = 64.0f * (float)step / 1024.0f;
			add(t, 0.0f, 0.0f); add(0.0f, t, 0.0f); add(0.0f, 0.0f, t);
			add(t, t, 0.0f); add(t, 0.0f, t); add(0.0f, t, t);
		}
		const float diffuse[] = {0.02f, 0.04f, 0.08f, 0.18f, 0.35f, 0.5f, 0.8f};
		for (float r : diffuse) for (float g : diffuse) for (float b : diffuse) add(r, g, b);
		uint32_t random_state = 0x544d4150u;
		for (int sample = 0; sample < 100000; ++sample)
		{
			const auto channel = [&]()
			{
				random_state = random_state * 1664525u + 1013904223u;
				const float unit = (float)(random_state & 0x00ffffffu) / (float)0x01000000u;
				return 64.0f * unit * unit * unit * unit;
			};
			add(channel(), channel(), channel());
		}
		return result;
	}

	struct Assets
	{
		DynamicArray<u16> gt7;
		ACES2Tonemapping::LoadedLUT aces2;
		AgXTonemapping::LoadedLUT agx;
	};

	inline bool load_assets(int output_mode, Assets* assets, std::string* error)
	{
		assets->gt7 = output_mode == 0
			? GT7Tonemapping::generate_sdr_lut()
			: GT7Tonemapping::generate_hdr_lut();
		if (!ACES2Tonemapping::load_lut(output_mode, &assets->aces2, error)) return false;
		if (!AgXTonemapping::load_lut(output_mode, &assets->agx, error)) return false;
		return true;
	}

	inline float integration_scale(int method, int output_mode)
	{
		const bool sdr = output_mode == 0;
		if (method == TONEMAP_METHOD_AGX)
			return sdr ? AgXTonemapping::SDR_INTEGRATION_SCALE : AgXTonemapping::HDR_INTEGRATION_SCALE;
		if (method == TONEMAP_METHOD_ACES_2)
			return sdr ? ACES2Tonemapping::SDR_INTEGRATION_SCALE : ACES2Tonemapping::HDR_INTEGRATION_SCALE;
		return sdr ? GT7Tonemapping::SDR_INTEGRATION_SCALE : GT7Tonemapping::HDR_INTEGRATION_SCALE;
	}

	inline RGB sample_cpu(
		const Assets& assets,
		int method,
		int output_mode,
		RGB input,
		bool pbr_match_middle_gray)
	{
		if (method == TONEMAP_METHOD_AGX)
			return AgXTonemapping::sample_lut(
				assets.agx.pixels, input, integration_scale(method, output_mode));
		if (method == TONEMAP_METHOD_ACES_2)
			return ACES2Tonemapping::sample_lut(
				assets.aces2.pixels, input, integration_scale(method, output_mode));
		if (method == TONEMAP_METHOD_KHRONOS_PBR_NEUTRAL)
			return pbr_neutral(input, pbr_match_middle_gray);
		return output_mode == 0
			? GT7Tonemapping::sample_sdr_lut(assets.gt7, input)
			: GT7Tonemapping::sample_hdr_lut(assets.gt7, input);
	}
}
