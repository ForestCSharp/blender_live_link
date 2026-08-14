#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>

#include "render/cloud_math.h"

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

int main()
{
	test_shell_intersection_and_sorting();
	test_periodicity_and_seed_determinism();
	test_coverage_and_energy();
	return 0;
}
