#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include "render/auto_adaptation_math.h"

using namespace AutoAdaptationMath;

static void expect_near(float actual, float expected, float tolerance = 1.0e-4f)
{
	if (std::abs(actual - expected) > tolerance)
	{
		std::fprintf(stderr, "Expected %.7f, got %.7f (tolerance %.7f)\n",
			expected, actual, tolerance);
		std::abort();
	}
}

static Measurement measure(const std::vector<Rgb>& samples)
{
	std::array<HistogramBin, HISTOGRAM_BIN_COUNT> bins = {};
	for (Rgb sample : samples) add_sample(bins.data(), sample);
	return reduce_histogram(bins.data());
}

static Measurement measure_with_limits(
	const std::vector<Rgb>& samples,
	float minimum_ev,
	float maximum_ev)
{
	std::array<HistogramBin, HISTOGRAM_BIN_COUNT> bins = {};
	for (Rgb sample : samples) add_sample(bins.data(), sample);
	return reduce_histogram(bins.data(), minimum_ev, maximum_ev);
}

static void test_neutral_exposure_and_white_balance()
{
	Measurement gray = measure(std::vector<Rgb>(1024, {0.18f, 0.18f, 0.18f}));
	assert(gray.valid);
	expect_near(gray.target_ev, 0.0f);
	expect_near(gray.white_x, 0.3127f, 2.0e-4f);
	expect_near(gray.white_y, 0.3290f, 2.0e-4f);
	for (float gain : gray.target_log_lms) expect_near(gain, 0.0f, 2.0e-4f);

	Measurement bright = measure(std::vector<Rgb>(1024, {0.72f, 0.72f, 0.72f}));
	assert(bright.valid);
	expect_near(bright.target_ev, -2.0f);

	Measurement limited = measure_with_limits(
		std::vector<Rgb>(1024, {0.72f, 0.72f, 0.72f}), -1.0f, 1.0f);
	expect_near(limited.target_ev, -1.0f);
}

static void test_percentile_rejection_and_invalid_samples()
{
	std::vector<Rgb> samples(1000, {0.18f, 0.18f, 0.18f});
	for (int i = 0; i < 10; ++i) samples.push_back({10000.0f, 10000.0f, 10000.0f});
	Measurement result = measure(samples);
	assert(result.valid);
	expect_near(result.target_ev, 0.0f);

	Measurement invalid = measure({
		{0.0f, 0.0f, 0.0f},
		{-1.0f, 0.0f, 0.0f},
		{NAN, 1.0f, 1.0f},
		{INFINITY, 1.0f, 1.0f},
	});
	assert(!invalid.valid);
	assert(invalid.accepted_count == 0);
}

static float converge_exposure(float start, float target, float fps, float seconds)
{
	float value = start;
	const int frames = (int)std::lround(fps * seconds);
	for (int frame = 0; frame < frames; ++frame)
		value = update_exposure(value, target, 1.0f / fps, true, false);
	return value;
}

static void test_frame_rate_independent_smoothing()
{
	for (float target : {-4.0f, 4.0f})
	{
		const float tau = target < 0.0f ? EXPOSURE_DECREASE_TAU : EXPOSURE_INCREASE_TAU;
		const float expected = target + (0.0f - target) * std::exp(-2.0f / tau);
		const float at_30 = converge_exposure(0.0f, target, 30.0f, 2.0f);
		const float at_60 = converge_exposure(0.0f, target, 60.0f, 2.0f);
		const float at_120 = converge_exposure(0.0f, target, 120.0f, 2.0f);
		expect_near(at_30, expected, 0.02f);
		expect_near(at_60, expected, 0.02f);
		expect_near(at_120, expected, 0.02f);
		expect_near(at_30, at_60, 2.0e-5f);
		expect_near(at_60, at_120, 2.0e-5f);
	}

	float wb_30 = 0.0f;
	float wb_60 = 0.0f;
	float wb_120 = 0.0f;
	for (int frame = 0; frame < 60; ++frame)
		wb_30 = update_white_balance_gain(wb_30, 1.0f, 1.0f / 30.0f, true, false);
	for (int frame = 0; frame < 120; ++frame)
		wb_60 = update_white_balance_gain(wb_60, 1.0f, 1.0f / 60.0f, true, false);
	for (int frame = 0; frame < 240; ++frame)
		wb_120 = update_white_balance_gain(wb_120, 1.0f, 1.0f / 120.0f, true, false);
	expect_near(wb_30, wb_60, 2.0e-5f);
	expect_near(wb_60, wb_120, 2.0e-5f);

	expect_near(update_exposure(1.25f, -4.0f, 0.1f, false, false), 1.25f);
	expect_near(update_white_balance_gain(-0.4f, 0.8f, 0.1f, false, false), -0.4f);
	expect_near(update_exposure(1.25f, -4.0f, 0.1f, true, true), -4.0f);
	const float custom_response = update_exposure(
		0.0f, -4.0f, 0.25f, true, false, 2.0f, 3.0f);
	expect_near(custom_response, -4.0f * (1.0f - std::exp(-0.1f / 2.0f)));
	const float custom_wb = update_white_balance_gain(
		0.0f, 0.5f, 0.1f, true, false, 2.0f);
	expect_near(custom_wb, 0.5f * (1.0f - std::exp(-0.1f / 2.0f)));
}

static void test_white_balance_limits_and_direction()
{
	Measurement warm = measure(std::vector<Rgb>(1024, {1.0f, 0.45f, 0.12f}));
	Measurement cool = measure(std::vector<Rgb>(1024, {0.15f, 0.5f, 1.0f}));
	assert(warm.valid && cool.valid);
	for (int channel = 0; channel < 3; ++channel)
	{
		assert(std::isfinite(warm.target_log_lms[channel]));
		assert(std::isfinite(cool.target_log_lms[channel]));
		assert(std::abs(warm.target_log_lms[channel]) <= 1.0f);
		assert(std::abs(cool.target_log_lms[channel]) <= 1.0f);
	}
	assert(warm.target_log_lms[0] < warm.target_log_lms[2]);
	assert(cool.target_log_lms[0] > cool.target_log_lms[2]);
}

int main()
{
	test_neutral_exposure_and_white_balance();
	test_percentile_rejection_and_invalid_samples();
	test_frame_rate_independent_smoothing();
	test_white_balance_limits_and_direction();
	return 0;
}
