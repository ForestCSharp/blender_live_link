#version 450

// Probe-capture sky evaluates the physical LUTs at each probe position. The
// analytic solar disc remains excluded to avoid double counting direct light.

#include "bruneton_parameters.h"

layout(set = 0, binding = 1) uniform sampler2D transmittance_texture;
layout(set = 0, binding = 2) uniform sampler2DArray scattering_texture;
layout(set = 0, binding = 3) uniform sampler2DArray unused_single_mie_texture;

layout(push_constant) uniform PushConstants
{
	mat4 inv_view_projection;
	vec4 capture_position;
	vec4 sun_direction;
	vec4 light_color_and_sky_intensity;
	vec4 planet_center_z;
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
		1.0);
	vec4 world_pos_h = pc.inv_view_projection * clip_pos;
	vec3 view_dir = normalize(
		world_pos_h.xyz / world_pos_h.w - pc.capture_position.xyz);

	AtmosphereParameters atmosphere = GetAtmosphere();
	vec3 camera = GetAtmosphereCameraPosition(
		atmosphere, pc.capture_position.xyz, pc.planet_center_z.x);
	vec3 transmittance;
	vec3 luminance = GetSkyLuminance(
		atmosphere, transmittance_texture, scattering_texture,
		unused_single_mie_texture, camera, view_dir,
		normalize(pc.sun_direction.xyz), transmittance);
	vec3 sky_color = max(luminance, vec3(0.0))
		* pc.light_color_and_sky_intensity.rgb
		* pc.light_color_and_sky_intensity.w;

	out_color = vec4(sky_color, 1.0);
	out_position = vec4(0.0);
	out_normal = vec4(0.0);
	out_roughness_metallic_emissive = vec4(0.0, 0.0, 1.0, 0.0);
}
