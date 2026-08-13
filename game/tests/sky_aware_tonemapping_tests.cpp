#include <array>
#include <cassert>
#include <cmath>

static float smoothstep(float edge0, float edge1, float value)
{
	const float t = std::fmax(0.0f, std::fmin(1.0f, (value - edge0) / (edge1 - edge0)));
	return t * t * (3.0f - 2.0f * t);
}

static float geometry_local_strength(const std::array<bool, 9>& mask)
{
	if (!mask[4]) return 0.0f;
	float count = 0.0f;
	for (bool geometry : mask) count += geometry ? 1.0f : 0.0f;
	return smoothstep(2.0f / 3.0f, 1.0f, count / 9.0f);
}

struct ProxyResult
{
	float value;
	float coverage;
};

static ProxyResult binomial_geometry_proxy(
	const std::array<float, 16>& values,
	const std::array<bool, 16>& mask)
{
	constexpr float kernel[4] = { 1.0f, 3.0f, 3.0f, 1.0f };
	float weighted_value = 0.0f;
	float geometry_weight = 0.0f;
	for (int y = 0; y < 4; ++y)
	{
		for (int x = 0; x < 4; ++x)
		{
			const int index = y * 4 + x;
			const float weight = kernel[x] * kernel[y];
			if (!mask[index]) continue;
			weighted_value += values[index] * weight;
			geometry_weight += weight;
		}
	}
	return {
		geometry_weight > 1.0e-5f ? weighted_value / geometry_weight : 0.0f,
		geometry_weight / 64.0f,
	};
}

static float bounded_local_multiplier(
	float requested,
	float local_strength,
	float shadow_recovery,
	float highlight_recovery)
{
	const float minimum = std::exp2(-std::fmax(highlight_recovery, 0.0f));
	const float maximum = std::exp2(std::fmax(shadow_recovery, 0.0f));
	const float bounded = std::fmax(minimum, std::fmin(maximum, requested));
	return 1.0f + (bounded - 1.0f) * local_strength;
}

int main()
{
	const std::array<float, 16> bright_sky = {
		10000, 10000, 10000, 10000,
		10000, 10000, 10000, 10000,
		10000, 10000, 10000, 10000,
		10000, 10000, 10000, 10000,
	};
	std::array<bool, 16> mask = {};
	ProxyResult proxy = binomial_geometry_proxy(bright_sky, mask);
	assert(proxy.coverage == 0.0f);
	assert(proxy.value == 0.0f);

	mask.fill(true);
	proxy = binomial_geometry_proxy(bright_sky, mask);
	assert(std::abs(proxy.coverage - 1.0f) < 1.0e-6f);
	assert(std::abs(proxy.value - 10000.0f) < 1.0e-3f);

	// A horizontal half-geometry/half-sky footprint carries half coverage but
	// the sky luminance cannot leak into the normalized geometry value.
	std::array<float, 16> silhouette_values = {
		0.1f, 0.1f, 0.1f, 0.1f,
		0.1f, 0.1f, 0.1f, 0.1f,
		10000, 10000, 10000, 10000,
		10000, 10000, 10000, 10000,
	};
	mask = {
		true, true, true, true,
		true, true, true, true,
		false, false, false, false,
		false, false, false, false,
	};
	proxy = binomial_geometry_proxy(silhouette_values, mask);
	assert(std::abs(proxy.coverage - 0.5f) < 1.0e-6f);
	assert(std::abs(proxy.value - 0.1f) < 1.0e-6f);

	std::array<bool, 9> neighborhood = {};
	assert(geometry_local_strength(neighborhood) == 0.0f); // all sky
	neighborhood.fill(true);
	assert(geometry_local_strength(neighborhood) == 1.0f); // geometry interior
	neighborhood = {
		true, true, false,
		true, true, false,
		true, true, false,
	};
	assert(geometry_local_strength(neighborhood) == 0.0f); // straight silhouette
	neighborhood = {
		false, true, false,
		false, true, false,
		false, true, false,
	};
	assert(geometry_local_strength(neighborhood) == 0.0f); // thin geometry

	assert(bounded_local_multiplier(8.0f, 1.0f, 1.5f, 2.0f)
		<= std::exp2(1.5f));
	assert(bounded_local_multiplier(0.01f, 1.0f, 1.5f, 2.0f)
		>= std::exp2(-2.0f));
	assert(bounded_local_multiplier(8.0f, 0.0f, 1.5f, 2.0f) == 1.0f); // sky/global
	return 0;
}
