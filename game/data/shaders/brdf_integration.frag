#version 450

// Preintegrated split-sum BRDF LUT. R = scale, G = bias.

#include "brdf.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

float radical_inverse_vdc(uint bits)
{
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint index, uint count)
{
	return vec2(float(index) / float(count), radical_inverse_vdc(index));
}

vec3 importance_sample_ggx(vec2 xi, vec3 normal, float perceptual_roughness)
{
	float alpha = perceptual_roughness * perceptual_roughness;
	float alpha_squared = alpha * alpha;
	float phi = 2.0 * M_PI * xi.x;
	float cos_theta = sqrt((1.0 - xi.y) / max(1.0 + (alpha_squared - 1.0) * xi.y, 0.00001));
	float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
	vec3 half_vector = vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);

	vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, normal));
	vec3 bitangent = cross(normal, tangent);
	return normalize(tangent * half_vector.x + bitangent * half_vector.y + normal * half_vector.z);
}

vec2 integrate_brdf(float n_dot_v, float roughness)
{
	vec3 view = vec3(sqrt(max(1.0 - n_dot_v * n_dot_v, 0.0)), 0.0, n_dot_v);
	vec3 normal = vec3(0.0, 0.0, 1.0);
	float scale = 0.0;
	float bias = 0.0;
	const uint sample_count = 1024u;
	for (uint sample_index = 0u; sample_index < sample_count; ++sample_index)
	{
		vec3 half_vector = importance_sample_ggx(hammersley(sample_index, sample_count), normal, roughness);
		vec3 light = normalize(2.0 * dot(view, half_vector) * half_vector - view);
		float n_dot_l = max(light.z, 0.0);
		float n_dot_h = max(half_vector.z, 0.0);
		float v_dot_h = max(dot(view, half_vector), 0.0);
		if (n_dot_l > 0.0)
		{
			float geometry = geometry_smith(n_dot_v, n_dot_l, roughness);
			float visibility = geometry * v_dot_h / max(n_dot_h * n_dot_v, 0.00001);
			float fresnel = pow(1.0 - v_dot_h, 5.0);
			scale += (1.0 - fresnel) * visibility;
			bias += fresnel * visibility;
		}
	}
	return vec2(scale, bias) / float(sample_count);
}

void main()
{
	vec2 integrated = integrate_brdf(max(uv.x, 0.0001), clamp(uv.y, 0.0, 1.0));
	out_color = vec4(integrated, 0.0, 1.0);
}
