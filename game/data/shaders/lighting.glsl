
// Common Shader Code
#include "shader_common.h"

// BRDF
#include "brdf.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.glslh"

@fs fs

@include_block brdf 

struct PointLight {
	vec4 location;
	vec4 color;

	float power;
};

struct SpotLight {
	vec4 location;
	vec4 color;

	float power;
	float spot_angle_radians;
	float edge_blend;

	vec3 direction;
};

struct SunLight {
	vec4 location;
	vec4 color;

	float power;
	int cast_shadows;

	vec3 direction;
};

layout(binding=0) uniform texture2D color_tex;
layout(binding=1) uniform texture2D position_tex;
layout(binding=2) uniform texture2D normal_tex;
layout(binding=3) uniform texture2D roughness_metallic_emissive_tex;
layout(binding=4) uniform texture2D ssao_tex;

layout(binding=0) uniform sampler tex_sampler;

layout(binding=0) uniform fs_params {
	vec3 view_position;
    int num_point_lights;
	int num_spot_lights;
	int num_sun_lights;
};

layout(binding=0) readonly buffer PointLightsBuffer {
	PointLight point_lights[];
};

layout(binding=1) readonly buffer SpotLightsBuffer {
	SpotLight spot_lights[];
};

layout(binding=2) readonly buffer SunLightsBuffer {
	SunLight sun_lights[];
};

float vec2_length_squared(vec2 v)
{
	return dot(v,v);
}

float dist_squared(vec3 a, vec3 b)
{
    vec3 c = a - b;
    return dot(c,c);
}

float dist(vec3 a, vec3 b)
{
	return sqrt(dist_squared(a,b));
}

float cosine_angle(vec3 a, vec3 b)
{
	return dot(normalize(a), normalize(b));
}

float positive_cosine_angle(vec3 a, vec3 b)
{
	return max(0.0, cosine_angle(a,b));
}

float mix_clamped(float a, float b, float alpha)
{
	return mix(a, b, clamp(alpha, 0.0, 1.0));
}

float remap(float value, float old_min, float old_max, float new_min, float new_max)
{
  return new_min + (value - old_min) * (new_max - new_min) / (old_max - old_min);
}

float remap_clamped(float value, float old_min, float old_max, float new_min, float new_max)
{
	float clamped_value = clamp(value, old_min, old_max);
	return remap(clamped_value, old_min, old_max, new_min, new_max);
}

/* Gradient noise from Jorge Jimenez's presentation: */
/* http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare */
float gradient_noise(in vec2 uv)
{
	return fract(52.9829189 * fract(dot(uv, vec2(0.06711056, 0.00583715))));
}

vec4 dither_noise()
{
	return vec4(vec3((1.0 / 255.0) * gradient_noise(gl_FragCoord.xy) - (0.5 / 255.0)), 0.0);
}

vec3 sample_point_light(
	PointLight in_point_light, 
	vec3 in_view_position,
	vec3 in_surface_position, 
	vec3 in_surface_normal, 
	vec3 in_surface_albedo, 
	float in_surface_roughness, 
	float in_surface_metallic
)
{
	// world location of light
	vec3 light_location = in_point_light.location.xyz;

	// surface normal
	vec3 n = in_surface_normal;

	// surface to light
	vec3 l = normalize(light_location - in_surface_position);

	//surface to eye
	vec3 v = normalize(in_view_position - in_surface_position);

	// point light radiance at surface
	float light_attenuation = in_point_light.power / (4 * M_PI * dist_squared(light_location, in_surface_position));
	vec3 light_radiance = in_point_light.color.xyz * light_attenuation; 

	vec3 f0 = mix(vec3(0.04, 0.04, 0.04), in_surface_albedo, vec3(in_surface_metallic));
	return cook_torrance_brdf(n,l,v, in_surface_albedo, f0, in_surface_roughness, in_surface_metallic, light_radiance);
}

vec3 sample_spot_light(
	SpotLight in_spot_light, 
	vec3 in_view_position,
	vec3 in_surface_position, 
	vec3 in_surface_normal, 
	vec3 in_surface_albedo, 
	float in_surface_roughness, 
	float in_surface_metallic
)
{
	// world location of light
	vec3 light_location = in_spot_light.location.xyz;

	//// surface normal
	vec3 n = in_surface_normal;

	// surface to light
	vec3 l = normalize(light_location - in_surface_position);

	//surface to eye
	vec3 v = normalize(in_view_position - in_surface_position);


	// Spot light cone check
	vec3 light_to_world_pos = normalize(in_surface_position - light_location);
	float surface_cosine_angle = cosine_angle(light_to_world_pos, in_spot_light.direction);
	float outer_cone_cosine_angle = cos(in_spot_light.spot_angle_radians);

	// Check if we're outside spotlight's outer cone
	if (surface_cosine_angle <= outer_cone_cosine_angle)
	{
		return vec3(0,0,0);
	}

	PointLight point_light;
	point_light.location = in_spot_light.location;
	point_light.color = in_spot_light.color;
	point_light.power = in_spot_light.power;
	return sample_point_light(
		point_light, 
		in_view_position, 
		in_surface_position,
	 	in_surface_normal,
		in_surface_albedo,
	 	in_surface_roughness,
		in_surface_metallic
	);
}


vec3 sample_sun_light(
	SunLight in_sun_light, 
	vec3 in_view_position,
	vec3 in_surface_position, 
	vec3 in_surface_normal, 
	vec3 in_surface_albedo, 
	float in_surface_roughness, 
	float in_surface_metallic
)
{
	// world location of light
	vec3 light_location = in_sun_light.location.xyz;

	//// surface normal
	vec3 n = in_surface_normal;

	// surface to light
	vec3 l = -normalize(in_sun_light.direction);

	//surface to eye
	vec3 v = normalize(in_view_position - in_surface_position);

	// Sun light radiance at surface
	float light_attenuation = in_sun_light.power;
	vec3 light_radiance = in_sun_light.color.xyz * light_attenuation;

	vec3 f0 = mix(vec3(0.04, 0.04, 0.04), in_surface_albedo, vec3(in_surface_metallic));
	return cook_torrance_brdf(n,l,v, in_surface_albedo, f0, in_surface_roughness, in_surface_metallic, light_radiance);
}

in vec2 uv;

out vec4 frag_color;

void main()
{
	vec4 final_color = vec4(0);

	vec4 sampled_color	= texture(sampler2D(color_tex, tex_sampler), uv);
	vec4 sampled_normal	= texture(sampler2D(normal_tex, tex_sampler), uv);

	if (sampled_normal == vec4(0))
	{
		final_color = sampled_color;
	}
	else
	{
		vec4 sampled_position	= texture(sampler2D(position_tex, tex_sampler), uv);
		vec4 sampled_roughness_metallic_emissive = texture(sampler2D(roughness_metallic_emissive_tex, tex_sampler), uv);
		float roughness = sampled_roughness_metallic_emissive.r;
		float metallic = sampled_roughness_metallic_emissive.g;
		float emission_strength = sampled_roughness_metallic_emissive.b;

		float ambient_occlusion = texture(sampler2D(ssao_tex, tex_sampler), uv).r;

		// When emission_strength is greater than zero, the emission color lives in color_tex
		if (emission_strength > 0.0)
		{
			final_color = sampled_color * emission_strength;
		}
		else
		{
			for (int i = 0; i < num_point_lights; ++i)
			{
				final_color.xyz += sample_point_light(
					point_lights[i], 
					view_position,
					sampled_position.xyz, 
					sampled_normal.xyz, 
					sampled_color.xyz, 
					roughness, 
					metallic
				);

			}

			for (int i = 0; i < num_spot_lights; ++i)
			{
				final_color.xyz += sample_spot_light(
					spot_lights[i], 
					view_position,
					sampled_position.xyz, 
					sampled_normal.xyz, 
					sampled_color.xyz, 
					roughness, 
					metallic
				);
			}

			for (int i = 0; i < num_sun_lights; ++i)
			{
				final_color.xyz += sample_sun_light(
					sun_lights[i], 
					view_position,
					sampled_position.xyz, 
					sampled_normal.xyz, 
					sampled_color.xyz, 
					roughness, 
					metallic
				);
			}

			// Apply ambient occlusion
			final_color *= ambient_occlusion;
		}
	}

	frag_color = final_color;
}

@end

@program lighting vs fs
