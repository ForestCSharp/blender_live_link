#version 450

// Deferred lighting, including probe GI, SSAO, and EVSM.

#include "shader_common.h"
#include "brdf.h"
#include "gi_helpers.h"
#include "octahedral_helpers.h"
#include "probe_radiance.h"
#include "evsm.h"
#define BRUNETON_PARAMETER_BINDING 20
#include "bruneton_parameters.h"

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
	int probe_specular_enable;
	int specular_atlas_total_size;
	int specular_atlas_entry_size;
	int specular_mip_count;
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
	float shadow_bias;
	int atmosphere_sun_index;
	int atmosphere_enabled;
	float atmosphere_planet_center_z;
	vec2 shadow_map_texel_size;
	vec4 shadow_cascade_distances;
	vec3 shadow_cascade_view_position;
	vec3 shadow_cascade_view_forward;
	mat4 shadow_view_projections[4];
	vec4 cloud_shadow_extent_enabled;
	int moments_atlas_total_size;
	int moments_atlas_entry_size;
	int probe_relight;	// GI relight: inputs are the probe geometry cache, not the G-buffer
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
layout(set = 0, binding = 10) uniform sampler2D cloud_shadow_transmittance_tex;

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

layout(set = 0, binding = 18) uniform sampler2D specular_probe_atlas;
layout(set = 0, binding = 19) uniform sampler2D brdf_lut;
layout(set = 0, binding = 21) uniform sampler2D atmosphere_transmittance_tex;

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

// ---- shared lighting helpers ----

float dist_squared(vec3 a, vec3 b)
{
	vec3 c = a - b;
	return dot(c, c);
}

float cosine_angle(vec3 a, vec3 b)
{
	return dot(normalize(a), normalize(b));
}

// ---- analytic light sampling ----

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
	return cook_torrance_brdf(n, l, v, in_surface_albedo, f0,
		in_surface_roughness, in_surface_metallic, light_radiance);
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

	// Spot light cone check uses a hard edge; edge_blend is currently unused.
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
	int in_sun_index,
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

	vec3 light_radiance = GetSceneSolarIrradiance(
		in_sun_light.color.xyz, in_sun_light.power);
	if (atmosphere_enabled != 0 && in_sun_index == atmosphere_sun_index)
	{
		AtmosphereParameters atmosphere = GetAtmosphere();
		vec3 atmosphere_position = GetAtmosphereCameraPosition(
			atmosphere, in_surface_position, atmosphere_planet_center_z);
		light_radiance *= GetAtmosphereSunTransmittance(
			atmosphere, atmosphere_transmittance_tex, atmosphere_position, l);
	}
	light_radiance = SanitizeSceneColor(light_radiance);

	vec3 f0 = mix(vec3(0.04, 0.04, 0.04), in_surface_albedo, vec3(in_surface_metallic));
	return SanitizeSceneColor(cook_torrance_brdf(
		n, l, v, in_surface_albedo, f0, in_surface_roughness,
		in_surface_metallic, light_radiance));
}

// ---- EVSM shadow sampling (enabled in the shadow step) ----

#if defined(SHADOWS_ENABLED)

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

vec3 sample_probe_specular(GI_Probe probe, vec3 reflection_direction, float roughness)
{
	if (probe.specular_atlas_idx < 0 || probe_specular_enable == 0 || specular_mip_count <= 0)
	{
		return vec3(0.0);
	}

	float lod = clamp(roughness, 0.0, 1.0) * float(specular_mip_count - 1);
	int low_mip = int(floor(lod));
	int high_mip = min(low_mip + 1, specular_mip_count - 1);
	float mip_alpha = fract(lod);

	int low_total_size = specular_atlas_total_size >> low_mip;
	int low_entry_size = specular_atlas_entry_size >> low_mip;
	vec2 low_uv = padded_atlas_uv_from_normal(
		reflection_direction, probe.specular_atlas_idx, low_total_size, low_entry_size);
	vec3 low_radiance = textureLod(specular_probe_atlas, low_uv, float(low_mip)).rgb;

	int high_total_size = specular_atlas_total_size >> high_mip;
	int high_entry_size = specular_atlas_entry_size >> high_mip;
	vec2 high_uv = padded_atlas_uv_from_normal(
		reflection_direction, probe.specular_atlas_idx, high_total_size, high_entry_size);
	vec3 high_radiance = textureLod(specular_probe_atlas, high_uv, float(high_mip)).rgb;
	return mix(low_radiance, high_radiance, mip_alpha);
}

// Returns the deepest diffuse payload (leaf cell) containing position, or -1.
// out_specular_payload_index: the coarse specular cell (depth D-1) passed on
// the way down, or -1.
int find_gi_octree_payload_index(vec3 position, out int out_specular_payload_index)
{
	out_specular_payload_index = -1;
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
		if (node.specular_payload_index >= 0)
		{
			out_specular_payload_index = node.specular_payload_index;
		}
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

// Blends one probe term over a cell's 8 corners: each corner weighted by how
// much it faces the surface times its trilinear weight, then by radial-depth
// visibility, and the unoccluded/occluded averages mixed by total visibility.
// Diffuse uses the leaf cell, specular the coarser one. False if no weight.
bool gi_blend_cell(
	int in_cell_index, bool in_specular, vec3 position, vec3 normal,
	vec3 reflection_direction, float roughness, out vec3 out_value)
{
	out_value = vec3(0.0);
	bool probe_isolation_active = isolated_probe_index != -1;
	GI_Cell cell = gi_cells[in_cell_index];
	vec3 cell_min_pos = gi_probes[cell.probe_indices[0]].position.xyz;
	vec3 cell_max_pos = gi_probes[cell.probe_indices[7]].position.xyz;
	vec3 cell_extent = max(cell_max_pos - cell_min_pos, vec3(0.00001));
	vec3 alpha = clamp((position - cell_min_pos) / cell_extent, 0.0, 1.0);
	vec3 unoccluded_value = vec3(0.0);
	float unoccluded_weight = 0.0;
	vec3 occluded_value = vec3(0.0);
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

		vec3 probe_value = in_specular
			? sample_probe_specular(probe, reflection_direction, roughness)
			: sample_probe_radiance(probe, probe_index, normal);
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

		unoccluded_value += probe_value * weight;
		unoccluded_weight += weight;

		if (gi_probe_occlusion != 0)
		{
			vec2 depth_uv = padded_atlas_uv_from_normal(
				direction_from_probe, probe.atlas_idx, moments_atlas_total_size, moments_atlas_entry_size);
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
		occluded_value += probe_value * weight;
		occluded_weight += weight;
	}

	if (unoccluded_weight <= 0.0 || occluded_weight <= 0.0)
	{
		return false;
	}
	out_value = mix(
		unoccluded_value / unoccluded_weight,
		occluded_value / occluded_weight,
		saturate(occluded_weight));
	return true;
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

// Frustum mode selects the first cascade whose view-forward distance covers
// the receiver. CenteredSquares mode selects the first cascade whose square
// contains the receiver.
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

	// Use the camera basis captured with the cascade matrices. The active view
	// continues moving while shadow depth is frozen, but cascade classification
	// must remain consistent with the stale map.
	float receiver_camera_distance = dot(
		in_surface_position - shadow_cascade_view_position,
		normalize(shadow_cascade_view_forward)
	);
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

void main()
{
	vec4 final_color = vec4(0);

	vec4 sampled_color;
	vec4 sampled_normal;
	vec4 sampled_position;
	vec4 sampled_rme;
	vec3 eye_position = view_position;
	if (probe_relight != 0)
	{
		// One radiance texel of one probe: the slot (== probe index) and the
		// octahedral direction come from the pixel; the cache says what the
		// probe sees there (color_tex = surface, normal_tex = normal,
		// position_tex = hit). Sky texels keep the sky pass's radiance.
		const ivec2 texel = ivec2(gl_FragCoord.xy);
		const ivec2 slot = texel / moments_atlas_entry_size;
		const int probe_index = slot.y * (moments_atlas_total_size / moments_atlas_entry_size) + slot.x;
		const vec4 hit = texelFetch(position_tex, texel, 0);
		if (hit.x <= 0.0)
		{
			discard;
		}
		const vec2 entry_uv = (vec2(texel - slot * moments_atlas_entry_size) + 0.5) / float(moments_atlas_entry_size);
		const vec3 direction = octahedral_decode(make_padded_atlas_uv(entry_uv, float(moments_atlas_entry_size)));
		const vec4 surface = texelFetch(color_tex, texel, 0);
		const vec4 packed_normal = texelFetch(normal_tex, texel, 0);
		eye_position = gi_probes[probe_index].position.xyz;
		sampled_color = vec4(surface.rgb, 1.0);
		sampled_normal = vec4(packed_normal.xyz * 2.0 - 1.0, 0.0);
		sampled_position = vec4(eye_position + direction * hit.x, 1.0);
		sampled_rme = vec4(surface.a, packed_normal.a, hit.y, 0.0);
	}
	else
	{
		sampled_color = texture(color_tex, uv);
		sampled_normal = texture(normal_tex, uv);
		sampled_position = texture(position_tex, uv);
		sampled_rme = texture(roughness_metallic_emissive_tex, uv);
	}

	if (sampled_normal == vec4(0))
	{
		// Sky / no-geometry sentinel: pass color straight through
		final_color = sampled_color;
	}
	else
	{
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
			const vec3 view_direction = normalize(eye_position - position);
			const vec3 reflection_direction = reflect(-view_direction, normal);
			const float n_dot_v = max(dot(normal, view_direction), 0.0);
			const vec3 f0 = mix(vec3(0.04), color, vec3(metallic));
			const vec3 environment_fresnel = fresnel_schlick_roughness(n_dot_v, f0, roughness);
			const vec3 environment_diffuse_weight = (vec3(1.0) - environment_fresnel) * (1.0 - metallic);
			const vec2 environment_brdf = texture(brdf_lut, vec2(n_dot_v, roughness)).rg;

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
						point_lights[i], eye_position, position, normal,
						color, roughness, metallic);
				}

				for (int i = 0; i < num_spot_lights; ++i)
				{
					final_color.xyz += sample_spot_light(
						spot_lights[i], eye_position, position, normal,
						color, roughness, metallic);
				}

				for (int i = 0; i < num_sun_lights; ++i)
				{
					float sun_shadow_visibility = sun_lights[i].cast_shadows != 0
						? sample_shadow_visibility(position, normal, sun_lights[i].direction.xyz)
						: 1.0;
					if (cloud_shadow_extent_enabled.y > 0.5
						&& i == atmosphere_sun_index
						&& sun_lights[i].cast_shadows != 0)
					{
						float cloud_extent = max(cloud_shadow_extent_enabled.x, 1000.0);
						vec2 cloud_uv = (position.xy - view_position.xy) / cloud_extent + 0.5;
						if (all(greaterThanEqual(cloud_uv, vec2(0.0)))
							&& all(lessThanEqual(cloud_uv, vec2(1.0))))
						{
							float boundary = smoothstep(0.0, 0.08,
								min(min(cloud_uv.x, cloud_uv.y),
									min(1.0 - cloud_uv.x, 1.0 - cloud_uv.y)));
							float cloud_visibility = texture(cloud_shadow_transmittance_tex, cloud_uv).r;
							sun_shadow_visibility *= mix(1.0, cloud_visibility, boundary);
						}
					}

					final_color.xyz += sun_shadow_visibility * sample_sun_light(
						sun_lights[i], i, eye_position, position, normal,
						color, roughness, metallic);
				}
			}

			if (gi_enable != 0)
			{
				// No receiver-side sun darkening: probes are now captured with sun
				// shadows (GIShadowMap), so their light already accounts for it.
				int specular_cell_index = -1;
				int cell_index = find_gi_octree_payload_index(position, specular_cell_index);
				bool probe_isolation_active = isolated_probe_index != -1;

				if (cell_index < 0)
				{
					if (!probe_isolation_active && gi_fallback_probe_index >= 0)
					{
						GI_Probe probe = gi_probes[gi_fallback_probe_index];
						if (probe.atlas_idx >= 0)
						{
							vec3 irradiance = sample_probe_radiance(probe, gi_fallback_probe_index, normal);
							vec3 specular_radiance = sample_probe_specular(
								probe, reflection_direction, roughness);
							vec3 indirect_diffuse = irradiance * color * environment_diffuse_weight;
							vec3 indirect_specular = specular_radiance
								* (environment_fresnel * environment_brdf.x + environment_brdf.y);
							final_color.xyz += (indirect_diffuse + indirect_specular)
								* gi_intensity;
						}
					}
				}
				else
				{
					vec3 irradiance;
					if (gi_blend_cell(cell_index, false, position, normal, reflection_direction, roughness, irradiance))
					{
						vec3 specular_radiance = vec3(0.0);
						if (specular_cell_index >= 0)
						{
							gi_blend_cell(specular_cell_index, true, position, normal,
								reflection_direction, roughness, specular_radiance);
						}
						vec3 indirect_diffuse = irradiance * color * environment_diffuse_weight;
						vec3 indirect_specular = specular_radiance
							* (environment_fresnel * environment_brdf.x + environment_brdf.y);
						final_color.xyz += (indirect_diffuse + indirect_specular)
							* gi_intensity;
					}
				}
			}

			final_color *= ambient_occlusion;
		}
	}

	frag_color = final_color;
}
