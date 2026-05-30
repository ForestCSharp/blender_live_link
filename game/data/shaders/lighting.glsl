
// Common Shader Code
#include "shader_common.h"

// BRDF
#include "brdf.h"

// GI Helpers
#include "gi_helpers.h"

// Octahedral Helpers
#include "octahedral_helpers.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.h"

@fs fs

@include_block brdf 
@include_block gi_helpers
@include_block octahedral_helpers
@include_block dither_noise
@include_block probe_radiance

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
@sampler_type shadow_moments_sampler filtering
layout(binding=1) uniform sampler shadow_moments_sampler;

layout(binding=0) uniform fs_params {
	vec3 view_position;
    int num_point_lights;
	int num_spot_lights;
	int num_sun_lights;
	int ssao_enable;

	int direct_lighting_enable;
	int gi_enable;
	int gi_probe_occlusion;
	int probe_occlusion_mode;
	int probe_radiance_mode;
	float gi_intensity;
	int atlas_total_size;
	int atlas_entry_size;
	int shadow_map_enable;
	float shadow_bias;
	vec2 shadow_map_texel_size;
	mat4 shadow_view_projection;
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

layout(binding=10) uniform texture2D octahedral_lighting_texture;
layout(binding=11) uniform texture2D octahedral_depth_texture;
@image_sample_type shadow_moments_texture float
layout(binding=12) uniform texture2D shadow_moments_texture;

layout(binding=13) readonly buffer SH9CoefficientsBuffer {
	ProbeRadianceCoefficient sh9_coefficients[];
};

layout(binding=14) readonly buffer SG9LobesBuffer {
	ProbeSGLobe sg9_lobes[];
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

float reduce_light_bleeding(float p_max, float reduction_amount)
{
	return clamp((p_max - reduction_amount) / (1.0 - reduction_amount), 0.0, 1.0);
}

float normalized_shadow_depth(vec3 in_shadow_ndc)
{
#if USE_INVERSE_DEPTH
	return 1.0 - in_shadow_ndc.z;
#else
	return in_shadow_ndc.z * 0.5 + 0.5;
#endif
}

const float EVSM_POSITIVE_EXPONENT = 5.0;
const float EVSM_NEGATIVE_EXPONENT = 5.0;

vec2 evsm_warp_depth(float depth)
{
	float centered_depth = depth * 2.0 - 1.0;
	return vec2(
		exp(EVSM_POSITIVE_EXPONENT * centered_depth),
		-exp(-EVSM_NEGATIVE_EXPONENT * centered_depth)
	);
}

float chebyshev_upper_bound(float mean, float mean_squared, float receiver_depth, float min_variance)
{
	if (receiver_depth <= mean)
	{
		return 1.0;
	}

	float variance = max(mean_squared - mean * mean, min_variance);
	float depth_delta = receiver_depth - mean;
	return variance / (variance + depth_delta * depth_delta);
}

const int PROBE_OCCLUSION_MODE_CHEBYSHEV = 0;
const int PROBE_OCCLUSION_MODE_EVRP4 = 1;

const int PROBE_RADIANCE_MODE_OCTAHEDRAL = 0;
const int PROBE_RADIANCE_MODE_SH9 = 1;
const int PROBE_RADIANCE_MODE_SG9 = 2;

const float EVRP_POSITIVE_EXPONENT = 5.0;
const float EVRP_NEGATIVE_EXPONENT = 5.0;

vec2 evrp_warp_depth(float normalized_depth)
{
	return vec2(
		exp(EVRP_POSITIVE_EXPONENT * normalized_depth),
		-exp(-EVRP_NEGATIVE_EXPONENT * normalized_depth)
	);
}

vec3 sample_probe_radiance(GI_Probe probe, int probe_index, vec3 normal)
{
	if (probe_radiance_mode == PROBE_RADIANCE_MODE_SH9)
	{
		vec3 irradiance = vec3(0.0);
		int coefficient_offset = probe_index * 9;
		for (int i = 0; i < 9; ++i)
		{
			irradiance += sh9_coefficients[coefficient_offset + i].value.rgb * sh9_basis(i, normal) * sh9_diffuse_convolution_factor(i);
		}
		return max(irradiance, vec3(0.0));
	}

	if (probe_radiance_mode == PROBE_RADIANCE_MODE_SG9)
	{
		vec3 reconstructed_radiance = vec3(0.0);
		float weight_sum = 0.0;
		int coefficient_offset = probe_index * 9;
		for (int i = 0; i < 9; ++i)
		{
			vec4 lobe = sg9_lobes[coefficient_offset + i].params;
			float response = sg_lobe_diffuse_response(normalize(lobe.xyz), lobe.w, normal);
			reconstructed_radiance += sg9_lobes[coefficient_offset + i].amplitude.rgb * response;
			weight_sum += response;
		}
		return max(reconstructed_radiance / max(weight_sum, 0.00001), vec3(0.0));
	}

	const vec2 octahedral_lighting_coords = padded_atlas_uv_from_normal(normal, probe.atlas_idx, atlas_total_size, atlas_entry_size);
	return texture(sampler2D(octahedral_lighting_texture, tex_sampler), octahedral_lighting_coords).rgb;
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

float slope_scaled_shadow_bias(vec3 in_surface_normal, vec3 in_light_direction)
{
	vec3 n = normalize(in_surface_normal);
	vec3 l = normalize(-in_light_direction);
	float normal_light = saturate(dot(n, l));
	float slope_factor = 1.0 - normal_light;
	return max(shadow_bias, shadow_bias * (1.0 + 8.0 * slope_factor));
}

float sample_shadow_visibility(vec3 in_surface_position, vec3 in_surface_normal, vec3 in_light_direction)
{
	if (shadow_map_enable == 0)
	{
		return 1.0;
	}

	const float normal_offset_bias = 0.02;
	vec3 shadow_sample_position = in_surface_position + normalize(in_surface_normal) * normal_offset_bias;
	vec4 shadow_clip_position = shadow_view_projection * vec4(shadow_sample_position, 1.0);
	if (shadow_clip_position.w <= 0.0)
	{
		return 1.0;
	}

	vec3 shadow_ndc = shadow_clip_position.xyz / shadow_clip_position.w;
	vec2 shadow_uv = shadow_ndc.xy * 0.5 + 0.5;
	shadow_uv.y = 1.0 - shadow_uv.y;
	if (shadow_uv.x < 0.0 || shadow_uv.x > 1.0 || shadow_uv.y < 0.0 || shadow_uv.y > 1.0)
	{
		return 1.0;
	}

#if USE_INVERSE_DEPTH
	if (shadow_ndc.z < 0.0 || shadow_ndc.z > 1.0)
	{
		return 1.0;
	}
#else
	if (shadow_ndc.z < -1.0 || shadow_ndc.z > 1.0)
	{
		return 1.0;
	}
#endif

	float receiver_depth = normalized_shadow_depth(shadow_ndc);
	float effective_shadow_bias = slope_scaled_shadow_bias(in_surface_normal, in_light_direction);
	receiver_depth -= effective_shadow_bias;

	vec4 moments = texture(sampler2D(shadow_moments_texture, shadow_moments_sampler), shadow_uv);
	vec2 warped_receiver_depth = evsm_warp_depth(receiver_depth);

	const float min_variance = 0.00001;
	const float light_bleed_reduction = 0.2;
	float positive_visibility = chebyshev_upper_bound(moments.x, moments.y, warped_receiver_depth.x, min_variance);
	float negative_visibility = chebyshev_upper_bound(moments.z, moments.w, warped_receiver_depth.y, min_variance);
	float p_max = min(positive_visibility, negative_visibility);
	return reduce_light_bleeding(p_max, light_bleed_reduction);
}

float sample_gi_shadow_visibility(vec3 in_surface_position, vec3 in_surface_normal)
{
	for (int i = 0; i < num_sun_lights; ++i)
	{
		if (sun_lights[i].cast_shadows != 0)
		{
			float shadow_visibility = sample_shadow_visibility(in_surface_position, in_surface_normal, sun_lights[i].direction);
			float normal_light = saturate(dot(normalize(in_surface_normal), normalize(-sun_lights[i].direction)));
			return shadow_visibility * normal_light;
		}
	}

	return 1.0;
}

in vec2 uv;

out vec4 frag_color;

float square(float in_x)
{
	return in_x * in_x;
}

vec2 square(vec2 in_v2)
{
	return in_v2 * in_v2;
}

vec3 square(vec3 in_v3)
{
	return in_v3 * in_v3;
}

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
			const vec3 normal = normalize(sampled_normal.xyz);
			const vec3 color = sampled_color.xyz;
			const float gi_shadow_multiplier = mix(0.5, 1.0, sample_gi_shadow_visibility(position, normal));

			if (direct_lighting_enable != 0)
			{
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
					float sun_shadow_visibility = sun_lights[i].cast_shadows != 0
						? sample_shadow_visibility(position, normal, sun_lights[i].direction)
						: 1.0;

					final_color.xyz += sun_shadow_visibility * sample_sun_light(
							sun_lights[i], 
							view_position,
							position, 
							normal, 
							color, 
							roughness, 
							metallic
							);
				}
			}

			if (gi_enable != 0)
			{
				GI_Coords cell_coords = gi_cell_coords_from_position(position);
				int cell_index = gi_cell_index_from_coords(cell_coords);

				if (cell_index < 0)
				{
					// Fallback probe is at end of probes array
					GI_Probe probe = gi_probes[GI_FALLBACK_PROBE_IDX];
					const vec3 probe_radiance = sample_probe_radiance(probe, GI_FALLBACK_PROBE_IDX, normal);
					
					const vec3 final_irradiance = probe_radiance;		
					const vec3 albedo = color * (1.0 - metallic); 
					vec3 final_gi = final_irradiance * albedo * gi_intensity * gi_shadow_multiplier;
					final_color.xyz += final_gi;
				}
				else
				{
					GI_Cell cell = gi_cells[cell_index];

					// 1. Find the "alpha" position of the pixel within this specific cell
					// This requires the world position of the '0,0,0' corner probe of the cell
					vec3 cell_min_pos = gi_probe_position_from_index(cell.probe_indices[0]);
					vec3 alpha = clamp((position - cell_min_pos) / GI_CELL_EXTENT, 0.0, 1.0);

					vec3 accumulated_irradiance_no_cheb = vec3(0.0);
					float accumulated_weight_no_cheb = 0.0;

					vec3 accumulated_irradiance = vec3(0.0);
					float accumulated_weight = 0.0;

					for (int i = 0; i < 8; ++i)
					{
						int probe_index = cell.probe_indices[i];
						if (probe_index == -1) { continue; }

						GI_Probe probe = gi_probes[probe_index];

						if (probe.atlas_idx < 0) { continue; }
						const vec3 probe_radiance = sample_probe_radiance(probe, probe_index, normal);
						const vec3 probe_position = gi_probe_position_from_index(probe_index);
						const vec3 position_to_probe = probe_position - position;
						const vec3 probe_to_pos = position - probe_position;
						const float dist_to_pixel = length(probe_to_pos);
						const vec3 dir_to_probe = normalize(position_to_probe);
						vec3 dir_from_probe = -dir_to_probe;

						// 1. Smooth Backface
						float weight = (dot(dir_to_probe, normal) + 1.0) * 0.5;

						// 2. Trilinear Weight 
						{
							// x_side is 1.0 if the probe is on the "high" side of X, 0.0 if "low"
							float x_side = float(i & 1);
							float y_side = float((i >> 1) & 1);
							float z_side = float((i >> 2) & 1);

							// Linear blend based on distance to the cell boundaries
							float trilinear_weight = 
								(x_side > 0.5 ? alpha.x : 1.0 - alpha.x) *
								(y_side > 0.5 ? alpha.y : 1.0 - alpha.y) *
								(z_side > 0.5 ? alpha.z : 1.0 - alpha.z);

							weight *= trilinear_weight + 0.001;
						}

						// Store irradiance without chebyshev weighting
						accumulated_irradiance_no_cheb += probe_radiance * weight;
						accumulated_weight_no_cheb += weight;

						// 3. Visibility (Chebyshev)
						if (gi_probe_occlusion != 0)
						{
							const vec2 octahedral_depth_coords = padded_atlas_uv_from_normal(dir_from_probe, probe.atlas_idx, atlas_total_size, atlas_entry_size);
							vec4 moments = texture(sampler2D(octahedral_depth_texture, tex_sampler), octahedral_depth_coords);

							if (probe_occlusion_mode == PROBE_OCCLUSION_MODE_EVRP4)
							{
								const float normalized_depth = clamp(dist_to_pixel / GI_MAX_RADIAL_DEPTH, 0.0, 1.0);
								const vec2 warped_receiver_depth = evrp_warp_depth(normalized_depth);
								const float min_variance = 0.00001;
								const float light_bleed_reduction = 0.2;
								float positive_visibility = chebyshev_upper_bound(moments.x, moments.y, warped_receiver_depth.x, min_variance);
								float negative_visibility = chebyshev_upper_bound(moments.z, moments.w, warped_receiver_depth.y, min_variance);
								float visibility = min(positive_visibility, negative_visibility);
								visibility = reduce_light_bleeding(visibility, light_bleed_reduction);
								weight *= visibility;
							}
							else
							{
								const float mean = moments.x;
								const float mean_squared = moments.y;
								if (dist_to_pixel > mean)
								{
									const float variance = abs(square(mean) - mean_squared);
									weight *= variance / (variance + square(dist_to_pixel - mean));
								}
							}
						}

						const float threshold = 0.01f;
						if (weight < threshold)
						{
							weight *= square(weight) / square(threshold);
						}

						//FCS TDOO: sqrt in accum, square before final apply

						accumulated_irradiance += probe_radiance * weight;
						accumulated_weight += weight;
					}

					const vec3 final_irradiance = mix(
						accumulated_irradiance_no_cheb * (1.0 / accumulated_weight_no_cheb),
						accumulated_irradiance * (1.0 / accumulated_weight),
						saturate(accumulated_weight)
					);

					const vec3 albedo = color * (1.0 - metallic); 

					vec3 final_gi = final_irradiance * albedo * gi_intensity * gi_shadow_multiplier;
					final_color.xyz += final_gi;
				}
			}	

			// Apply ambient occlusion
			final_color *= ambient_occlusion;
		}
	}

	final_color += dither_noise();

	frag_color = final_color;
}

@end

@program lighting vs fs
