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

// Mirrors the step sizing in cloud_raymarch.frag. Step length has to be bounded
// in absolute terms: dividing a shell chord by a fixed step count gives ~90 m
// steps at zenith and ~6 km near the horizon, against noise features on the
// order of 1 km. A scale of 1.0 reproduces the zenith step exactly, since at
// zenith the chord is the layer thickness.
inline float cloud_march_step_size_cpu(
	float interval_length, float layer_thickness, float view_steps, float max_step_scale)
{
	view_steps = std::max(view_steps, 1.0f);
	const float nominal_step = std::max(interval_length, 0.0f) / view_steps;
	const float max_step = std::max(layer_thickness, 0.0f) / view_steps
		* std::max(max_step_scale, 0.0f);
	return std::min(nominal_step, max_step);
}

// How many steps a fully dense march would take across the interval. Compare
// against MAX_STEPS_PER_LAYER: exceeding it truncates the ray before its far
// boundary, which is view-dependent and so shows up as a horizon artifact.
inline float cloud_march_step_count_cpu(
	float interval_length, float step_size, float dense_step_scale)
{
	if (!(step_size > 0.0f)) return 0.0f;
	return std::max(interval_length, 0.0f)
		/ (step_size * std::clamp(dense_step_scale, 0.01f, 1.0f));
}

// Depth rides in an fp16 channel, whose largest finite value is 65504. Storing
// kilometres keeps grazing-angle depths (~100 km) representable; storing metres
// overflows to +Inf and poisons the reprojection UV with NaN.
inline float cloud_depth_encode_cpu(float depth_m) { return depth_m * 0.001f; }
inline float cloud_depth_decode_cpu(float stored) { return stored * 1000.0f; }

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

inline std::uint32_t cloud_reverse_4_bits_cpu(std::uint32_t value)
{
	value &= 15u;
	return ((value & 1u) << 3u) | ((value & 2u) << 1u)
		| ((value & 4u) >> 1u) | ((value & 8u) >> 3u);
}

inline float cloud_fract_cpu(float value)
{
	return value - std::floor(value);
}

inline float cloud_low_discrepancy_jitter_cpu(
	float pixel_x, float pixel_y, std::uint32_t frame_index, std::uint32_t dimension)
{
	const float inner = cloud_fract_cpu(pixel_x * 0.06711056f + pixel_y * 0.00583715f);
	const float spatial = cloud_fract_cpu(52.9829189f * inner);
	const std::uint32_t phase = cloud_reverse_4_bits_cpu(
		(frame_index + dimension * 5u) & 15u);
	return cloud_fract_cpu(spatial + ((float)phase + 0.5f) / 16.0f);
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
