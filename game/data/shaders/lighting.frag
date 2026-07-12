#version 450

// Deferred lighting, including probe GI, SSAO/contact shadows, and EVSM.

#include "shader_common.h"
#include "brdf.h"
#include "gi_helpers.h"
#include "octahedral_helpers.h"
#include "probe_radiance.h"

// Set 0 (lighting layout C)
layout(set = 0, binding = 0, std140) uniform LightingParamsBlock
{
	vec3 view_position;
	vec3 view_forward;
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
	int gi_fallback_probe_index;
	int gi_octree_node_count;
	int shadow_map_enable;
	int shadow_num_cascades;
	int shadow_cascade_placement_mode;
	int shadow_debug_show_cascade_selection;
	int isolated_probe_index;
	int screen_space_shadows_enable;
	float shadow_bias;
	float screen_space_shadow_intensity;
	vec2 shadow_map_texel_size;
	vec4 shadow_cascade_distances;
	mat4 shadow_view_projections[4];
};

layout(set = 0, binding = 1) uniform sampler2D color_tex;
layout(set = 0, binding = 2) uniform sampler2D position_tex;
layout(set = 0, binding = 3) uniform sampler2D normal_tex;
layout(set = 0, binding = 4) uniform sampler2D roughness_metallic_emissive_tex;

#if defined(SHADOWS_ENABLED)
layout(set = 0, binding = 5) uniform sampler2DArray shadow_moments_tex;
#endif

layout(set = 0, binding = 6, std430) readonly buffer PointLightsBlock
{
	PointLightData point_lights[];
};

// Blurred half-res SSAO (BlurPass output)
layout(set = 0, binding = 9) uniform sampler2D ssao_tex;

// Filtered half-res screen-space contact shadow mask
layout(set = 0, binding = 10) uniform sampler2D screen_space_shadow_tex;

layout(set = 0, binding = 11, std430) readonly buffer GIProbesBlock
{
	GI_Probe gi_probes[];
};

layout(set = 0, binding = 12, std430) readonly buffer GICellsBlock
{
	GI_Cell gi_cells[];
};

layout(set = 0, binding = 13) uniform sampler2D octahedral_lighting_texture;
layout(set = 0, binding = 14) uniform sampler2D octahedral_depth_texture;

layout(set = 0, binding = 15, std430) readonly buffer SH9CoefficientsBlock
{
	ProbeRadianceCoefficient sh9_coefficients[];
};

layout(set = 0, binding = 16, std430) readonly buffer SG9LobesBlock
{
	ProbeSGLobe sg9_lobes[];
};

layout(set = 0, binding = 17, std430) readonly buffer GIOctreeNodesBlock
{
	GI_OctreeNode gi_octree_nodes[];
};

layout(set = 0, binding = 7, std430) readonly buffer SpotLightsBlock
{
	SpotLightData spot_lights[];
};

layout(set = 0, binding = 8, std430) readonly buffer SunLightsBlock
{
	SunLightData sun_lights[];
};

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

// ---- helpers (ports of game/ lighting.glsl + shader_common.h) ----

float dist_squared(vec3 a, vec3 b)
{
	vec3 c = a - b;
	return dot(c, c);
}

float cosine_angle(vec3 a, vec3 b)
{
	return dot(normalize(a), normalize(b));
}

// Gradient noise from Jorge Jimenez's presentation:
// http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
float gradient_noise(in vec2 noise_uv)
{
	return fract(52.9829189 * fract(dot(noise_uv, vec2(0.06711056, 0.00583715))));
}

vec4 dither_noise()
{
	return vec4(vec3((1.0 / 255.0) * gradient_noise(gl_FragCoord.xy) - (0.5 / 255.0)), 0.0);
}

// ---- light sampling (ports of game/ lighting.glsl:300-407) ----

vec3 sample_point_light(
	PointLightData in_point_light,
	vec3 in_view_position,
	vec3 in_surface_position,
	vec3 in_surface_normal,
	vec3 in_surface_albedo,
	float in_surface_roughness,
	float in_surface_metallic
)
{
	vec3 light_location = in_point_light.location.xyz;
	vec3 n = in_surface_normal;
	vec3 l = normalize(light_location - in_surface_position);
	vec3 v = normalize(in_view_position - in_surface_position);

	float light_attenuation = in_point_light.power / (4 * M_PI * dist_squared(light_location, in_surface_position));
	vec3 light_radiance = in_point_light.color.xyz * light_attenuation;

	vec3 f0 = mix(vec3(0.04, 0.04, 0.04), in_surface_albedo, vec3(in_surface_metallic));
	return cook_torrance_brdf(n, l, v, in_surface_albedo, f0, in_surface_roughness, in_surface_metallic, light_radiance);
}

vec3 sample_spot_light(
	SpotLightData in_spot_light,
	vec3 in_view_position,
	vec3 in_surface_position,
	vec3 in_surface_normal,
	vec3 in_surface_albedo,
	float in_surface_roughness,
	float in_surface_metallic
)
{
	vec3 light_location = in_spot_light.location.xyz;

	// Spot light cone check (hard edge — edge_blend unused, game/ parity)
	vec3 light_to_world_pos = normalize(in_surface_position - light_location);
	float surface_cosine_angle = cosine_angle(light_to_world_pos, in_spot_light.direction.xyz);
	float outer_cone_cosine_angle = cos(in_spot_light.spot_angle_radians);

	if (surface_cosine_angle <= outer_cone_cosine_angle)
	{
		return vec3(0, 0, 0);
	}

	PointLightData point_light;
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
	SunLightData in_sun_light,
	vec3 in_view_position,
	vec3 in_surface_position,
	vec3 in_surface_normal,
	vec3 in_surface_albedo,
	float in_surface_roughness,
	float in_surface_metallic
)
{
	vec3 n = in_surface_normal;
	vec3 l = -normalize(in_sun_light.direction.xyz);
	vec3 v = normalize(in_view_position - in_surface_position);

	float light_attenuation = in_sun_light.power;
	vec3 light_radiance = in_sun_light.color.xyz * light_attenuation;

	vec3 f0 = mix(vec3(0.04, 0.04, 0.04), in_surface_albedo, vec3(in_surface_metallic));
	return cook_torrance_brdf(n, l, v, in_surface_albedo, f0, in_surface_roughness, in_surface_metallic, light_radiance);
}

// ---- EVSM shadow sampling (enabled in the shadow step) ----

#if defined(SHADOWS_ENABLED)

const float EVSM_POSITIVE_EXPONENT = 5.0;
const float EVSM_NEGATIVE_EXPONENT = 5.0;

// Matches the depth-pass warp exactly (game/ shadow_depth.glsl:52-62)
vec2 evsm_warp_depth(float depth)
{
	float centered_depth = depth * 2.0 - 1.0;
	return vec2(
		exp(EVSM_POSITIVE_EXPONENT * centered_depth),
		-exp(-EVSM_NEGATIVE_EXPONENT * centered_depth)
	);
}

float reduce_light_bleeding(float p_max, float reduction_amount)
{
	return clamp((p_max - reduction_amount) / (1.0 - reduction_amount), 0.0, 1.0);
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

float square(float value)
{
	return value * value;
}

vec3 sample_probe_radiance(GI_Probe probe, int probe_index, vec3 normal)
{
	if (probe.atlas_idx < 0)
	{
		return vec3(0.0);
	}

	if (probe_radiance_mode == PROBE_RADIANCE_MODE_SH9)
	{
		vec3 irradiance = vec3(0.0);
		int coefficient_offset = probe_index * 9;
		for (int i = 0; i < 9; ++i)
		{
			irradiance += sh9_coefficients[coefficient_offset + i].value.rgb
				* sh9_basis(i, normal) * sh9_diffuse_convolution_factor(i);
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

	vec2 atlas_uv = padded_atlas_uv_from_normal(normal, probe.atlas_idx, atlas_total_size, atlas_entry_size);
	return texture(octahedral_lighting_texture, atlas_uv).rgb;
}

int find_gi_octree_payload_index(vec3 position)
{
	if (gi_octree_node_count <= 0)
	{
		return -1;
	}

	int node_index = 0;
	GI_OctreeNode node = gi_octree_nodes[node_index];
	if (!gi_octree_is_valid_position(node, position))
	{
		return -1;
	}

	int payload_index = node.payload_index;
	for (int i = 0; i < GI_MAX_OCTREE_SEARCH_DEPTH; ++i)
	{
		if (node.is_leaf != 0)
		{
			return payload_index;
		}
		int child_index = node.child_indices[gi_octree_child_slot(node, position)];
		if (child_index < 0 || child_index >= gi_octree_node_count)
		{
			return payload_index;
		}
		node = gi_octree_nodes[child_index];
		if (node.payload_index >= 0)
		{
			payload_index = node.payload_index;
		}
	}
	return payload_index;
}

float slope_scaled_shadow_bias(vec3 in_surface_normal, vec3 in_light_direction)
{
	vec3 n = normalize(in_surface_normal);
	vec3 l = normalize(-in_light_direction);
	float normal_light = saturate(dot(n, l));
	float slope_factor = 1.0 - normal_light;
	return max(shadow_bias, shadow_bias * (1.0 + 8.0 * slope_factor));
}

const int SHADOW_CASCADE_PLACEMENT_FRUSTUM = 0;
const int SHADOW_CASCADE_PLACEMENT_CENTERED_SQUARES = 1;

// Cascade selection (game/ lighting.glsl:420-463). Frustum mode: first
// cascade whose view-forward distance covers the receiver. CenteredSquares
// mode: first cascade whose square actually contains the receiver.
int select_shadow_cascade(vec3 in_surface_position)
{
	if (shadow_cascade_placement_mode == SHADOW_CASCADE_PLACEMENT_CENTERED_SQUARES)
	{
		for (int cascade_idx = 0; cascade_idx < shadow_num_cascades; ++cascade_idx)
		{
			vec4 shadow_clip = shadow_view_projections[cascade_idx] * vec4(in_surface_position, 1.0);
			if (shadow_clip.w <= 0.0)
			{
				continue;
			}

			vec3 ndc = shadow_clip.xyz / shadow_clip.w;
			vec2 shadow_uv = ndc.xy * 0.5 + 0.5;
			shadow_uv.y = 1.0 - shadow_uv.y;
			if (shadow_uv.x >= 0.0 && shadow_uv.x <= 1.0 && shadow_uv.y >= 0.0 && shadow_uv.y <= 1.0)
			{
				return cascade_idx;
			}
		}

		return -1;
	}

	float receiver_camera_distance = dot(in_surface_position - view_position, normalize(view_forward));
	for (int cascade_idx = 0; cascade_idx < shadow_num_cascades; ++cascade_idx)
	{
		if (receiver_camera_distance <= shadow_cascade_distances[cascade_idx])
		{
			return cascade_idx;
		}
	}
	return -1;
}

vec3 debug_shadow_cascade_color(vec3 in_surface_position)
{
	int cascade_idx = select_shadow_cascade(in_surface_position);
	if (cascade_idx == 0) { return vec3(1.0, 0.0, 0.0); }
	if (cascade_idx == 1) { return vec3(0.0, 1.0, 0.0); }
	if (cascade_idx == 2) { return vec3(0.0, 0.0, 1.0); }
	if (cascade_idx == 3) { return vec3(1.0, 1.0, 0.0); }
	return vec3(0.0);
}

float sample_shadow_visibility(vec3 in_surface_position, vec3 in_surface_normal, vec3 in_light_direction)
{
	if (shadow_map_enable == 0)
	{
		return 1.0;
	}

	int cascade_idx = select_shadow_cascade(in_surface_position);
	if (cascade_idx < 0)
	{
		return 1.0;
	}

	// Normal-offset bias
	vec3 biased_position = in_surface_position + normalize(in_surface_normal) * 0.02;

	vec4 shadow_clip = shadow_view_projections[cascade_idx] * vec4(biased_position, 1.0);
	if (shadow_clip.w <= 0.0)
	{
		return 1.0;
	}
	vec3 ndc = shadow_clip.xyz / shadow_clip.w;

	vec2 shadow_uv = ndc.xy * 0.5 + 0.5;
	shadow_uv.y = 1.0 - shadow_uv.y;

	if (shadow_uv.x < 0.0 || shadow_uv.x > 1.0 || shadow_uv.y < 0.0 || shadow_uv.y > 1.0
		|| ndc.z < 0.0 || ndc.z > 1.0)
	{
		return 1.0;
	}

	// Reverse-Z: normalize receiver depth so larger = farther from light
	float receiver_depth = 1.0 - ndc.z;
	receiver_depth -= slope_scaled_shadow_bias(in_surface_normal, in_light_direction);

	vec4 moments = texture(shadow_moments_tex, vec3(shadow_uv, float(cascade_idx)));
	vec2 warped_receiver_depth = evsm_warp_depth(receiver_depth);

	const float min_variance = 0.00001;
	const float light_bleed_reduction = 0.2;
	float positive_visibility = chebyshev_upper_bound(moments.x, moments.y, warped_receiver_depth.x, min_variance);
	float negative_visibility = chebyshev_upper_bound(moments.z, moments.w, warped_receiver_depth.y, min_variance);
	float p_max = min(positive_visibility, negative_visibility);
	return reduce_light_bleeding(p_max, light_bleed_reduction);
}

#else

float sample_shadow_visibility(vec3 in_surface_position, vec3 in_surface_normal, vec3 in_light_direction)
{
	return 1.0;
}

#endif // SHADOWS_ENABLED

float sample_gi_shadow_visibility(vec3 surface_position, vec3 surface_normal)
{
	for (int i = 0; i < num_sun_lights; ++i)
	{
		if (sun_lights[i].cast_shadows != 0)
		{
			float visibility = sample_shadow_visibility(surface_position, surface_normal, sun_lights[i].direction.xyz);
			float normal_light = saturate(dot(normalize(surface_normal), normalize(-sun_lights[i].direction.xyz)));
			return visibility * normal_light;
		}
	}
	return 1.0;
}

void main()
{
	vec4 final_color = vec4(0);

	vec4 sampled_color = texture(color_tex, uv);
	vec4 sampled_normal = texture(normal_tex, uv);

	if (sampled_normal == vec4(0))
	{
		// Sky / no-geometry sentinel: pass color straight through
		final_color = sampled_color;
	}
	else
	{
		vec4 sampled_position = texture(position_tex, uv);
		vec4 sampled_rme = texture(roughness_metallic_emissive_tex, uv);
		float roughness = sampled_rme.r;
		float metallic = sampled_rme.g;
		float emission_strength = sampled_rme.b;

		float ambient_occlusion = (ssao_enable != 0) ? texture(ssao_tex, uv).r : 1.0;

		// When emission_strength > 0, the emission color lives in color_tex
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

#if defined(SHADOWS_ENABLED)
			if (shadow_debug_show_cascade_selection != 0)
			{
				frag_color = vec4(debug_shadow_cascade_color(position), 1.0);
				return;
			}
#endif

			if (direct_lighting_enable != 0)
			{
				for (int i = 0; i < num_point_lights; ++i)
				{
					final_color.xyz += sample_point_light(
						point_lights[i], view_position, position, normal,
						color, roughness, metallic);
				}

				for (int i = 0; i < num_spot_lights; ++i)
				{
					final_color.xyz += sample_spot_light(
						spot_lights[i], view_position, position, normal,
						color, roughness, metallic);
				}

				for (int i = 0; i < num_sun_lights; ++i)
				{
					float sun_shadow_visibility = sun_lights[i].cast_shadows != 0
						? sample_shadow_visibility(position, normal, sun_lights[i].direction.xyz)
						: 1.0;

					if (screen_space_shadows_enable != 0 && sun_lights[i].cast_shadows != 0)
					{
						float screen_space_shadow_visibility = texture(screen_space_shadow_tex, uv).r;
						sun_shadow_visibility *= mix(1.0, screen_space_shadow_visibility, screen_space_shadow_intensity);
					}

					final_color.xyz += sun_shadow_visibility * sample_sun_light(
						sun_lights[i], view_position, position, normal,
						color, roughness, metallic);
				}
			}

			if (gi_enable != 0)
			{
				int cell_index = find_gi_octree_payload_index(position);
				bool probe_isolation_active = isolated_probe_index != -1;

				if (cell_index < 0)
				{
					if (!probe_isolation_active && gi_fallback_probe_index >= 0)
					{
						GI_Probe probe = gi_probes[gi_fallback_probe_index];
						if (probe.atlas_idx >= 0)
						{
							vec3 irradiance = sample_probe_radiance(probe, gi_fallback_probe_index, normal);
							final_color.xyz += irradiance * color * (1.0 - metallic) * gi_intensity * gi_shadow_multiplier;
						}
					}
				}
				else
				{
					GI_Cell cell = gi_cells[cell_index];
					vec3 cell_min_pos = gi_probes[cell.probe_indices[0]].position.xyz;
					vec3 cell_max_pos = gi_probes[cell.probe_indices[7]].position.xyz;
					vec3 cell_extent = max(cell_max_pos - cell_min_pos, vec3(0.00001));
					vec3 alpha = clamp((position - cell_min_pos) / cell_extent, 0.0, 1.0);
					vec3 unoccluded_irradiance = vec3(0.0);
					float unoccluded_weight = 0.0;
					vec3 occluded_irradiance = vec3(0.0);
					float occluded_weight = 0.0;

					for (int i = 0; i < 8; ++i)
					{
						int probe_index = cell.probe_indices[i];
						if (probe_index < 0 || (probe_isolation_active && probe_index != isolated_probe_index))
						{
							continue;
						}
						GI_Probe probe = gi_probes[probe_index];
						if (probe.atlas_idx < 0)
						{
							continue;
						}

						vec3 probe_radiance = sample_probe_radiance(probe, probe_index, normal);
						vec3 position_to_probe = probe.position.xyz - position;
						vec3 probe_to_position = -position_to_probe;
						float distance_to_pixel = length(probe_to_position);
						vec3 direction_to_probe = normalize(position_to_probe);
						vec3 direction_from_probe = -direction_to_probe;
						float weight = (dot(direction_to_probe, normal) + 1.0) * 0.5;

						float x_side = float(i & 1);
						float y_side = float((i >> 1) & 1);
						float z_side = float((i >> 2) & 1);
						float trilinear_weight =
							(x_side > 0.5 ? alpha.x : 1.0 - alpha.x) *
							(y_side > 0.5 ? alpha.y : 1.0 - alpha.y) *
							(z_side > 0.5 ? alpha.z : 1.0 - alpha.z);
						weight *= trilinear_weight + 0.001;

						unoccluded_irradiance += probe_radiance * weight;
						unoccluded_weight += weight;

						if (gi_probe_occlusion != 0)
						{
							vec2 depth_uv = padded_atlas_uv_from_normal(
								direction_from_probe, probe.atlas_idx, atlas_total_size, atlas_entry_size);
							vec4 moments = texture(octahedral_depth_texture, depth_uv);
							if (probe_occlusion_mode == PROBE_OCCLUSION_MODE_EVRP4)
							{
								float normalized_depth = clamp(distance_to_pixel / max(probe.max_radial_depth, 0.00001), 0.0, 1.0);
								vec2 warped_depth = evrp_warp_depth(normalized_depth);
								float positive = chebyshev_upper_bound(moments.x, moments.y, warped_depth.x, 0.00001);
								float negative = chebyshev_upper_bound(moments.z, moments.w, warped_depth.y, 0.00001);
								weight *= reduce_light_bleeding(min(positive, negative), 0.2);
							}
							else if (distance_to_pixel > moments.x)
							{
								float variance = abs(square(moments.x) - moments.y);
								weight *= variance / (variance + square(distance_to_pixel - moments.x));
							}
						}

						const float threshold = 0.01;
						if (weight < threshold)
						{
							weight *= square(weight) / square(threshold);
						}
						occluded_irradiance += probe_radiance * weight;
						occluded_weight += weight;
					}

					if (unoccluded_weight > 0.0 && occluded_weight > 0.0)
					{
						vec3 irradiance = mix(
							unoccluded_irradiance / unoccluded_weight,
							occluded_irradiance / occluded_weight,
							saturate(occluded_weight));
						final_color.xyz += irradiance * color * (1.0 - metallic) * gi_intensity * gi_shadow_multiplier;
					}
				}
			}

			final_color *= ambient_occlusion;
		}
	}

	final_color += dither_noise();

	frag_color = final_color;
}
