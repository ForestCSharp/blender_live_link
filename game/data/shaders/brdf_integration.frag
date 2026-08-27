#version 450

// Preintegrated split-sum BRDF LUT. R = scale, G = bias.

#include "brdf.h"
#include "sampling.h"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

vec2 integrate_brdf(float n_dot_v, float roughness)
{
	vec3 view = vec3(sqrt(max(1.0 - n_dot_v * n_dot_v, 0.0)), 0.0, n_dot_v);
	vec3 normal = vec3(0.0, 0.0, 1.0);
	float scale = 0.0;
	float bias = 0.0;
	const uint sample_count = 1024u;
	for (uint sample_index = 0u; sample_index < sample_count; ++sample_index)
	{
		vec3 half_vector = sampling_importance_sample_ggx(
			sampling_hammersley(sample_index, sample_count), normal, roughness);
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
