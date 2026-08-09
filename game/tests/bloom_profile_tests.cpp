#include <algorithm>
#include <cassert>
#include <cmath>

#include "render/bloom_profile.inl"

static void expect_near(float actual, float expected, float tolerance = 1e-6f)
{
	assert(std::abs(actual - expected) <= tolerance);
}

static void validate_profile(int band_count)
{
	const BloomProfile::ResolvedProfile profile = BloomProfile::resolve(band_count);
	assert(profile.band_count == std::clamp(
		band_count, 1, BloomProfile::MAX_BAND_COUNT));

	float red_sum = 0.0f;
	float green_sum = 0.0f;
	float blue_sum = 0.0f;
	for (int band = 0; band < profile.band_count; ++band)
	{
		const BloomProfile::RgbWeight weight = profile.bands[band];
		assert(std::isfinite(weight.r) && weight.r >= 0.0f);
		assert(std::isfinite(weight.g) && weight.g >= 0.0f);
		assert(std::isfinite(weight.b) && weight.b >= 0.0f);
		expect_near(weight.r, weight.g);
		expect_near(weight.g, weight.b);
		red_sum += weight.r;
		green_sum += weight.g;
		blue_sum += weight.b;
	}
	expect_near(red_sum, 1.0f);
	expect_near(green_sum, 1.0f);
	expect_near(blue_sum, 1.0f);
}

static float reconstruct_recursively(
	const BloomProfile::ResolvedProfile& profile,
	const float* bands)
{
	float reconstructed = bands[profile.band_count - 1];
	for (int fine_band = profile.band_count - 2; fine_band >= 0; --fine_band)
	{
		const BloomProfile::RgbWeight ratio =
			BloomProfile::reconstruction_ratio(profile, fine_band);
		reconstructed = bands[fine_band] + reconstructed * ratio.r;
	}
	return reconstructed * profile.bands[0].r;
}

int main()
{
	for (int band_count = 1; band_count <= BloomProfile::MAX_BAND_COUNT; ++band_count)
	{
		validate_profile(band_count);

		const BloomProfile::ResolvedProfile profile = BloomProfile::resolve(band_count);
		float band_values[BloomProfile::MAX_BAND_COUNT] = {};
		float direct_sum = 0.0f;
		for (int band = 0; band < band_count; ++band)
		{
			band_values[band] = 0.25f + 0.75f * (float)(band + 1);
			direct_sum += profile.bands[band].r * band_values[band];
		}
		expect_near(reconstruct_recursively(profile, band_values), direct_sum, 2e-6f);

		for (int band = 0; band < band_count; ++band)
		{
			band_values[band] = 1.0f;
		}
		expect_near(reconstruct_recursively(profile, band_values), 1.0f, 2e-6f);
	}

	const BloomProfile::ResolvedProfile full_profile =
		BloomProfile::resolve(BloomProfile::MAX_BAND_COUNT);
	for (int band = 0; band < BloomProfile::MAX_BAND_COUNT; ++band)
	{
		expect_near(
			full_profile.bands[band].r,
			BloomProfile::DIFFRACTION_WEIGHTS[band]);
	}

	validate_profile(0);
	validate_profile(BloomProfile::MAX_BAND_COUNT + 1);
	return 0;
}
