#version 450

#include "shader_common.h"
#include "cloud_common.h"
#define BRUNETON_DESCRIPTOR_SET 2
#include "bruneton_parameters.h"

layout(set = 1, binding = 1) uniform sampler2D lit_scene_tex;
layout(set = 1, binding = 2) uniform sampler2D position_tex;
layout(set = 1, binding = 3) uniform sampler2D cloud_tex;
layout(set = 1, binding = 4) uniform sampler2D cloud_depth_tex;
layout(set = 2, binding = 1) uniform sampler2D atmosphere_transmittance_tex;
layout(set = 2, binding = 2) uniform sampler2DArray atmosphere_scattering_tex;
layout(set = 2, binding = 3) uniform sampler2DArray atmosphere_single_mie_tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_position;
layout(location = 2) out vec4 out_reactive;
layout(location = 3) out vec4 out_fog_metadata;

void sample_cloud_bilateral(vec4 geometry_position, out vec4 cloud_value, out vec4 depth_value)
{
	// At the default 1.0x cloud scale this is a resolve, not an upscale. The
	// ray march already clips against scene depth, so avoid eight redundant
	// texture reads and preserve the exact temporally resolved pixel.
	if (all(equal(textureSize(cloud_tex, 0), textureSize(lit_scene_tex, 0))))
	{
		cloud_value = texture(cloud_tex, uv);
		depth_value = texture(cloud_depth_tex, uv);
		return;
	}
	vec2 source_size = vec2(textureSize(cloud_tex, 0));
	vec2 source_pixel = uv * source_size - 0.5;
	vec2 source_base = floor(source_pixel);
	vec2 fraction = fract(source_pixel);
	float geometry_distance = geometry_position.w > 0.0
		? length(geometry_position.xyz - per_frame.camera_position.xyz) : 1.0e30;
	float reference_depth = texture(cloud_depth_tex, uv).x;
	cloud_value = vec4(0.0);
	depth_value = vec4(0.0);
	float weight_sum = 0.0;
	for (int y = 0; y < 2; ++y)
	for (int x = 0; x < 2; ++x)
	{
		vec2 corner = vec2(x, y);
		vec2 sample_uv = (source_base + corner + 0.5) / source_size;
		vec4 sample_cloud = texture(cloud_tex, sample_uv);
		vec4 sample_depth = texture(cloud_depth_tex, sample_uv);
		float spatial_weight = mix(1.0 - fraction.x, fraction.x, float(x))
			* mix(1.0 - fraction.y, fraction.y, float(y));
		float foreground_weight = sample_depth.x <= 0.0
			|| sample_depth.x < geometry_distance + 2.0 ? 1.0 : 0.0;
		float depth_weight = reference_depth > 0.0 && sample_depth.x > 0.0
			? exp(-abs(sample_depth.x - reference_depth) / max(reference_depth * 0.08, 20.0))
			: 1.0;
		float weight = spatial_weight * foreground_weight * depth_weight;
		cloud_value += sample_cloud * weight;
		depth_value += sample_depth * weight;
		weight_sum += weight;
	}
	if (weight_sum > 1.0e-5)
	{
		cloud_value /= weight_sum;
		depth_value /= weight_sum;
	}
	else
	{
		cloud_value = vec4(0.0, 0.0, 0.0, 1.0);
		depth_value = vec4(0.0);
	}
}

void main()
{
	vec4 background = texture(lit_scene_tex, uv);
	ivec2 position_size = textureSize(position_tex, 0);
	ivec2 position_pixel = clamp(
		ivec2(uv * vec2(position_size)), ivec2(0), position_size - ivec2(1));
	vec4 geometry_position = texelFetch(position_tex, position_pixel, 0);
	vec4 cloud_value;
	vec4 depth_value;
	sample_cloud_bilateral(geometry_position, cloud_value, depth_value);
	if (depth_value.x <= 0.0 || cloud_value.a >= 0.9995)
	{
		out_color = background;
		out_position = geometry_position;
		out_reactive = vec4(0.0);
		out_fog_metadata = vec4(0.0);
		return;
	}

	vec4 clip = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
	vec4 world_h = per_frame.inv_view_projection * clip;
	vec3 ray_direction = normalize(world_h.xyz / world_h.w - per_frame.camera_position.xyz);
	vec3 cloud_world_position = per_frame.camera_position.xyz + ray_direction * depth_value.x;
	vec3 cloud_scattering = cloud_value.rgb;

	AtmosphereParameters atmosphere = GetAtmosphere();
	vec3 atmosphere_camera = GetAtmosphereCameraPosition(
		atmosphere, per_frame.camera_position.xyz, cloud.planet_center_time.x);
	vec3 atmosphere_point = GetAtmosphereCameraPosition(
		atmosphere, cloud_world_position, cloud.planet_center_time.x);
	vec3 aerial_transmittance;
	vec3 aerial_scattering = GetSkyRadianceToPoint(
		atmosphere, atmosphere_transmittance_tex, atmosphere_scattering_tex,
		atmosphere_single_mie_tex, atmosphere_camera, atmosphere_point, 0.0,
		normalize(cloud.sun_direction_layer_count.xyz), aerial_transmittance)
		* BRUNETON_SKY_RADIANCE_TO_LUMINANCE
		* BRUNETON_SCENE_PHOTOMETRIC_SCALE
		* cloud.sun_color_history.rgb;
	// The scene already contains its own atmospheric path. Only replace that
	// path in proportion to cloud opacity; otherwise a nearly transparent
	// density sample receives a full cloud-depth aerial term and becomes a
	// visible gray fringe.
	float cloud_opacity = clamp(1.0 - cloud_value.a, 0.0, 1.0);
	cloud_scattering = cloud_scattering * aerial_transmittance
		+ aerial_scattering * cloud_opacity;

	vec3 composited = cloud_scattering + cloud_value.a * background.rgb;
	out_color = vec4(SanitizeSceneColor(composited), 1.0);
	// The position target is also consumed by fog, DoF, and TAA. Preserve an
	// opaque surface as the authoritative depth at mixed cloud/geometry pixels;
	// replacing it with cloud depth defeats silhouette rejection in DoF.
	// w = 2 tags a cloud-only atmospheric pixel, while normal geometry remains 1.
	out_position = geometry_position.w > 0.5
		? geometry_position : vec4(cloud_world_position, 2.0);
	out_reactive = vec4(clamp(1.0 - cloud_value.a, 0.0, 1.0), 0.0, 0.0, 1.0);
	out_fog_metadata = vec4(cloud_world_position, cloud_opacity);
}
