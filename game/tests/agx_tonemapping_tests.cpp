#include <cassert>
#include <cmath>
#include <cstdio>

#include "render/agx_tonemapping.h"

using GT7Tonemapping::RGB;

static void expect_near(float actual, float expected, float tolerance)
{
	if (std::abs(actual - expected) > tolerance)
		std::fprintf(stderr, "expected %.9f, got %.9f (tolerance %.9f)\n", expected, actual, tolerance);
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

static AgXTonemapping::LoadedLUT load_target(i32 output_mode, u32 target, u32 expected_crc)
{
	AgXTonemapping::LoadedLUT lut;
	std::string error;
	assert(AgXTonemapping::load_lut(output_mode, &lut, &error));
	assert(error.empty());
	assert(lut.target == target);
	assert(lut.crc32 == expected_crc);
	assert(lut.pixels.length() == AgXTonemapping::LUT_PAYLOAD_SIZE / sizeof(u16));
	return lut;
}

static void test_reference_vectors(
	const AgXTonemapping::LoadedLUT& lut,
	const RGB* inputs,
	const RGB* outputs,
	size_t count)
{
	for (size_t index = 0; index < count; ++index)
	{
		expect_rgb_near(
			AgXTonemapping::sample_lut(lut.pixels, inputs[index], 1.0f, false),
			outputs[index],
			0.015f);
	}
}

static void test_middle_gray_and_monotonicity(
	const AgXTonemapping::LoadedLUT& lut,
	f32 integration_scale,
	f32 target)
{
	const RGB middle = AgXTonemapping::sample_lut(
		lut.pixels, {0.18f, 0.18f, 0.18f}, integration_scale);
	expect_near((middle.r + middle.g + middle.b) / 3.0f, target, 0.001f);

	f32 previous = -1.0f;
	for (u32 step = 0; step <= 8192; ++step)
	{
		const f32 input = AgXTonemapping::LUT_INPUT_MAX * (f32)step / 8192.0f;
		const RGB output = AgXTonemapping::sample_lut(
			lut.pixels, {input, input, input}, 1.0f, false);
		assert(std::isfinite(output.r) && std::isfinite(output.g) && std::isfinite(output.b));
		assert(output.r + 1e-4f >= previous);
		expect_near(output.r, output.g, 0.002f);
		expect_near(output.g, output.b, 0.002f);
		previous = output.r;
	}

	const RGB at_limit = AgXTonemapping::sample_lut(
		lut.pixels, {64.0f, 4.0f, 0.5f}, 1.0f, false);
	const RGB above_limit = AgXTonemapping::sample_lut(
		lut.pixels, {65504.0f, 4.0f, 0.5f}, 1.0f, false);
	expect_rgb_near(at_limit, above_limit, 0.0f);
}

static void test_bounds(const AgXTonemapping::LoadedLUT& lut, bool hdr)
{
	u32 random_state = 0xa65u;
	bool found_extended_srgb = false;
	for (u32 sample = 0; sample < 20000; ++sample)
	{
		const auto random_channel = [&]()
		{
			random_state = random_state * 1664525u + 1013904223u;
			const f32 unit = (f32)(random_state & 0x00ffffffu) / (f32)0x01000000u;
			return AgXTonemapping::LUT_INPUT_MAX * unit * unit * unit * unit;
		};
		const RGB output = AgXTonemapping::sample_lut(
			lut.pixels, {random_channel(), random_channel(), random_channel()}, 1.0f, false);
		assert(std::isfinite(output.r) && std::isfinite(output.g) && std::isfinite(output.b));
		if (hdr)
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
	if (hdr) assert(found_extended_srgb);
}

int main()
{
	const AgXTonemapping::LoadedLUT sdr = load_target(0, 0, 0x140773c7u);
	const AgXTonemapping::LoadedLUT edr = load_target(1, 1, 0x4fde2344u);
	const AgXTonemapping::LoadedLUT hdr10 = load_target(2, 1, 0x4fde2344u);

	const RGB inputs[] = {
		{0.0f, 0.0f, 0.0f}, {0.18f, 0.18f, 0.18f}, {1.0f, 1.0f, 1.0f},
		{4.0f, 2.0f, 0.5f}, {12.0f, 0.25f, 2.0f}, {64.0f, 64.0f, 64.0f},
	};
	const RGB sdr_outputs[] = {
		{0.0f, 0.0f, 0.0f}, {0.1799663f, 0.1799646f, 0.1799949f},
		{0.6274431f, 0.6274421f, 0.6274756f}, {0.9486495f, 0.7497817f, 0.5815994f},
		{1.0f, 0.7303922f, 0.7953750f}, {1.0f, 1.0f, 1.0f},
	};
	const RGB hdr_outputs[] = {
		{0.0f, 0.0f, 0.0f}, {0.0181022f, 0.0181023f, 0.0181051f},
		{0.2243948f, 0.2243961f, 0.2244195f}, {0.7824970f, 0.5207464f, 0.3004327f},
		{1.1492393f, 0.5942379f, 0.6885362f}, {0.9999897f, 0.9999897f, 0.9999897f},
	};
	test_reference_vectors(sdr, inputs, sdr_outputs, 6);
	test_reference_vectors(edr, inputs, hdr_outputs, 6);
	test_reference_vectors(hdr10, inputs, hdr_outputs, 6);
	test_middle_gray_and_monotonicity(
		sdr, AgXTonemapping::SDR_INTEGRATION_SCALE, 0.214519f);
	test_middle_gray_and_monotonicity(
		edr, AgXTonemapping::HDR_INTEGRATION_SCALE, 0.203f);
	test_bounds(sdr, false);
	test_bounds(edr, true);
	std::printf("Blender 5.2 AgX LUT tests passed\n");
	return 0;
}
