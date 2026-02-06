
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

layout(binding=0) uniform sampler tex_sampler;

layout(binding=0) uniform fs_params {
	vec3 view_position;
    int num_point_lights;
	int num_spot_lights;
	int num_sun_lights;
	int ssao_enable;
	int gi_enable;
};


layout(binding=0) uniform texture2D color_tex;
layout(binding=1) uniform texture2D position_tex;
layout(binding=2) uniform texture2D normal_tex;
layout(binding=3) uniform texture2D roughness_metallic_emissive_tex;
layout(binding=4) uniform texture2D ssao_tex;

layout(binding=5) readonly buffer PointLightsBuffer {
	PointLight point_lights[];
};

layout(binding=6) readonly buffer SpotLightsBuffer {
	SpotLight spot_lights[];
};

layout(binding=7) readonly buffer SunLightsBuffer {
	SunLight sun_lights[];
};

layout(binding=8) uniform textureCube cubemap_tex;

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

		float ambient_occlusion = (ssao_enable != 0) ? texture(sampler2D(ssao_tex, tex_sampler), uv).r : 1.0;

		// When emission_strength is greater than zero, the emission color lives in color_tex
		if (emission_strength > 0.0)
		{
			final_color = sampled_color * emission_strength;
		}
		else
		{
			const vec3 position = sampled_position.xyz;
			const vec3 normal = sampled_normal.xyz;
			const vec3 color = sampled_color.xyz;

			for (int i = 0; i < num_point_lights; ++i)
			{
				final_color.xyz += sample_point_light(
					point_lights[i], 
					view_position,
					position, 
					normal, 
					color, 
					roughness, 
					metallic
				);

			}

			for (int i = 0; i < num_spot_lights; ++i)
			{
				final_color.xyz += sample_spot_light(
					spot_lights[i], 
					view_position,
					position, 
					normal, 
					color, 
					roughness, 
					metallic
				);
			}

			for (int i = 0; i < num_sun_lights; ++i)
			{
				final_color.xyz += sample_sun_light(
					sun_lights[i], 
					view_position,
					position, 
					normal, 
					color, 
					roughness, 
					metallic
				);
			}

			//TODO: uniform var to enable/disable this
			//TODO: sample cubemap
			//if (gi_enable != 0)
			//{
			//	vec4 cubemap_color = texture(samplerCube(cubemap_tex,tex_sampler), sampled_normal.xyz);
			//	final_color.xyz += cubemap_color.xyz * 0.25f;	
			//}
			if (gi_enable != 0)
			{
				// 1. Setup Standard PBR vectors
				vec3 N = normal;
				vec3 V = normalize(view_position - position);
				vec3 R = reflect(-V, N); 
				
				// clamp NdotV to avoid artifacts at edges
				float NdotV = max(dot(N, V), 0.0);

				// 2. Calculate F0 (Base Reflectivity)
				// 0.04 is the standard base reflectivity for non-metals (dielectrics)
				vec3 F0 = vec3(0.04); 
				F0 = mix(F0, sampled_color.xyz, metallic);

				// 3. Calculate Fresnel (kS) and Diffuse (kD) ratios
				vec3 kS = fresnel_schlick_roughness(NdotV, F0, roughness);
				vec3 kD = 1.0 - kS;
				kD *= (1.0 - metallic); // Pure metals have no diffuse lighting

				// 4. Sample Diffuse Irradiance
				// We sample the cubemap with the Normal at a very high MIP level to blur it.
				// If you have a dedicated pre-convoluted irradiance map, use that instead at LOD 0.
				float max_lod = 8.0; // Adjust based on your texture's mip count
				vec3 irradiance = textureLod(samplerCube(cubemap_tex, tex_sampler), N, max_lod).rgb;
				vec3 diffuse = irradiance * sampled_color.rgb;

				// 5. Sample Specular Reflection
				// Sample along the reflection vector R. 
				// Roughness determines which MIP level we read from (rougher = blurrier).
				vec3 prefiltered_color = textureLod(samplerCube(cubemap_tex, tex_sampler), R, roughness * max_lod).rgb;

				// 6. Scale Specular by BRDF (The "Split Sum" Approximation)
				vec2 brdf  = env_brdf_approx(roughness, NdotV);
				vec3 specular = prefiltered_color * (kS * brdf.x + brdf.y);

				// 7. Combine and apply AO
				// Note: Ideally AO affects specular differently (specular occlusion), but this is a safe baseline.
				vec3 ambient = (kD * diffuse + specular);
				
				final_color.xyz += ambient; 
			}
			//TODO: 

			// Apply ambient occlusion
			final_color *= ambient_occlusion;
		}
	}

	frag_color = final_color;
}

@end

@program lighting vs fs
