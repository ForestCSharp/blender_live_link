#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <vector>

#include "render/gt7_tonemapping.h"

using GT7Tonemapping::RGB;

static void expect_near(f32 actual, f32 expected, f32 tolerance)
{
	if (std::abs(actual - expected) > tolerance)
	{
		std::fprintf(stderr, "Expected %.9f, got %.9f (tolerance %.9f)\n",
			expected, actual, tolerance);
		std::abort();
	}
}

static void test_reference_vectors()
{
	const RGB inputs[] = {
		{0.5f, 1.23f, 0.75f},
		{12.3f, 34.3f, 56.9f},
		{1504.7f, 64.51f, 0.5f},
	};
	const RGB expected[] = {
		{0.199630708f, 0.490702361f, 0.299564064f},
		{1.0f, 1.0f, 1.0f},
		{1.0f, 1.0f, 0.738762081f},
	};
	for (u32 index = 0; index < 3; ++index)
	{
		const RGB output = GT7Tonemapping::apply_sdr_rec2020(inputs[index]);
		expect_near(output.r, expected[index].r, 1e-4f);
		expect_near(output.g, expected[index].g, 1e-4f);
		expect_near(output.b, expected[index].b, 1e-4f);
	}
}

static u32 random_state = 0x4a3b2c1du;

static f32 random_unit()
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return (f32)(random_state & 0x00ffffffu) / (f32)0x01000000u;
}

static f32 agx_neutral(f32 value)
{
	value = std::log2(MAX(value, 1e-10f));
	value = CLAMP((value + 12.47393f) / (4.026069f + 12.47393f), 0.0f, 1.0f);
	const f32 value_2 = value * value;
	const f32 value_4 = value_2 * value_2;
	const f32 contrasted = 15.5f * value_4 * value_2
		- 40.14f * value_4 * value
		+ 31.96f * value_4
		- 6.868f * value_2 * value
		+ 0.4298f * value_2
		+ 0.1191f * value
		- 0.00232f;
	return std::pow(MAX(contrasted, 0.0f), 2.2f);
}

static void add_rgb_errors(std::vector<f32>& errors, RGB actual, RGB expected)
{
	assert(std::isfinite(actual.r) && std::isfinite(actual.g) && std::isfinite(actual.b));
	assert(std::isfinite(expected.r) && std::isfinite(expected.g) && std::isfinite(expected.b));
	assert(actual.r >= 0.0f && actual.r <= 1.0f);
	assert(actual.g >= 0.0f && actual.g <= 1.0f);
	assert(actual.b >= 0.0f && actual.b <= 1.0f);
	errors.push_back(std::abs(actual.r - expected.r));
	errors.push_back(std::abs(actual.g - expected.g));
	errors.push_back(std::abs(actual.b - expected.b));
}

static void test_lut_accuracy_and_invariants()
{
	const DynamicArray<u16> lut = GT7Tonemapping::generate_sdr_lut();
	assert(lut.length() ==
		(size_t)GT7Tonemapping::LUT_RESOLUTION * GT7Tonemapping::LUT_RESOLUTION
		* GT7Tonemapping::LUT_RESOLUTION * 4);

	std::vector<f32> errors;
	errors.reserve(30000);
	const RGB directed[] = {
		{0.0f, 0.0f, 0.0f}, {0.18f, 0.18f, 0.18f}, {1.0f, 1.0f, 1.0f},
		{4.0f, 0.0f, 0.0f}, {0.0f, 4.0f, 0.0f}, {0.0f, 0.0f, 4.0f},
		{16.0f, 1.0f, 0.1f}, {0.1f, 16.0f, 1.0f}, {1.0f, 0.1f, 16.0f},
		{64.0f, 64.0f, 64.0f},
	};
	for (RGB input : directed)
	{
		add_rgb_errors(errors,
			GT7Tonemapping::sample_sdr_lut(lut, input, false),
			GT7Tonemapping::apply_sdr_linear_srgb(input));
	}
	for (u32 index = 0; index < 10000; ++index)
	{
		// Bias samples toward the visually important lower range while retaining
		// coverage of the full HDR LUT domain.
		const RGB input = {
			GT7Tonemapping::LUT_INPUT_MAX * std::pow(random_unit(), 4.0f),
			GT7Tonemapping::LUT_INPUT_MAX * std::pow(random_unit(), 4.0f),
			GT7Tonemapping::LUT_INPUT_MAX * std::pow(random_unit(), 4.0f),
		};
		add_rgb_errors(errors,
			GT7Tonemapping::sample_sdr_lut(lut, input, false),
			GT7Tonemapping::apply_sdr_linear_srgb(input));
	}
	std::sort(errors.begin(), errors.end());
	f64 sum = 0.0;
	for (f32 error : errors) sum += error;
	const f64 mean = sum / (f64)errors.size();
	const f32 percentile_99 = errors[(size_t)(errors.size() * 0.99)];
	std::printf("GT7 LUT error: mean %.7f, p99 %.7f, max %.7f\n",
		mean, percentile_99, errors.back());
	assert(mean < 0.005);
	assert(percentile_99 < 0.02f);

	const RGB calibrated_middle_gray = GT7Tonemapping::apply_sdr_linear_srgb({
		0.18f * GT7Tonemapping::SDR_INTEGRATION_SCALE,
		0.18f * GT7Tonemapping::SDR_INTEGRATION_SCALE,
		0.18f * GT7Tonemapping::SDR_INTEGRATION_SCALE,
	});
	expect_near(calibrated_middle_gray.r, agx_neutral(0.18f), 5e-4f);
	expect_near(calibrated_middle_gray.g, agx_neutral(0.18f), 5e-4f);
	expect_near(calibrated_middle_gray.b, agx_neutral(0.18f), 5e-4f);

	// Every extreme of the shaped domain must land on its exact array texel;
	// this catches half-texel and last-layer addressing errors.
	for (u32 b : {0u, GT7Tonemapping::LUT_RESOLUTION - 1})
	for (u32 g : {0u, GT7Tonemapping::LUT_RESOLUTION - 1})
	for (u32 r : {0u, GT7Tonemapping::LUT_RESOLUTION - 1})
	{
		const RGB input = {
			r == 0 ? 0.0f : GT7Tonemapping::LUT_INPUT_MAX,
			g == 0 ? 0.0f : GT7Tonemapping::LUT_INPUT_MAX,
			b == 0 ? 0.0f : GT7Tonemapping::LUT_INPUT_MAX,
		};
		const RGB sampled = GT7Tonemapping::sample_sdr_lut(lut, input, false);
		const RGB texel = GT7Tonemapping::lut_texel(lut, r, g, b);
		expect_near(sampled.r, texel.r, 1e-6f);
		expect_near(sampled.g, texel.g, 1e-6f);
		expect_near(sampled.b, texel.b, 1e-6f);
	}

	f32 previous = -1.0f;
	for (u32 step = 0; step <= 1024; ++step)
	{
		const f32 input = 8.0f * (f32)step / 1024.0f;
		const RGB output = GT7Tonemapping::sample_sdr_lut(lut, {input, input, input});
		assert(output.r + 1e-4f >= previous);
		assert(output.r >= 0.0f && output.r <= 1.0f);
		previous = output.r;
	}

	const RGB clamped = GT7Tonemapping::sample_sdr_lut(lut, {64.0f, 0.5f, 0.5f}, false);
	const RGB extreme = GT7Tonemapping::sample_sdr_lut(lut, {4096.0f, 0.5f, 0.5f}, false);
	expect_near(clamped.r, extreme.r, 1e-6f);
	expect_near(clamped.g, extreme.g, 1e-6f);
	expect_near(clamped.b, extreme.b, 1e-6f);

	// ICtCp chroma is intentionally reduced near the shoulder, but its angular
	// direction should stay stable for a bright, in-gamut colored highlight.
	const RGB highlight = {1.4f, 0.8f, 0.35f};
	const RGB highlight_output = GT7Tonemapping::apply_sdr_linear_srgb(highlight);
	const RGB input_ucs = GT7Tonemapping::rgb_to_ictcp(
		GT7Tonemapping::linear_srgb_to_rec2020(highlight));
	const RGB output_ucs = GT7Tonemapping::rgb_to_ictcp(
		GT7Tonemapping::linear_srgb_to_rec2020(highlight_output));
	const f32 input_hue = std::atan2(input_ucs.b, input_ucs.g);
	const f32 output_hue = std::atan2(output_ucs.b, output_ucs.g);
	f32 hue_delta = std::abs(input_hue - output_hue);
	if (hue_delta > 3.14159265f) hue_delta = 6.28318530f - hue_delta;
	assert(hue_delta < 0.12f);
}

static void test_hdr_lut_accuracy_and_invariants()
{
	const DynamicArray<u16> lut = GT7Tonemapping::generate_hdr_lut();
	assert(lut.length() ==
		(size_t)GT7Tonemapping::LUT_RESOLUTION * GT7Tonemapping::LUT_RESOLUTION
		* GT7Tonemapping::LUT_RESOLUTION * 4);

	const RGB middle_gray = GT7Tonemapping::apply_hdr_linear_srgb({
		0.18f * GT7Tonemapping::HDR_INTEGRATION_SCALE,
		0.18f * GT7Tonemapping::HDR_INTEGRATION_SCALE,
		0.18f * GT7Tonemapping::HDR_INTEGRATION_SCALE,
	});
	expect_near(middle_gray.r, 0.203f, 2e-5f);
	expect_near(middle_gray.g, 0.203f, 2e-5f);
	expect_near(middle_gray.b, 0.203f, 2e-5f);
	expect_near(middle_gray.r * GT7Tonemapping::HDR_PEAK_NITS,
		GT7Tonemapping::HDR_PAPER_WHITE_NITS, 0.02f);

	std::vector<f32> errors;
	errors.reserve(30000);
	for (u32 index = 0; index < 10000; ++index)
	{
		const RGB input = {
			GT7Tonemapping::LUT_INPUT_MAX * std::pow(random_unit(), 4.0f),
			GT7Tonemapping::LUT_INPUT_MAX * std::pow(random_unit(), 4.0f),
			GT7Tonemapping::LUT_INPUT_MAX * std::pow(random_unit(), 4.0f),
		};
		add_rgb_errors(errors,
			GT7Tonemapping::sample_hdr_lut(lut, input, false),
			GT7Tonemapping::apply_hdr_linear_srgb(input));
	}
	std::sort(errors.begin(), errors.end());
	f64 sum = 0.0;
	for (f32 error : errors) sum += error;
	const f64 mean = sum / (f64)errors.size();
	const f32 percentile_99 = errors[(size_t)(errors.size() * 0.99)];
	std::printf("GT7 HDR LUT error: mean %.7f, p99 %.7f, max %.7f\n",
		mean, percentile_99, errors.back());
	assert(mean < 0.005);
	assert(percentile_99 < 0.02f);

	f32 previous = -1.0f;
	for (u32 step = 0; step <= 1024; ++step)
	{
		const f32 input = 8.0f * (f32)step / 1024.0f;
		const RGB output = GT7Tonemapping::sample_hdr_lut(lut, {input, input, input});
		assert(std::isfinite(output.r));
		assert(output.r + 1e-4f >= previous);
		assert(output.r >= 0.0f && output.r <= 1.0f);
		previous = output.r;
	}

	const RGB clamped = GT7Tonemapping::sample_hdr_lut(lut, {64.0f, 0.5f, 0.5f}, false);
	const RGB extreme = GT7Tonemapping::sample_hdr_lut(lut, {4096.0f, 0.5f, 0.5f}, false);
	expect_near(clamped.r, extreme.r, 1e-6f);
	expect_near(clamped.g, extreme.g, 1e-6f);
	expect_near(clamped.b, extreme.b, 1e-6f);

	const RGB highlight = {4.0f, 1.5f, 0.5f};
	const RGB highlight_output = GT7Tonemapping::apply_hdr_linear_srgb(highlight);
	const RGB input_ucs = GT7Tonemapping::rgb_to_ictcp(
		GT7Tonemapping::linear_srgb_to_rec2020(highlight));
	const RGB output_ucs = GT7Tonemapping::rgb_to_ictcp(
		GT7Tonemapping::linear_srgb_to_rec2020(highlight_output));
	const f32 input_hue = std::atan2(input_ucs.b, input_ucs.g);
	const f32 output_hue = std::atan2(output_ucs.b, output_ucs.g);
	f32 hue_delta = std::abs(input_hue - output_hue);
	if (hue_delta > 3.14159265f) hue_delta = 6.28318530f - hue_delta;
	assert(hue_delta < 0.12f);
}

int main()
{
	test_reference_vectors();
	test_lut_accuracy_and_invariants();
	test_hdr_lut_accuracy_and_invariants();
	return 0;
}
