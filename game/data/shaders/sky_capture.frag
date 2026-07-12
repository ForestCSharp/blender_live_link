#version 450

// Probe-capture variant of sky.frag: the capture camera changes per face, so
// inv_view_projection + capture position ride in push constants instead of
// the per-frame UBO. Writes the same 4-MRT sky sentinel.

#include "octahedral_helpers.h"

// Set 0: baked octahedral sky (layout B)
layout(set = 0, binding = 0) uniform sampler2D octahedral_sky_tex;

layout(push_constant) uniform PushConstants
{
	mat4 inv_view_projection;
	vec4 capture_position;	// xyz used
} pc;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_position;
layout(location = 2) out vec4 out_normal;
layout(location = 3) out vec4 out_roughness_metallic_emissive;

void main()
{
	vec4 clip_pos = vec4(
		uv.x * 2.0 - 1.0,
		1.0 - uv.y * 2.0,
		0.0,
		1.0
	);

	vec4 world_pos_h = pc.inv_view_projection * clip_pos;
	vec3 view_dir = normalize(world_pos_h.xyz / world_pos_h.w - pc.capture_position.xyz);

	vec2 octahedral_coords = octahedral_encode(view_dir) * 0.5 + 0.5;
	const vec3 sky_color = texture(octahedral_sky_tex, octahedral_coords).rgb;

	out_color = vec4(sky_color, 1.0);
	out_position = vec4(0.0, 0.0, 0.0, 0.0);
	out_normal = vec4(0, 0, 0, 0);
	out_roughness_metallic_emissive = vec4(0, 0, 1, 0);
}
