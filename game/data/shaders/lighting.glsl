
// Common Shader Code
#include "shader_common.h"

// BRDF
#include "brdf.h"

// GI Helpers
#include "gi_helpers.h"

// Octahedral Helpers
#include "octahedral_helpers.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.glslh"

@fs fs

@include_block brdf 
@include_block gi_helpers
@include_block octahedral_helpers

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
	float gi_intensity;
	int atlas_total_size;
	int atlas_entry_size;
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

layout(binding=8) readonly buffer GIProbesBuffer {
	GI_Probe gi_probes[];
};

layout(binding=9) readonly buffer GICellsBuffer {
	GI_Cell gi_cells[];
};

layout(binding=10) uniform texture2D octahedral_atlas_texture;

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
		vec4 sampled_position = texture(sampler2D(position_tex, tex_sampler), uv);
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

			if (gi_enable != 0)
			{
				vec3 lookup_dir = normalize(normal);
				GI_Coords cell_coords = gi_cell_coords_from_position(position);
				int cell_index = gi_cell_index_from_coords(cell_coords);
				GI_Cell cell = gi_cells[cell_index];

				// 1. Find the "alpha" position of the pixel within this specific cell
				// This requires the world position of the '0,0,0' corner probe of the cell
				vec3 cell_min_pos = gi_probe_position_from_index(cell.probe_indices[0]);
				vec3 alpha = clamp((position - cell_min_pos) / GI_CELL_EXTENT, 0.0, 1.0);

				vec3 accumulated_irradiance = vec3(0.0);
				float accumulated_weight = 0.0;

				for (int i = 0; i < 8; ++i)
				{
					int probe_index = cell.probe_indices[i];
					if (probe_index == -1) continue;

					GI_Probe probe = gi_probes[probe_index];
					vec3 probe_position = gi_probe_position_from_index(probe_index);
					vec3 dir_to_probe = normalize(probe_position - position);

					// 2. Compute Trilinear Weight based on binary corner index (i)
					// x_side is 1.0 if the probe is on the "high" side of X, 0.0 if "low"
					float x_side = float(i & 1);
					float y_side = float((i >> 1) & 1);
					float z_side = float((i >> 2) & 1);

					// Linear blend based on distance to the cell boundaries
					float trilinear_weight = 
						(x_side > 0.5 ? alpha.x : 1.0 - alpha.x) *
						(y_side > 0.5 ? alpha.y : 1.0 - alpha.y) *
						(z_side > 0.5 ? alpha.z : 1.0 - alpha.z);

					// 3. Keep your Normal Weight to prevent light leaking
					float weight = trilinear_weight * max(0.0001, dot(dir_to_probe, lookup_dir));

					const vec2 octahedral_coords = padded_atlas_uv_from_normal(lookup_dir, probe.atlas_idx, atlas_total_size, atlas_entry_size);
					const vec3 probe_radiance = texture(sampler2D(octahedral_atlas_texture, tex_sampler), octahedral_coords).xyz;

					accumulated_irradiance += probe_radiance * weight;
					accumulated_weight += weight;
				}

				accumulated_weight = max(accumulated_weight, 0.0001);
				vec3 final_gi = (accumulated_irradiance / accumulated_weight) * color * gi_intensity;
				final_color.xyz += final_gi;
			}	

			// Apply ambient occlusion
			final_color *= ambient_occlusion;
		}
	}

	frag_color = final_color;
}

@end

@program lighting vs fs
