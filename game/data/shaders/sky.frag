#version 450

// Visible background sky. Bruneton's physical LUTs are evaluated directly at
// the current camera position; only the expensive LUT construction is cached.

#include "shader_common.h"
#define BRUNETON_DESCRIPTOR_SET 1
#include "bruneton_parameters.h"

layout(set = 1, binding = 1) uniform sampler2D transmittance_texture;
layout(set = 1, binding = 2) uniform sampler2DArray scattering_texture;
layout(set = 1, binding = 3) uniform sampler2DArray unused_single_mie_texture;

layout(push_constant) uniform SkyCompositePushConstants
{
	vec4 sun_direction_and_cos_radius;
	vec4 sun_tint_and_disc_intensity;
	vec4 planet_center_z_and_sky_intensity;
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
	vec4 world_pos_h = per_frame.inv_view_projection * clip_pos;
	vec3 view_dir = normalize(
		world_pos_h.xyz / world_pos_h.w - per_frame.camera_position.xyz);

	AtmosphereParameters atmosphere = GetAtmosphere();
	vec3 camera = GetAtmosphereCameraPosition(
		atmosphere, per_frame.camera_position.xyz,
		pc.planet_center_z_and_sky_intensity.x);
	vec3 sun_direction = normalize(pc.sun_direction_and_cos_radius.xyz);
	vec3 transmittance;
	vec3 sky_luminance = GetSkyLuminance(
		atmosphere, transmittance_texture, scattering_texture,
		unused_single_mie_texture, camera, view_dir, sun_direction,
		transmittance);
	vec3 sky_color = max(sky_luminance, vec3(0.0))
		* pc.sun_tint_and_disc_intensity.rgb
		* pc.planet_center_z_and_sky_intensity.y;

	float view_sun_cos = dot(view_dir, sun_direction);
	float edge_width = max(fwidth(view_sun_cos), 1.0e-6);
	float disc = smoothstep(
		pc.sun_direction_and_cos_radius.w - edge_width,
		pc.sun_direction_and_cos_radius.w + edge_width,
		view_sun_cos);
	const vec3 SUN_RADIANCE_TO_RGB =
		vec3(98242.786222, 69954.398112, 66475.012354) * 1.0e-5;
	vec3 solar_radiance = atmosphere.solar_irradiance /
		(PI * atmosphere.sun_angular_radius * atmosphere.sun_angular_radius);
	sky_color += disc * transmittance * solar_radiance * SUN_RADIANCE_TO_RGB
		* pc.sun_tint_and_disc_intensity.rgb
		* pc.sun_tint_and_disc_intensity.w;

	out_color = vec4(sky_color, 1.0);
	out_position = vec4(0.0);
	out_normal = vec4(0.0);
	out_roughness_metallic_emissive = vec4(0.0, 0.0, 1.0, 0.0);
}
