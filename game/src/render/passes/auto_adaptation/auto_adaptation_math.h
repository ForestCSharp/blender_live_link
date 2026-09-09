#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace AutoAdaptationMath
{
	static constexpr int HISTOGRAM_BIN_COUNT = 256;
	static constexpr int METER_WIDTH = 256;
	static constexpr int METER_HEIGHT = 144;
	static constexpr float MIDDLE_GRAY = 0.18f;
	static constexpr float MIN_HISTOGRAM_EV = -16.0f;
	static constexpr float MAX_HISTOGRAM_EV = 16.0f;
	static constexpr float LOW_PERCENTILE = 0.02f;
	static constexpr float HIGH_PERCENTILE = 0.98f;
	static constexpr float MIN_AUTO_EV = -8.0f;
	static constexpr float MAX_AUTO_EV = 8.0f;
	static constexpr float VIRTUAL_D65_Y = 0.01f;
	static constexpr float EXPOSURE_DECREASE_TAU = 0.35f;
	static constexpr float EXPOSURE_INCREASE_TAU = 1.0f;
	static constexpr float WHITE_BALANCE_TAU = 1.0f;
	static constexpr float MAX_FRAME_DELTA = 0.1f;
	static constexpr float CHROMATICITY_FIXED_SCALE = 65535.0f;

	struct Rgb
	{
		float r;
		float g;
		float b;
	};

	struct HistogramBin
	{
		std::uint32_t count = 0;
		std::uint32_t sum_x = 0;
		std::uint32_t sum_y = 0;
		std::uint32_t padding = 0;
	};
	static_assert(sizeof(HistogramBin) == 16);

	struct Measurement
	{
		float target_ev = 0.0f;
		float white_x = 0.3127f;
		float white_y = 0.3290f;
		float target_log_lms[3] = {};
		std::uint32_t accepted_count = 0;
		bool valid = false;
	};

	inline float clamp(float value, float low, float high)
	{
		return std::max(low, std::min(value, high));
	}

	inline Rgb linear_srgb_to_xyz(Rgb rgb)
	{
		return {
			0.4124564f * rgb.r + 0.3575761f * rgb.g + 0.1804375f * rgb.b,
			0.2126729f * rgb.r + 0.7151522f * rgb.g + 0.0721750f * rgb.b,
			0.0193339f * rgb.r + 0.1191920f * rgb.g + 0.9503041f * rgb.b,
		};
	}

	inline void add_sample(HistogramBin* bins, Rgb rgb)
	{
		if (!std::isfinite(rgb.r) || !std::isfinite(rgb.g) || !std::isfinite(rgb.b)
			|| rgb.r < 0.0f || rgb.g < 0.0f || rgb.b < 0.0f)
		{
			return;
		}
		const Rgb xyz = linear_srgb_to_xyz(rgb);
		if (!(xyz.g > 0.0f) || !std::isfinite(xyz.g)) return;
		const float xyz_sum = xyz.r + xyz.g + xyz.b;
		if (!(xyz_sum > 0.0f) || !std::isfinite(xyz_sum)) return;

		const float ev = std::log2(xyz.g / MIDDLE_GRAY);
		const float normalized = (ev - MIN_HISTOGRAM_EV)
			/ (MAX_HISTOGRAM_EV - MIN_HISTOGRAM_EV);
		const int bin_index = std::clamp(
			(int)std::floor(normalized * (float)HISTOGRAM_BIN_COUNT),
			0, HISTOGRAM_BIN_COUNT - 1);
		HistogramBin& bin = bins[bin_index];
		bin.count += 1;
		bin.sum_x += (std::uint32_t)std::lround(
			clamp(xyz.r / xyz_sum, 0.0f, 1.0f) * CHROMATICITY_FIXED_SCALE);
		bin.sum_y += (std::uint32_t)std::lround(
			clamp(xyz.g / xyz_sum, 0.0f, 1.0f) * CHROMATICITY_FIXED_SCALE);
	}

	inline void bradford_log_gains_from_xy(float x, float y, float* out_log_gains)
	{
		const float safe_y = std::max(y, 1.0e-6f);
		const float source_xyz[3] = {
			x / safe_y,
			1.0f,
			std::max(1.0f - x - y, 0.0f) / safe_y,
		};
		const float d65_xyz[3] = {0.95047f, 1.0f, 1.08883f};
		const float bradford[3][3] = {
			{ 0.8951f,  0.2664f, -0.1614f},
			{-0.7502f,  1.7135f,  0.0367f},
			{ 0.0389f, -0.0685f,  1.0296f},
		};
		for (int row = 0; row < 3; ++row)
		{
			float source_lms = 0.0f;
			float d65_lms = 0.0f;
			for (int column = 0; column < 3; ++column)
			{
				source_lms += bradford[row][column] * source_xyz[column];
				d65_lms += bradford[row][column] * d65_xyz[column];
			}
			out_log_gains[row] = clamp(
				std::log2(std::max(d65_lms, 1.0e-6f) / std::max(source_lms, 1.0e-6f)),
				-1.0f, 1.0f);
		}
	}

	inline Measurement reduce_histogram(
		const HistogramBin* bins,
		float minimum_auto_ev = MIN_AUTO_EV,
		float maximum_auto_ev = MAX_AUTO_EV)
	{
		Measurement result;
		std::uint32_t total_count = 0;
		for (int bin = 0; bin < HISTOGRAM_BIN_COUNT; ++bin)
			total_count += bins[bin].count;
		if (total_count == 0) return result;

		const std::uint32_t low_reject = (std::uint32_t)std::floor(
			(float)total_count * LOW_PERCENTILE);
		const std::uint32_t high_reject = total_count - (std::uint32_t)std::ceil(
			(float)total_count * HIGH_PERCENTILE);
		std::uint32_t low_remaining = low_reject;
		std::uint32_t high_remaining = high_reject;
		std::uint32_t accepted[HISTOGRAM_BIN_COUNT] = {};
		for (int bin = 0; bin < HISTOGRAM_BIN_COUNT; ++bin)
		{
			const std::uint32_t removed = std::min(low_remaining, bins[bin].count);
			accepted[bin] = bins[bin].count - removed;
			low_remaining -= removed;
		}
		for (int bin = HISTOGRAM_BIN_COUNT - 1; bin >= 0; --bin)
		{
			const std::uint32_t removed = std::min(high_remaining, accepted[bin]);
			accepted[bin] -= removed;
			high_remaining -= removed;
		}

		double weighted_ev = 0.0;
		double weighted_x = 0.0;
		double weighted_y = 0.0;
		double luminance_weight = 0.0;
		for (int bin = 0; bin < HISTOGRAM_BIN_COUNT; ++bin)
		{
			if (accepted[bin] == 0 || bins[bin].count == 0) continue;
			const float ev = MIN_HISTOGRAM_EV
				+ (float)bin * (MAX_HISTOGRAM_EV - MIN_HISTOGRAM_EV)
				/ (float)HISTOGRAM_BIN_COUNT;
			result.accepted_count += accepted[bin];
			weighted_ev += (double)ev * (double)accepted[bin];
			const double bin_luminance = (double)MIDDLE_GRAY * std::exp2((double)ev);
			const double weight = bin_luminance * (double)accepted[bin];
			const double average_x = (double)bins[bin].sum_x
				/ ((double)bins[bin].count * (double)CHROMATICITY_FIXED_SCALE);
			const double average_y = (double)bins[bin].sum_y
				/ ((double)bins[bin].count * (double)CHROMATICITY_FIXED_SCALE);
			weighted_x += average_x * weight;
			weighted_y += average_y * weight;
			luminance_weight += weight;
		}
		if (result.accepted_count == 0 || !(luminance_weight > 0.0)) return result;

		const float low_auto_ev = std::min(minimum_auto_ev, maximum_auto_ev);
		const float high_auto_ev = std::max(minimum_auto_ev, maximum_auto_ev);
		result.target_ev = clamp(
			-(float)(weighted_ev / (double)result.accepted_count),
			low_auto_ev, high_auto_ev);
		const float measured_x = (float)(weighted_x / luminance_weight);
		const float measured_y = (float)(weighted_y / luminance_weight);
		const float mean_y = (float)(luminance_weight / (double)result.accepted_count);
		const float safe_measured_y = std::max(measured_y, 1.0e-6f);
		float mixed_x_value = measured_x / safe_measured_y * mean_y
			+ 0.3127f / 0.3290f * VIRTUAL_D65_Y;
		float mixed_y_value = mean_y + VIRTUAL_D65_Y;
		float mixed_z_value = std::max(1.0f - measured_x - measured_y, 0.0f)
			/ safe_measured_y * mean_y
			+ (1.0f - 0.3127f - 0.3290f) / 0.3290f * VIRTUAL_D65_Y;
		const float mixed_sum = mixed_x_value + mixed_y_value + mixed_z_value;
		result.white_x = mixed_x_value / std::max(mixed_sum, 1.0e-6f);
		result.white_y = mixed_y_value / std::max(mixed_sum, 1.0e-6f);
		bradford_log_gains_from_xy(result.white_x, result.white_y, result.target_log_lms);
		result.valid = true;
		return result;
	}

	inline float smooth(float current, float target, float delta_time, float tau)
	{
		const float dt = clamp(delta_time, 0.0f, MAX_FRAME_DELTA);
		const float alpha = 1.0f - std::exp(-dt / std::max(tau, 1.0e-6f));
		return current + (target - current) * alpha;
	}

	inline float apply_solar_guard(
		float base_target_unclamped,
		float base_target,
		std::uint32_t accepted_count,
		float disc_ev,
		float guard_weight,
		float minimum_auto_ev = MIN_AUTO_EV,
		float maximum_auto_ev = MAX_AUTO_EV)
	{
		if (accepted_count == 0 || !std::isfinite(disc_ev)
			|| !(guard_weight > 0.0f)) return base_target;
		const float weight = clamp(guard_weight, 0.0f, 1.0f);
		const float virtual_count = (float)accepted_count * 0.10f * weight;
		const float guarded_unclamped = -(
			-base_target_unclamped * (float)accepted_count
			+ clamp(disc_ev, MIN_HISTOGRAM_EV, MAX_HISTOGRAM_EV) * virtual_count)
			/ ((float)accepted_count + virtual_count);
		const float guarded = clamp(guarded_unclamped,
			std::min(minimum_auto_ev, maximum_auto_ev),
			std::max(minimum_auto_ev, maximum_auto_ev));
		return std::min(base_target, guarded);
	}

	inline float update_exposure(
		float current,
		float target,
		float delta_time,
		bool measurement_valid,
		bool snap,
		float darkening_seconds = EXPOSURE_DECREASE_TAU,
		float brightening_seconds = EXPOSURE_INCREASE_TAU)
	{
		if (!measurement_valid) return current;
		if (snap) return target;
		return smooth(current, target, delta_time,
			target < current ? darkening_seconds : brightening_seconds);
	}

	inline float update_white_balance_gain(
		float current_log_gain,
		float target_log_gain,
		float delta_time,
		bool measurement_valid,
		bool snap,
		float response_seconds = WHITE_BALANCE_TAU)
	{
		if (!measurement_valid) return current_log_gain;
		if (snap) return target_log_gain;
		return smooth(current_log_gain, target_log_gain, delta_time, response_seconds);
	}
}
