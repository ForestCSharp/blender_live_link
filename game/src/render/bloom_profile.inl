#pragma once

#include <algorithm>

// Vulkan-independent bloom frequency-band profile. The RGB representation is
// intentionally retained even while all three channels share the same Stage 1
// weights, so reconstruction does not need another interface change when a
// spectral profile is introduced later.

namespace BloomProfile
{
	static constexpr int MAX_BAND_COUNT = 8;
	static constexpr float DIFFRACTION_WEIGHTS[MAX_BAND_COUNT] = {
		0.36f,
		0.23f,
		0.15f,
		0.10f,
		0.065f,
		0.045f,
		0.03f,
		0.02f,
	};

	struct RgbWeight
	{
		float r;
		float g;
		float b;
	};

	struct ResolvedProfile
	{
		RgbWeight bands[MAX_BAND_COUNT] = {};
		int band_count = 1;
	};

	inline ResolvedProfile resolve(int in_band_count)
	{
		ResolvedProfile result;
		result.band_count = std::clamp(in_band_count, 1, MAX_BAND_COUNT);

		float active_weight_sum = 0.0f;
		for (int band = 0; band < result.band_count; ++band)
		{
			active_weight_sum += DIFFRACTION_WEIGHTS[band];
		}
		const float normalization = 1.0f / active_weight_sum;
		for (int band = 0; band < result.band_count; ++band)
		{
			const float weight = DIFFRACTION_WEIGHTS[band] * normalization;
			result.bands[band] = {weight, weight, weight};
		}
		return result;
	}

	inline RgbWeight reconstruction_ratio(
		const ResolvedProfile& in_profile,
		int in_fine_band)
	{
		const RgbWeight& fine = in_profile.bands[in_fine_band];
		const RgbWeight& coarse = in_profile.bands[in_fine_band + 1];
		return {
			coarse.r / fine.r,
			coarse.g / fine.g,
			coarse.b / fine.b,
		};
	}
}
