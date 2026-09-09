#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>

#include "render/passes/clouds/cloud_math.h"

static void test_shell_intersection_and_sorting()
{
	const CloudRayInterval outer = cloud_sphere_interval_cpu(
		0.0f, 0.0f, 10.0f, 0.0f, 0.0f, -1.0f,
		0.0f, 0.0f, 0.0f, 8.0f);
	assert(outer.valid);
	assert(std::abs(outer.near_distance - 2.0f) < 1.0e-5f);
	assert(std::abs(outer.far_distance - 18.0f) < 1.0e-5f);
	const CloudRayInterval miss = cloud_sphere_interval_cpu(
		0.0f, 0.0f, 10.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 8.0f);
	assert(!miss.valid);

	std::array<float, 4> altitudes = { 8000.0f, 1800.0f, 12000.0f, 300.0f };
	std::sort(altitudes.begin(), altitudes.end());
	assert((altitudes == std::array<float, 4>{ 300.0f, 1800.0f, 8000.0f, 12000.0f }));
}

static void test_periodicity_and_seed_determinism()
{
	constexpr std::uint32_t period = 128;
	constexpr std::uint32_t seed = 0x1234abcdu;
	std::uint32_t checksum_a = 2166136261u;
	std::uint32_t checksum_b = 2166136261u;
	for (std::uint32_t z = 0; z < 16; ++z)
	for (std::uint32_t y = 0; y < 16; ++y)
	for (std::uint32_t x = 0; x < 16; ++x)
	{
		checksum_a = (checksum_a ^ cloud_periodic_hash_cpu(x, y, z, period, seed)) * 16777619u;
		checksum_b = (checksum_b ^ cloud_periodic_hash_cpu(
			x + period, y + period, z + period, period, seed)) * 16777619u;
	}
	assert(checksum_a == checksum_b);
	assert(checksum_a == 0x1f5ff5fau);
	assert(cloud_periodic_hash_cpu(7, 9, 11, period, seed)
		!= cloud_periodic_hash_cpu(7, 9, 11, period, seed + 1));
}

static void test_coverage_and_energy()
{
	for (int shape_index = 0; shape_index <= 20; ++shape_index)
	{
		const float shape = shape_index / 20.0f;
		float previous = cloud_coverage_remap_cpu(shape, 0.0f);
		for (int coverage_index = 1; coverage_index <= 20; ++coverage_index)
		{
			const float current = cloud_coverage_remap_cpu(shape, coverage_index / 20.0f);
			assert(current + 1.0e-6f >= previous);
			previous = current;
		}
	}
	for (float extinction : { 0.0f, 1.0e-8f, 0.001f, 0.1f, 10.0f, 1.0e5f })
	{
		const float integrated = cloud_energy_step_cpu(2.0f, extinction, 100.0f);
		assert(std::isfinite(integrated));
		assert(integrated >= 0.0f);
	}
}

static void test_low_discrepancy_jitter()
{
	std::array<float, 16> phases = {};
	for (std::uint32_t frame = 0; frame < phases.size(); ++frame)
	{
		phases[frame] = cloud_low_discrepancy_jitter_cpu(37.0f, 91.0f, frame, 0u);
		assert(phases[frame] >= 0.0f && phases[frame] < 1.0f);
	}
	assert(std::abs(phases[0]
		- cloud_low_discrepancy_jitter_cpu(37.0f, 91.0f, 16u, 0u)) < 1.0e-6f);
	std::sort(phases.begin(), phases.end());
	for (std::size_t index = 1; index < phases.size(); ++index)
		assert(phases[index] - phases[index - 1] > 0.05f);
	assert(std::abs(
		cloud_low_discrepancy_jitter_cpu(37.0f, 91.0f, 0u, 0u)
		- cloud_low_discrepancy_jitter_cpu(37.0f, 91.0f, 0u, 1u)) > 1.0e-4f);
}


// fp16 has a 10-bit mantissa; this reproduces the rounding a half-float store
// applies so the depth-encoding test measures the real storage error.
static float quantize_half(float value)
{
	if (!std::isfinite(value)) return value;
	if (std::abs(value) > 65504.0f) return std::copysign(INFINITY, value);
	if (value == 0.0f) return value;
	int exponent = 0;
	const float mantissa = std::frexp(std::abs(value), &exponent);
	const float scale = std::ldexp(1.0f, exponent - 11);
	return std::copysign(std::round(std::abs(value) / scale) * scale, value);
}

static void test_march_step_bounds()
{
	// Cumulus defaults: 1800 m base, 3000 m thick, planet radius 6360 km.
	const float ground_radius = 6360000.0f;
	const float thickness = 3000.0f;
	const float inner_radius = ground_radius + 1800.0f;
	const float outer_radius = inner_radius + thickness;
	const float camera_radius = ground_radius + 2.0f;
	const float view_steps = 40.0f;
	const float max_step_scale = 3.0f;
	const float max_march_length = 40000.0f;
	const int max_steps_per_layer = 256;

	const float elevations[] = { 90.0f, 45.0f, 20.0f, 10.0f, 5.0f, 2.0f, 1.0f, 0.5f };
	float zenith_step = 0.0f;
	for (float elevation_deg : elevations)
	{
		const float elevation = elevation_deg * 3.14159265358979323846f / 180.0f;
		const float mu = std::sin(elevation);
		// Ray from just above the ground, travelling outward through the shell.
		const float discriminant =
			camera_radius * camera_radius * (mu * mu - 1.0f) + outer_radius * outer_radius;
		assert(discriminant > 0.0f);
		const float far_distance = -camera_radius * mu + std::sqrt(discriminant);
		const float interval = std::min(far_distance, max_march_length);

		const float step = cloud_march_step_size_cpu(
			interval, thickness, view_steps, max_step_scale);
		if (elevation_deg == 90.0f) zenith_step = step;

		// The whole point: step length must not scale with the chord.
		assert(step <= thickness / view_steps * max_step_scale + 1.0e-3f);
		assert(step > 0.0f);
		// And a fully dense ray must still reach its far boundary.
		const float steps = cloud_march_step_count_cpu(interval, step, 0.75f);
		assert(steps <= (float)max_steps_per_layer);
	}
	// The clamp must stay out of the way where sampling was already adequate.
	// A ground-level camera marches base_altitude + thickness looking up, so
	// the zenith interval is ~4800 m and its unclamped step ~120 m - well under
	// the 225 m ceiling the default scale sets.
	assert(zenith_step > 100.0f && zenith_step < 130.0f);
	assert(zenith_step < thickness / view_steps * max_step_scale);

	// The scale is expressed in units of thickness/view_steps, so it sets the
	// ceiling directly and the clamp only engages once the chord exceeds
	// thickness * scale.
	assert(std::abs(cloud_march_step_size_cpu(1.0e6f, thickness, view_steps, 1.0f)
		- thickness / view_steps) < 1.0e-3f);

	// The old fixed-count sizing is what the clamp replaces: at 1 degree it
	// produced kilometre steps against ~1 km noise features.
	const float grazing_interval = 159000.0f;
	assert(grazing_interval / view_steps > 3000.0f);
	assert(cloud_march_step_size_cpu(
		grazing_interval, thickness, view_steps, max_step_scale) < 300.0f);
}

static void test_depth_storage_range()
{
	// Metres overflow fp16 past ~65 km; kilometres stay finite and keep
	// relative precision, which is what reprojection actually needs.
	assert(!std::isfinite(quantize_half(80000.0f)));
	const float depths[] = { 500.0f, 5000.0f, 26000.0f, 80000.0f, 250000.0f };
	for (float depth : depths)
	{
		const float stored = quantize_half(cloud_depth_encode_cpu(depth));
		assert(std::isfinite(stored));
		const float recovered = cloud_depth_decode_cpu(stored);
		assert(std::isfinite(recovered));
		// fp16 holds ~0.1% relative precision at any magnitude.
		assert(std::abs(recovered - depth) <= depth * 0.001f);
	}
}

int main()
{
	test_shell_intersection_and_sorting();
	test_periodicity_and_seed_determinism();
	test_coverage_and_energy();
	test_low_discrepancy_jitter();
	test_march_step_bounds();
	test_depth_storage_range();
	return 0;
}
