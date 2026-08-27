#version 450

// GGX-prefilters one probe cubemap into one padded octahedral atlas tile.

#include "octahedral_helpers.h"
#include "sampling.h"

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
		vec3 half_vector = sampling_importance_sample_ggx(
			sampling_hammersley(sample_index, count), normal, roughness);
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
