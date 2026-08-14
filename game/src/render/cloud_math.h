#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

struct CloudRayInterval
{
	float near_distance;
	float far_distance;
	bool valid;
};

inline CloudRayInterval cloud_sphere_interval_cpu(
	float ox, float oy, float oz, float dx, float dy, float dz,
	float cx, float cy, float cz, float radius)
{
	const float rx = ox - cx;
	const float ry = oy - cy;
	const float rz = oz - cz;
	const float b = rx * dx + ry * dy + rz * dz;
	const float c = rx * rx + ry * ry + rz * rz - radius * radius;
	const float discriminant = b * b - c;
	if (!(discriminant >= 0.0f) || !std::isfinite(discriminant))
		return { 1.0f, -1.0f, false };
	const float root = std::sqrt(discriminant);
	return { -b - root, -b + root, true };
}

inline float cloud_coverage_remap_cpu(float shape, float coverage)
{
	coverage = std::clamp(coverage, 0.0f, 1.0f);
	const float threshold = 1.0f - coverage;
	return std::clamp((shape - threshold) / std::max(coverage, 1.0e-5f), 0.0f, 1.0f);
}

inline float cloud_energy_step_cpu(float source, float extinction, float step_length)
{
	if (!(source >= 0.0f) || !(extinction >= 0.0f) || !(step_length >= 0.0f))
		return 0.0f;
	if (extinction < 1.0e-6f)
		return source * step_length;
	const float result = source * (1.0f - std::exp(-extinction * step_length)) / extinction;
	return std::isfinite(result) ? result : 0.0f;
}

inline std::uint32_t cloud_periodic_hash_cpu(
	std::uint32_t x, std::uint32_t y, std::uint32_t z,
	std::uint32_t period, std::uint32_t seed)
{
	period = std::max(period, 1u);
	x %= period;
	y %= period;
	z %= period;
	std::uint32_t h = seed ^ (x * 0x9e3779b9u) ^ (y * 0x85ebca6bu) ^ (z * 0xc2b2ae35u);
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	h *= 0x846ca68bu;
	return h ^ (h >> 16);
}
