#include <cassert>
#include <cmath>
#include <cstdio>

#include "render/aces2_tonemapping.h"

using GT7Tonemapping::RGB;

static void expect_near(float actual, float expected, float tolerance)
{
	assert(std::abs(actual - expected) <= tolerance);
}

static void expect_rgb_near(RGB actual, RGB expected, float tolerance)
{
	expect_near(actual.r, expected.r, tolerance);
	expect_near(actual.g, expected.g, tolerance);
	expect_near(actual.b, expected.b, tolerance);
}

static RGB test_linear_srgb_to_rec2020(RGB color)
{
	return {
		0.6274039f * color.r + 0.3292830f * color.g + 0.0433131f * color.b,
		0.0690973f * color.r + 0.9195404f * color.g + 0.0113623f * color.b,
		0.0163914f * color.r + 0.0880133f * color.g + 0.8955953f * color.b,
	};
}

static ACES2Tonemapping::LoadedLUT load_target(i32 target, u32 expected_crc)
{
	ACES2Tonemapping::LoadedLUT lut;
	std::string error;
	assert(ACES2Tonemapping::load_lut(target, &lut, &error));
	assert(error.empty());
	assert(lut.target == (u32)target);
	assert(lut.crc32 == expected_crc);
	assert(lut.pixels.length() == ACES2Tonemapping::LUT_PAYLOAD_SIZE / sizeof(u16));
	return lut;
}

static void test_reference_vectors(
	const ACES2Tonemapping::LoadedLUT& lut,
	const RGB* inputs,
	const RGB* outputs,
	size_t count)
{
	for (size_t index = 0; index < count; ++index)
	{
		expect_rgb_near(
			ACES2Tonemapping::sample_lut(lut.pixels, inputs[index], 1.0f, false),
			outputs[index],
			0.006f);
	}
}

static void test_middle_gray_and_monotonicity(
	const ACES2Tonemapping::LoadedLUT& lut,
	f32 integration_scale,
	f32 target)
{
	const RGB middle = ACES2Tonemapping::sample_lut(
		lut.pixels, {0.18f, 0.18f, 0.18f}, integration_scale);
	expect_near((middle.r + middle.g + middle.b) / 3.0f, target, 0.001f);

	f32 previous = -1.0f;
	for (u32 step = 0; step <= 8192; ++step)
	{
		const f32 input = ACES2Tonemapping::LUT_INPUT_MAX * (f32)step / 8192.0f;
		const RGB output = ACES2Tonemapping::sample_lut(
			lut.pixels, {input, input, input}, 1.0f, false);
		assert(std::isfinite(output.r) && std::isfinite(output.g) && std::isfinite(output.b));
		assert(output.r + 1e-5f >= previous);
		expect_near(output.r, output.g, 0.001f);
		expect_near(output.g, output.b, 0.001f);
		previous = output.r;
	}

	const RGB at_limit = ACES2Tonemapping::sample_lut(
		lut.pixels, {64.0f, 4.0f, 0.5f}, 1.0f, false);
	const RGB above_limit = ACES2Tonemapping::sample_lut(
		lut.pixels, {65504.0f, 4.0f, 0.5f}, 1.0f, false);
	expect_rgb_near(at_limit, above_limit, 0.0f);
}

static void test_bounds(const ACES2Tonemapping::LoadedLUT& lut, bool hdr10)
{
	u32 random_state = 0xace520u;
	bool found_extended_srgb = false;
	for (u32 sample = 0; sample < 20000; ++sample)
	{
		const auto random_channel = [&]()
		{
			random_state = random_state * 1664525u + 1013904223u;
			const f32 unit = (f32)(random_state & 0x00ffffffu) / (f32)0x01000000u;
			return ACES2Tonemapping::LUT_INPUT_MAX * unit * unit * unit * unit;
		};
		const RGB output = ACES2Tonemapping::sample_lut(
			lut.pixels, {random_channel(), random_channel(), random_channel()}, 1.0f, false);
		assert(std::isfinite(output.r) && std::isfinite(output.g) && std::isfinite(output.b));
		if (hdr10)
		{
			found_extended_srgb |= output.r < 0.0f || output.g < 0.0f || output.b < 0.0f
				|| output.r > 1.0f || output.g > 1.0f || output.b > 1.0f;
			const RGB rec2020 = test_linear_srgb_to_rec2020(output);
			assert(rec2020.r >= -0.002f && rec2020.r <= 1.002f);
			assert(rec2020.g >= -0.002f && rec2020.g <= 1.002f);
			assert(rec2020.b >= -0.002f && rec2020.b <= 1.002f);
		}
		else
		{
			assert(output.r >= -0.001f && output.r <= 1.001f);
			assert(output.g >= -0.001f && output.g <= 1.001f);
			assert(output.b >= -0.001f && output.b <= 1.001f);
		}
	}
	if (hdr10) assert(found_extended_srgb);
}

int main()
{
	const ACES2Tonemapping::LoadedLUT sdr = load_target(0, 0x9f558855u);
	const ACES2Tonemapping::LoadedLUT edr = load_target(1, 0x992383abu);
	const ACES2Tonemapping::LoadedLUT hdr10 = load_target(2, 0x4cf941e0u);

	const RGB inputs[] = {
		{0.0f, 0.0f, 0.0f}, {0.18f, 0.18f, 0.18f}, {1.0f, 1.0f, 1.0f},
		{4.0f, 2.0f, 0.5f}, {12.0f, 0.25f, 2.0f}, {64.0f, 64.0f, 64.0f},
	};
	const RGB sdr_outputs[] = {
		{0.0f, 0.0f, 0.0f}, {0.0999991f, 0.0999994f, 0.0999993f},
		{0.4575652f, 0.4575658f, 0.4575658f}, {0.8999217f, 0.6486813f, 0.4051842f},
		{1.0f, 0.5298021f, 0.6109191f}, {0.9915553f, 0.9915555f, 0.9915556f},
	};
	const RGB edr_outputs[] = {
		{0.0f, 0.0f, 0.0f}, {0.0145115f, 0.0145116f, 0.0145115f},
		{0.1065635f, 0.1065639f, 0.1065639f}, {0.3454080f, 0.2147541f, 0.0998950f},
		{0.8602135f, 0.1161065f, 0.2241157f}, {0.9152650f, 0.9152649f, 0.9152651f},
	};
	const RGB hdr10_outputs[] = {
		{0.0f, 0.0f, 0.0f}, {0.0145115f, 0.0145116f, 0.0145115f},
		{0.1065637f, 0.1065639f, 0.1065639f}, {0.3454086f, 0.2147539f, 0.0998950f},
		{0.8602502f, 0.1160995f, 0.2241149f}, {0.9152660f, 0.9152647f, 0.9152651f},
	};
	test_reference_vectors(sdr, inputs, sdr_outputs, 6);
	test_reference_vectors(edr, inputs, edr_outputs, 6);
	test_reference_vectors(hdr10, inputs, hdr10_outputs, 6);
	test_middle_gray_and_monotonicity(
		sdr, ACES2Tonemapping::SDR_INTEGRATION_SCALE, 0.214519f);
	test_middle_gray_and_monotonicity(
		edr, ACES2Tonemapping::HDR_INTEGRATION_SCALE, 0.203f);
	test_middle_gray_and_monotonicity(
		hdr10, ACES2Tonemapping::HDR_INTEGRATION_SCALE, 0.203f);
	test_bounds(sdr, false);
	test_bounds(edr, false);
	test_bounds(hdr10, true);
	std::printf("ACES 2.0 LUT tests passed\n");
	return 0;
}
