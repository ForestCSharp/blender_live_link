#version 450

// Visible sky composite: samples the baked octahedral map and writes all 4
// G-buffer attachments with the invalid-geometry sentinel so lighting passes
// the sky color straight through (port of game/data/shaders/sky_pass.glsl)

#include "shader_common.h"
#include "octahedral_helpers.h"

// Set 1: baked octahedral sky (layout B)
layout(set = 1, binding = 0) uniform sampler2D octahedral_sky_tex;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_position;
layout(location = 2) out vec4 out_normal;
layout(location = 3) out vec4 out_roughness_metallic_emissive;

void main()
{
	// UV -> NDC (Y remapped for top-down UVs); z = far (reverse-Z: 0)
	vec4 clip_pos = vec4(
		uv.x * 2.0 - 1.0,
		1.0 - uv.y * 2.0,
		0.0,
		1.0
	);

	vec4 world_pos_h = per_frame.inv_view_projection * clip_pos;
	vec3 view_dir = normalize(world_pos_h.xyz / world_pos_h.w - per_frame.camera_position.xyz);

	vec2 octahedral_coords = octahedral_encode(view_dir) * 0.5 + 0.5;
	const vec3 sky_color = texture(octahedral_sky_tex, octahedral_coords).rgb;

	out_color = vec4(sky_color, 1.0);
	// Mark sky/background as invalid geometry
	out_position = vec4(0.0, 0.0, 0.0, 0.0);
	out_normal = vec4(0, 0, 0, 0);
	out_roughness_metallic_emissive = vec4(0, 0, 1, 0);
}
