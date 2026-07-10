#version 450

// Bakes the atmosphere into a 256x256 octahedral map, re-run only when the
// sun direction changes (port of game/data/shaders/sky_bake.glsl)

#include "octahedral_helpers.h"
#include "sky_atmosphere.h"

layout(push_constant) uniform PushConstants
{
	vec4 sun_dir;	// xyz = direction toward the sun (game/ convention)
} pc;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 out_color;

float gradient_noise(in vec2 noise_uv)
{
	return fract(52.9829189 * fract(dot(noise_uv, vec2(0.06711056, 0.00583715))));
}

vec4 dither_noise()
{
	return vec4(vec3((1.0 / 255.0) * gradient_noise(gl_FragCoord.xy) - (0.5 / 255.0)), 0.0);
}

void main()
{
	const vec3 camera_position = vec3(0, 0, 0);
	const vec2 oct_uv = uv * 2.0 - 1.0;
	const vec3 view_dir = octahedral_decode(oct_uv);
	const float ray_length = INFINITY;
	const vec3 light_dir = normalize(pc.sun_dir.xyz);
	const vec3 light_color = vec3(1, 1, 1);

	vec3 out_transmittance;
	const vec3 sky_color = IntegrateScattering(
		camera_position,
		view_dir,
		ray_length,
		light_dir,
		light_color,
		out_transmittance
	);
	out_color = vec4(sky_color, 1.0) + dither_noise();
}
