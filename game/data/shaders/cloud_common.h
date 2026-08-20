#ifndef CLOUD_COMMON_INCLUDED
#define CLOUD_COMMON_INCLUDED

const int MAX_CLOUD_LAYERS = 4;
const float CLOUD_PI = 3.14159265358979323846;

struct CloudLayerGpu
{
	vec4 altitude_thickness_coverage_density;
	vec4 scales_erosion_anvil;
	vec4 wind_phase;
	vec4 ambient_multi_profile_seed;
};

layout(set = 1, binding = 0, std140) uniform CloudParamsBlock
{
	mat4 previous_view_projection;
	vec4 planet_center_time;
	vec4 wind_weather;
	vec4 sun_direction_layer_count;
	vec4 sun_color_history;
	vec4 shadow_extent_misc;
	vec4 march_quality;
	vec4 temporal_quality;
	CloudLayerGpu layers[MAX_CLOUD_LAYERS];
} cloud;

uint cloud_reverse_4_bits(uint value)
{
	value &= 15u;
	return ((value & 1u) << 3u) | ((value & 2u) << 1u)
		| ((value & 4u) >> 1u) | ((value & 8u) >> 3u);
}

float cloud_low_discrepancy_jitter(vec2 pixel, uint frame_index, uint dimension)
{
	float spatial = fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
	uint phase = cloud_reverse_4_bits((frame_index + dimension * 5u) & 15u);
	return fract(spatial + (float(phase) + 0.5) / 16.0);
}

float cloud_remap(float value, float old_min, float old_max, float new_min, float new_max)
{
	return new_min + (clamp(value, old_min, old_max) - old_min)
		/ max(old_max - old_min, 1.0e-5) * (new_max - new_min);
}

float cloud_hg(float cosine_angle, float eccentricity)
{
	float g2 = eccentricity * eccentricity;
	return (1.0 - g2) /
		(max(4.0 * CLOUD_PI * pow(max(1.0 + g2 - 2.0 * eccentricity * cosine_angle, 1.0e-4), 1.5), 1.0e-4));
}

float cloud_height_profile(float height_fraction, int profile, float anvil)
{
	float bottom = smoothstep(0.0, profile == 0 ? 0.08 : 0.16, height_fraction);
	float top_start = profile == 3 ? 0.45 : (profile == 0 ? 0.55 : 0.68);
	float top = 1.0 - smoothstep(top_start, 1.0, height_fraction);
	float result = bottom * top;
	if (profile == 2)
	{
		float anvil_shape = smoothstep(0.55, 0.88, height_fraction)
			* (1.0 - smoothstep(0.92, 1.0, height_fraction));
		result = max(result, anvil * anvil_shape);
	}
	return clamp(result, 0.0, 1.0);
}

vec2 cloud_sphere_interval(vec3 origin, vec3 direction, vec3 center, float radius)
{
	vec3 offset = origin - center;
	float b = dot(offset, direction);
	float c = dot(offset, offset) - radius * radius;
	float d = b * b - c;
	if (d < 0.0) return vec2(1.0, -1.0);
	float root = sqrt(max(d, 0.0));
	return vec2(-b - root, -b + root);
}

#endif
