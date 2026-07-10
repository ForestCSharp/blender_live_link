#version 450

#include "shader_common.h"

layout(location = 0) in vec3 in_world_normal;
layout(location = 1) in vec2 in_texcoord;
layout(location = 2) flat in int in_material_index;

layout(location = 0) out vec4 out_color;

// Fallback when the object has no material. Deviation from game/'s magenta
// debug output: grey keeps all pre-material golden screenshots valid.
const vec3 FALLBACK_BASE_COLOR = vec3(0.6, 0.6, 0.6);

void main()
{
	vec3 normal = normalize(in_world_normal);

	// per_frame.sun_direction is the direction the light travels; negate to
	// get the surface-to-light vector
	vec3 to_light = -normalize(per_frame.sun_direction.xyz);
	float n_dot_l = max(dot(normal, to_light), 0.0);
	float sun_term = 0.15 + 0.85 * n_dot_l;

	vec3 base_color = FALLBACK_BASE_COLOR;
	vec3 emission = vec3(0.0);

	if (in_material_index >= 0)
	{
		Material material = material_data_array[in_material_index];

		base_color = material.base_color.rgb;
		if (material.base_color_image_index >= 0)
		{
			base_color = texture(sampler2D(scene_textures[material.base_color_image_index], scene_sampler), in_texcoord).rgb;
		}

		// Forward approximation of game/'s deferred emissive (real lighting
		// consumes this via the G-buffer in Phase 3)
		if (material.emission_strength > 0.0)
		{
			vec3 emission_color = material.emission_color.rgb;
			if (material.emission_color_image_index >= 0)
			{
				emission_color = texture(sampler2D(scene_textures[material.emission_color_image_index], scene_sampler), in_texcoord).rgb;
			}
			emission = emission_color * material.emission_strength;
		}

		// metallic/roughness are stored in the material SSBO but unused
		// until the Phase 3 lighting pass
	}

	vec3 color = base_color * per_frame.sun_color.rgb * sun_term + emission;
	out_color = vec4(color, 1.0);
}
