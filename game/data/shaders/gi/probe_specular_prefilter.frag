#version 450

// GGX-prefilters one probe's relit radiance entry into its specular atlas tile.

#include "shared/octahedral_helpers.h"
#include "shared/sampling.h"

layout(push_constant) uniform PrefilterParams
{
	int atlas_entry_size;
	float roughness;
	int sample_count;
	int probe_index;			// radiance atlas slot
	int radiance_total_size;
	int radiance_entry_size;
};

layout(set = 0, binding = 0) uniform sampler2D radiance_tex;

vec3 sample_radiance(vec3 direction)
{
	return texture(radiance_tex, padded_atlas_uv_from_normal(
		direction, probe_index, radiance_total_size, radiance_entry_size)).rgb;
}

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main()
{
	vec2 octahedral_coords = make_padded_atlas_uv(uv, float(atlas_entry_size));
	vec3 reflection_direction = octahedral_decode(octahedral_coords);

	if (roughness <= 0.00001)
	{
		out_color = vec4(sample_radiance(reflection_direction), 1.0);
		return;
	}

	vec3 accumulated = vec3(0.0);
	float weight = 0.0;
	vec3 normal = reflection_direction;
	vec3 view = reflection_direction;
	uint count = uint(max(sample_count, 1));
	for (uint sample_index = 0u; sample_index < count; ++sample_index)
	{
		vec3 half_vector = sampling_importance_sample_ggx(
			sampling_hammersley(sample_index, count), normal, roughness);
		vec3 light = normalize(2.0 * dot(view, half_vector) * half_vector - view);
		float n_dot_l = max(dot(normal, light), 0.0);
		if (n_dot_l > 0.0)
		{
			accumulated += sample_radiance(light) * n_dot_l;
			weight += n_dot_l;
		}
	}
	out_color = vec4(accumulated / max(weight, 0.00001), 1.0);
}
