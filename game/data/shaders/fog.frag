#version 450

// Exponential height fog with an optional ceiling and Henyey-Greenstein sun
// in-scatter. The normal scene path fogs the lighting color at the geometry
// depth. When clouds are present, the cloud composite and metadata let the
// two contributions be fogged at their respective depths before recombination.

const float M_PI = 3.14159265358979323846;

#define BRUNETON_PARAMETER_BINDING 3
#include "bruneton_parameters.h"

layout(set = 0, binding = 0) uniform fs_params
{
	vec3 camera_position;
	float fog_base_height;
	vec3 fog_color;
	float density;
	float scale_height;
	float max_distance;
	int ceiling_enabled;
	float ceiling_height;
	float ceiling_fade;
	float ambient_intensity;
	float sun_intensity;
	float anisotropy;
	vec3 sun_direction;
	vec3 sun_color;
	float _sun_color_pad;
	int atmosphere_enabled;
	float atmosphere_planet_center_z;
	float _atmosphere_pad0;
	float _atmosphere_pad1;
	int cloud_enabled;
	float _cloud_pad0;
	float _cloud_pad1;
	float _cloud_pad2;
};

layout(set = 0, binding = 1) uniform sampler2D composite_color_tex;
layout(set = 0, binding = 2) uniform sampler2D geometry_position_tex;
layout(set = 0, binding = 4) uniform sampler2D atmosphere_transmittance_tex;
layout(set = 0, binding = 5) uniform sampler2D background_color_tex;
layout(set = 0, binding = 6) uniform sampler2D cloud_metadata_tex;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

float height_fog_optical_depth(vec3 ray_origin, vec3 ray_dir, float ray_length)
{
	float safe_scale_height = max(scale_height, 0.001);
	float safe_ray_length = max(ray_length, 0.0);
	float base_density = max(density, 0.0) * exp(-(ray_origin.z - fog_base_height) / safe_scale_height);
	float height_slope = ray_dir.z / safe_scale_height;

	if (abs(height_slope) < 0.00001)
	{
		return base_density * safe_ray_length;
	}

	return base_density * (1.0 - exp(-height_slope * safe_ray_length)) / height_slope;
}

float ceiling_density_factor(float height)
{
	if (ceiling_enabled == 0)
	{
		return 1.0;
	}

	float safe_fade = max(ceiling_fade, 0.0);
	if (safe_fade <= 0.0001)
	{
		return height < ceiling_height ? 1.0 : 0.0;
	}

	return 1.0 - smoothstep(ceiling_height - safe_fade, ceiling_height, height);
}

float bounded_height_fog_optical_depth(vec3 ray_origin, vec3 ray_dir, float ray_length)
{
	if (ceiling_enabled == 0)
	{
		return height_fog_optical_depth(ray_origin, ray_dir, ray_length);
	}

	float safe_scale_height = max(scale_height, 0.001);
	float safe_ray_length = max(ray_length, 0.0);
	const int sample_count = 16;
	float step_size = safe_ray_length / float(sample_count);
	float optical_depth = 0.0;

	for (int i = 0; i < sample_count; ++i)
	{
		float ray_time = (float(i) + 0.5) * step_size;
		vec3 sample_position = ray_origin + ray_dir * ray_time;
		float sample_density = max(density, 0.0) * exp(-(sample_position.z - fog_base_height) / safe_scale_height);
		optical_depth += sample_density * ceiling_density_factor(sample_position.z) * step_size;
	}

	return optical_depth;
}

float henyey_greenstein_phase(float cos_theta, float g)
{
	float clamped_g = clamp(g, -0.95, 0.95);
	float g2 = clamped_g * clamped_g;
	float denominator = max(1.0 + g2 - 2.0 * clamped_g * cos_theta, 0.0001);
	return (1.0 - g2) / (4.0 * M_PI * denominator * sqrt(denominator));
}

struct FogResult
{
	float transmittance;
	vec3 inscatter;
};

FogResult evaluate_fog(vec3 world_position)
{
	FogResult result = FogResult(1.0, vec3(0.0));
	vec3 camera_to_pixel = world_position - camera_position;
	float pixel_distance = length(camera_to_pixel);
	if (pixel_distance <= 0.00001)
	{
		return result;
	}

	vec3 ray_dir = camera_to_pixel / pixel_distance;
	float fog_distance = min(pixel_distance, max_distance);
	float optical_depth = max(
		bounded_height_fog_optical_depth(camera_position, ray_dir, fog_distance), 0.0);
	result.transmittance = exp(-min(optical_depth, 80.0));
	float fog_amount = 1.0 - result.transmittance;

	float sun_phase = henyey_greenstein_phase(
		dot(ray_dir, -normalize(sun_direction)), anisotropy) * 4.0 * M_PI;
	vec3 ambient_inscatter = fog_color * ambient_intensity;
	vec3 attenuated_sun_color = sun_color;
	if (atmosphere_enabled != 0)
	{
		AtmosphereParameters atmosphere = GetAtmosphere();
		vec3 atmosphere_camera = GetAtmosphereCameraPosition(
			atmosphere, camera_position, atmosphere_planet_center_z);
		attenuated_sun_color *= GetAtmosphereSunTransmittance(
			atmosphere, atmosphere_transmittance_tex, atmosphere_camera,
			-normalize(sun_direction));
	}
	vec3 sun_inscatter = SanitizeSceneColor(
		SanitizeSceneColor(attenuated_sun_color)
		* fog_color * sun_intensity * sun_phase);
	result.inscatter = ambient_inscatter + sun_inscatter;
	return result;
}

vec3 apply_fog(vec3 scene_color, FogResult fog)
{
	return scene_color * fog.transmittance
		+ fog.inscatter * (1.0 - fog.transmittance);
}

vec4 sample_nearest(sampler2D image, vec2 sample_uv)
{
	ivec2 size = textureSize(image, 0);
	ivec2 pixel = clamp(
		ivec2(sample_uv * vec2(size)), ivec2(0), size - ivec2(1));
	return texelFetch(image, pixel, 0);
}

void main()
{
	vec4 composite_color = texture(composite_color_tex, uv);
	vec4 background_color = texture(background_color_tex, uv);
	vec4 geometry_position = sample_nearest(geometry_position_tex, uv);

	if (density <= 0.0 || max_distance <= 0.0)
	{
		frag_color = composite_color;
		return;
	}

	if (cloud_enabled == 0)
	{
		if (geometry_position.a == 0.0)
		{
			frag_color = composite_color;
			return;
		}
		frag_color = vec4(
			apply_fog(composite_color.rgb, evaluate_fog(geometry_position.xyz)),
			composite_color.a);
		return;
	}

	vec4 cloud_metadata = sample_nearest(cloud_metadata_tex, uv);
	float cloud_opacity = clamp(cloud_metadata.w, 0.0, 1.0);
	if (cloud_opacity <= 0.00001)
	{
		if (geometry_position.a == 0.0)
		{
			frag_color = composite_color;
			return;
		}
		frag_color = vec4(
			apply_fog(composite_color.rgb, evaluate_fog(geometry_position.xyz)),
			composite_color.a);
		return;
	}

	float cloud_transmittance = 1.0 - cloud_opacity;
	vec3 cloud_scattering = max(
		composite_color.rgb - cloud_transmittance * background_color.rgb,
		vec3(0.0));
	vec3 fogged_cloud = apply_fog(
		cloud_scattering, evaluate_fog(cloud_metadata.xyz));
	vec3 fogged_background = background_color.rgb;
	if (geometry_position.a != 0.0)
	{
		fogged_background = apply_fog(
			background_color.rgb, evaluate_fog(geometry_position.xyz));
	}
	frag_color = vec4(
		SanitizeSceneColor(fogged_cloud + cloud_transmittance * fogged_background),
		composite_color.a);
}
