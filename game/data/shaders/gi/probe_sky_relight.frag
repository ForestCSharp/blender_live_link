#version 450

// GI relight R1: sky radiance for one probe radiance texel, evaluated from the
// physical LUTs at the probe's position in the texel's octahedral direction.
// Texels where the geometry cache recorded a surface hit get 0; the lighting
// draw that follows (lighting.frag, probe_relight) overwrites exactly those.
// As in sky_capture.frag, the analytic solar disc is excluded (direct light
// already carries the sun).

#include "sky/bruneton_parameters.h"
#include "gi/gi_helpers.h"
#include "shared/octahedral_helpers.h"

layout(set = 0, binding = 1) uniform sampler2D transmittance_texture;
layout(set = 0, binding = 2) uniform sampler2DArray scattering_texture;
layout(set = 0, binding = 3) uniform sampler2DArray unused_single_mie_texture;

layout(set = 1, binding = 0) uniform sampler2D cache_hit_tex;
layout(set = 1, binding = 1, std430) readonly buffer ProbeBlock { GI_Probe probes[]; };

layout(push_constant) uniform PushConstants
{
	vec4 sun_direction;
	vec4 light_color_and_sky_intensity;	// w = 0 disables the sky
	vec4 planet_center_z;
	ivec4 atlas;						// x = total size, y = entry size
} pc;

layout(location = 0) out vec4 out_color;

void main()
{
	const ivec2 texel = ivec2(gl_FragCoord.xy);
	if (texelFetch(cache_hit_tex, texel, 0).x > 0.0 || pc.light_color_and_sky_intensity.w <= 0.0)
	{
		out_color = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}

	const int entry_size = pc.atlas.y;
	const ivec2 slot = texel / entry_size;
	const int probe_index = slot.y * (pc.atlas.x / entry_size) + slot.x;
	const vec2 entry_uv = (vec2(texel - slot * entry_size) + 0.5) / float(entry_size);
	const vec3 view_dir = octahedral_decode(make_padded_atlas_uv(entry_uv, float(entry_size)));

	AtmosphereParameters atmosphere = GetAtmosphere();
	vec3 camera = GetAtmosphereCameraPosition(
		atmosphere, probes[probe_index].position.xyz, pc.planet_center_z.x);
	vec3 transmittance;
	vec3 luminance = GetSkyLuminance(
		atmosphere, transmittance_texture, scattering_texture,
		unused_single_mie_texture, camera, view_dir,
		normalize(pc.sun_direction.xyz), transmittance);
	vec3 sky_color = max(luminance, vec3(0.0))
		* BRUNETON_SCENE_PHOTOMETRIC_SCALE
		* pc.light_color_and_sky_intensity.rgb
		* pc.light_color_and_sky_intensity.w;
	out_color = vec4(SanitizeSceneColor(sky_color), 1.0);
}
