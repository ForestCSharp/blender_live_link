
// Common Shader Code
#include "shader_common.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.glslh"

@fs fs

struct PointLight {
	vec4 location;
	vec4 color;

	float power;
	PADDING(3);
};

struct SpotLight {
	vec4 location;
	vec4 color;

	float power;
	float spot_angle_radians;
	float edge_blend;
	PADDING(1);

	vec3 direction;
	PADDING(1);
};

struct SunLight {
	vec4 location;
	vec4 color;

	float power;
	int cast_shadows;
	PADDING(2);

	vec3 direction;
	PADDING(1);
};

layout(binding=0) uniform texture2D color_tex;
layout(binding=1) uniform texture2D position_tex;
layout(binding=2) uniform texture2D normal_tex;
layout(binding=3) uniform texture2D ssao_tex;

layout(binding=0) uniform sampler tex_sampler;

layout(binding=0) uniform fs_params {
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

vec4 sample_point_light(PointLight in_point_light, vec3 in_world_position, vec3 in_world_normal, vec4 in_color)
{
	vec3 light_location = in_point_light.location.xyz;
	vec3 world_pos_to_light = normalize(light_location - in_world_position);

	const float surface_angle = positive_cosine_angle(world_pos_to_light, in_world_normal);
	const float numerator = in_point_light.power * surface_angle;
	const float denominator = 4 * M_PI * dist_squared(light_location, in_world_position);
	const float lighting_factor = numerator / denominator;
	return in_color * in_point_light.color * lighting_factor;
}

vec4 sample_spot_light(SpotLight in_spot_light, vec3 in_world_position, vec3 in_world_normal, vec4 in_color)
{
	vec3 spot_light_location = in_spot_light.location.xyz;
	vec3 light_to_world_pos = normalize(in_world_position - spot_light_location);

	float surface_cosine_angle = cosine_angle(light_to_world_pos, in_spot_light.direction);
	float outer_cone_cosine_angle = cos(in_spot_light.spot_angle_radians);

	// Check if we're inside spotlight's outer cone
	if (surface_cosine_angle > outer_cone_cosine_angle)
	{
		const float outer_cone_angle	= in_spot_light.spot_angle_radians;
		const float inner_cone_angle 	= mix_clamped(0.0, outer_cone_angle, 1.0 - in_spot_light.edge_blend);
		const float spot_attenuation 	= in_spot_light.edge_blend > 0.0 
										? 1.0 - remap_clamped(acos(surface_cosine_angle), inner_cone_angle, outer_cone_angle, 0.0, 1.0) 
										: 1.0;

		// Create a point light based on spot-lights location, color, power and sample that
		PointLight point_light;
		point_light.location = in_spot_light.location;
		point_light.color = in_spot_light.color;
		point_light.power = in_spot_light.power;
		return sample_point_light(point_light, in_world_position, in_world_normal, in_color) * spot_attenuation;		
	}

	// Outside spotlight cone, return black
	return vec4(0,0,0,1);
}

vec4 sample_sun_light(SunLight in_sun_light, vec3 in_world_normal, vec4 in_color)
{	
	const vec3 light_direction = -normalize(in_sun_light.direction);
	const float surface_angle = positive_cosine_angle(light_direction, in_world_normal);
	const float numerator = in_sun_light.power * surface_angle;
	return in_color * numerator;
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
		float ambient_occlusion = texture(sampler2D(ssao_tex, tex_sampler), uv).r;

		for (int i = 0; i < num_point_lights; ++i)
		{
			final_color += sample_point_light(point_lights[i], sampled_position.xyz, sampled_normal.xyz, sampled_color);
		}

		for (int i = 0; i < num_spot_lights; ++i)
		{
			final_color += sample_spot_light(spot_lights[i], sampled_position.xyz, sampled_normal.xyz, sampled_color);
		}

		for (int i = 0; i < num_sun_lights; ++i)
		{
			final_color += sample_sun_light(sun_lights[i], sampled_normal.xyz, sampled_color);
		}

		const vec4 ambient = vec4(1,1,1,0) * 0.15;
		final_color += ambient;

		// Apply ambient occlusion
		final_color *= ambient_occlusion;
	}

	frag_color = final_color;
}

@end

@program lighting vs fs
