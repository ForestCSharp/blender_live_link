#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cmath>

struct RGB
{
	float r;
	float g;
	float b;
};

static constexpr float MIDDLE_GRAY_MATCH_SCALE = 1.4139944f;

// CPU mirror of Khronos PBR Neutral, pinned to reference commit
// f5dc101149fc5c85c0f9852fe2ba438853e8a7d1. Copyright 2024 The Khronos
// Group, Inc.; licensed under Apache-2.0. See the repository license copy at
// LICENSES/Khronos-ToneMapping-Apache-2.0.txt.
static RGB pbr_neutral(RGB color, bool match_existing_middle_gray = false)
{
	color = {
		std::max(color.r, 0.0f),
		std::max(color.g, 0.0f),
		std::max(color.b, 0.0f),
	};
	if (match_existing_middle_gray)
	{
		const float safe_input_max = FLT_MAX / MIDDLE_GRAY_MATCH_SCALE;
		color.r = std::min(color.r, safe_input_max) * MIDDLE_GRAY_MATCH_SCALE;
		color.g = std::min(color.g, safe_input_max) * MIDDLE_GRAY_MATCH_SCALE;
		color.b = std::min(color.b, safe_input_max) * MIDDLE_GRAY_MATCH_SCALE;
	}

	constexpr float start_compression = 0.8f - 0.04f;
	constexpr float desaturation = 0.15f;
	const float x = std::min(color.r, std::min(color.g, color.b));
	const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
	color.r -= offset;
	color.g -= offset;
	color.b -= offset;

	const float peak = std::max(color.r, std::max(color.g, color.b));
	if (peak < start_compression) return color;

	constexpr float d = 1.0f - start_compression;
	const float new_peak = 1.0f - d * d / (peak + d - start_compression);
	color.r *= new_peak / peak;
	color.g *= new_peak / peak;
	color.b *= new_peak / peak;

	const float g = 1.0f - 1.0f / (desaturation * (peak - new_peak) + 1.0f);
	color.r += (new_peak - color.r) * g;
	color.g += (new_peak - color.g) * g;
	color.b += (new_peak - color.b) * g;
	return color;
}

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

static void expect_valid(RGB color)
{
	assert(std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b));
	assert(color.r >= 0.0f && color.r <= 1.0f);
	assert(color.g >= 0.0f && color.g <= 1.0f);
	assert(color.b >= 0.0f && color.b <= 1.0f);
}

static void test_black_and_middle_gray()
{
	expect_rgb_near(pbr_neutral({0.0f, 0.0f, 0.0f}), {}, 0.0f);
	expect_rgb_near(pbr_neutral({-10.0f, -1.0f, -0.1f}), {}, 0.0f);
	expect_rgb_near(
		pbr_neutral({0.18f, 0.18f, 0.18f}),
		{0.14f, 0.14f, 0.14f},
		1e-6f);
	expect_rgb_near(
		pbr_neutral({0.18f, 0.18f, 0.18f}, true),
		{0.214519f, 0.214519f, 0.214519f},
		1e-6f);
}

static void test_reference_boundaries()
{
	// The near-black offset joins the constant 0.04 dielectric offset at 0.08.
	expect_rgb_near(
		pbr_neutral({0.04f, 0.04f, 0.04f}),
		{0.01f, 0.01f, 0.01f},
		1e-6f);
	const RGB below_offset = pbr_neutral({0.08f - 1e-5f, 0.08f - 1e-5f, 0.08f - 1e-5f});
	const RGB above_offset = pbr_neutral({0.08f + 1e-5f, 0.08f + 1e-5f, 0.08f + 1e-5f});
	assert(std::abs(above_offset.r - below_offset.r) < 3e-5f);

	// Compression begins continuously when the post-offset peak reaches 0.76.
	expect_rgb_near(pbr_neutral({0.76f, 0.0f, 0.0f}), {0.76f, 0.0f, 0.0f}, 1e-6f);
	const RGB below_compression = pbr_neutral({0.76f - 1e-5f, 0.0f, 0.0f});
	const RGB above_compression = pbr_neutral({0.76f + 1e-5f, 0.0f, 0.0f});
	assert(std::abs(above_compression.r - below_compression.r) < 3e-5f);
}

static void test_reference_vectors()
{
	// Independently evaluated from the pinned Khronos equations. These vectors
	// guard the CPU mirror against changing in lockstep with the shader port.
	expect_rgb_near(
		pbr_neutral({1.0f, 1.0f, 1.0f}),
		{0.869090909f, 0.869090909f, 0.869090909f},
		2e-6f);
	expect_rgb_near(
		pbr_neutral({4.0f, 2.0f, 0.5f}),
		{0.983255814f, 0.639951387f, 0.382473067f},
		2e-6f);
	expect_rgb_near(
		pbr_neutral({12.0f, 0.25f, 2.0f}),
		{0.994965035f, 0.625367540f, 0.680413975f},
		2e-6f);
}

static void test_diffuse_color_reproduction()
{
	const RGB inputs[] = {
		{0.08f, 0.08f, 0.08f},
		{0.18f, 0.35f, 0.70f},
		{0.80f, 0.50f, 0.08f},
		{0.80f, 0.80f, 0.80f},
	};
	for (RGB input : inputs)
	{
		expect_rgb_near(
			pbr_neutral(input),
			{input.r - 0.04f, input.g - 0.04f, input.b - 0.04f},
			1e-6f);
	}
}

static void test_monotonicity_and_bounds()
{
	float previous = -1.0f;
	for (int step = 0; step <= 8192; ++step)
	{
		const float value = 65504.0f * (float)step / 8192.0f;
		const RGB output = pbr_neutral({value, value, value});
		expect_valid(output);
		assert(output.r + 1e-6f >= previous);
		expect_near(output.r, output.g, 1e-6f);
		expect_near(output.g, output.b, 1e-6f);
		previous = output.r;
	}

	const RGB directed[] = {
		{1e-20f, 1e-20f, 1e-20f},
		{1.0f, 0.0f, 0.0f},
		{0.0f, 65504.0f, 1.0f},
		{FLT_MAX, FLT_MAX * 0.5f, FLT_MAX * 0.25f},
	};
	for (RGB input : directed) expect_valid(pbr_neutral(input));
	for (RGB input : directed) expect_valid(pbr_neutral(input, true));
}

static void test_highlight_hue_and_desaturation()
{
	const RGB input = {8.0f, 3.0f, 0.5f};
	const RGB output = pbr_neutral(input);
	expect_valid(output);

	// Mixing only within the input/white plane keeps hue angle around the white
	// axis: ratios between channel differences remain unchanged.
	expect_near(
		(output.r - output.g) / (output.g - output.b),
		(input.r - input.g) / (input.g - input.b),
		1e-5f);

	const float input_chroma = (input.r - input.b) / input.r;
	const float output_chroma = (output.r - output.b) / output.r;
	assert(output_chroma < input_chroma);
}

int main()
{
	test_black_and_middle_gray();
	test_reference_boundaries();
	test_reference_vectors();
	test_diffuse_color_reproduction();
	test_monotonicity_and_bounds();
	test_highlight_hue_and_desaturation();
	return 0;
}
