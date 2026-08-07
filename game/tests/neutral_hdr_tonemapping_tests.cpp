#include <algorithm>
#include <cassert>
#include <cmath>
#include <cfloat>

struct RGB
{
	float r;
	float g;
	float b;
};

static constexpr float LUMA_R = 0.2126f;
static constexpr float LUMA_G = 0.7152f;
static constexpr float LUMA_B = 0.0722f;
static constexpr float EXPOSURE_SCALE = 1.5172523f;

static RGB neutral_hdr(RGB color)
{
	color = {
		std::max(color.r, 0.0f),
		std::max(color.g, 0.0f),
		std::max(color.b, 0.0f),
	};
	const float input_peak = std::max(color.r, std::max(color.g, color.b));
	if (input_peak <= 1e-8f) return {};
	const float luminance = input_peak * (
		color.r / input_peak * LUMA_R
		+ color.g / input_peak * LUMA_G
		+ color.b / input_peak * LUMA_B);
	if (luminance <= 1e-8f) return {};
	const float exposed_luminance = luminance * EXPOSURE_SCALE;
	const float mapped_luminance = 1.0f - 1.0f / (1.0f + exposed_luminance);
	const float multiplier = mapped_luminance / luminance;
	RGB mapped = {
		color.r * multiplier,
		color.g * multiplier,
		color.b * multiplier,
	};
	const float peak = std::max(mapped.r, std::max(mapped.g, mapped.b));
	if (peak > 1.0f)
	{
		mapped.r /= peak;
		mapped.g /= peak;
		mapped.b /= peak;
	}
	return {
		std::clamp(mapped.r, 0.0f, 1.0f),
		std::clamp(mapped.g, 0.0f, 1.0f),
		std::clamp(mapped.b, 0.0f, 1.0f),
	};
}

static void expect_near(float actual, float expected, float tolerance)
{
	assert(std::abs(actual - expected) <= tolerance);
}

static void expect_valid(RGB color)
{
	assert(std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b));
	assert(color.r >= 0.0f && color.r <= 1.0f);
	assert(color.g >= 0.0f && color.g <= 1.0f);
	assert(color.b >= 0.0f && color.b <= 1.0f);
}

int main()
{
	const RGB black = neutral_hdr({0.0f, 0.0f, 0.0f});
	expect_near(black.r, 0.0f, 0.0f);
	expect_near(black.g, 0.0f, 0.0f);
	expect_near(black.b, 0.0f, 0.0f);

	const RGB middle_gray = neutral_hdr({0.18f, 0.18f, 0.18f});
	expect_near(middle_gray.r, 0.214519f, 1e-5f);
	expect_near(middle_gray.g, 0.214519f, 1e-5f);
	expect_near(middle_gray.b, 0.214519f, 1e-5f);

	float previous = -1.0f;
	for (int step = 0; step <= 4096; ++step)
	{
		const float value = 64.0f * (float)step / 4096.0f;
		const RGB output = neutral_hdr({value, value, value});
		expect_valid(output);
		assert(output.r + 1e-6f >= previous);
		expect_near(output.r, output.g, 1e-6f);
		expect_near(output.g, output.b, 1e-6f);
		previous = output.r;
	}

	const RGB ratio_input = {12.0f, 3.0f, 0.75f};
	const RGB ratio_output = neutral_hdr(ratio_input);
	expect_valid(ratio_output);
	expect_near(ratio_output.r / ratio_output.g,
		ratio_input.r / ratio_input.g, 1e-5f);
	expect_near(ratio_output.g / ratio_output.b,
		ratio_input.g / ratio_input.b, 1e-5f);

	const RGB directed[] = {
		{-10.0f, -1.0f, -0.1f},
		{1e-20f, 1e-20f, 1e-20f},
		{1.0f, 0.0f, 0.0f},
		{0.0f, 65504.0f, 1.0f},
		{FLT_MAX, FLT_MAX * 0.5f, FLT_MAX * 0.25f},
	};
	for (RGB input : directed) expect_valid(neutral_hdr(input));
	return 0;
}
