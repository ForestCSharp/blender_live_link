#version 450

// GGX-prefilters one probe cubemap into one padded octahedral atlas tile.

#include "octahedral_helpers.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

layout(push_constant) uniform PrefilterParams
{
	int atlas_entry_size;
	float roughness;
	int sample_count;
	int padding0;
};

layout(set = 0, binding = 0) uniform samplerCube cubemap_lighting_tex;

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

void main()
{
	vec2 octahedral_coords = make_padded_atlas_uv(uv, float(atlas_entry_size));
	vec3 reflection_direction = octahedral_decode(octahedral_coords);

	if (roughness <= 0.00001)
	{
		out_color = vec4(texture(cubemap_lighting_tex, reflection_direction).rgb, 1.0);
		return;
	}

	vec3 accumulated = vec3(0.0);
	float weight = 0.0;
	vec3 normal = reflection_direction;
	vec3 view = reflection_direction;
	uint count = uint(max(sample_count, 1));
	for (uint sample_index = 0u; sample_index < count; ++sample_index)
	{
		vec3 half_vector = importance_sample_ggx(hammersley(sample_index, count), normal, roughness);
		vec3 light = normalize(2.0 * dot(view, half_vector) * half_vector - view);
		float n_dot_l = max(dot(normal, light), 0.0);
		if (n_dot_l > 0.0)
		{
			accumulated += texture(cubemap_lighting_tex, light).rgb * n_dot_l;
			weight += n_dot_l;
		}
	}
	out_color = vec4(accumulated / max(weight, 0.00001), 1.0);
}
