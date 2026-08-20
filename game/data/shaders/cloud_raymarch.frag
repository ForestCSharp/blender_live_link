#version 450

#include "shader_common.h"
#include "cloud_common.h"
#define BRUNETON_DESCRIPTOR_SET 2
#include "bruneton_parameters.h"

layout(set = 1, binding = 1) uniform sampler2DArray base_shape_tex;
layout(set = 1, binding = 2) uniform sampler2DArray erosion_tex;
layout(set = 1, binding = 3) uniform sampler2DArray weather_tex;
layout(set = 1, binding = 4) uniform sampler2D position_tex;
layout(set = 2, binding = 1) uniform sampler2D atmosphere_transmittance_tex;
layout(set = 2, binding = 8) uniform sampler2D atmosphere_irradiance_tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_scattering_transmittance;
layout(location = 1) out vec4 out_depth_reactive;

float sample_volume(sampler2DArray tex, vec3 uvw, float slices)
{
	uvw = fract(uvw);
	float z = uvw.z * slices - 0.5;
	float z0 = floor(z);
	float blend = fract(z);
	float a = texture(tex, vec3(uvw.xy, mod(z0, slices))).r;
	float b = texture(tex, vec3(uvw.xy, mod(z0 + 1.0, slices))).r;
	return mix(a, b, blend);
}

float cloud_density_at(
	vec3 world_position, int layer_index, bool detailed, out float coarse_density)
{
	coarse_density = 0.0;
	CloudLayerGpu layer = cloud.layers[layer_index];
	float altitude = layer.altitude_thickness_coverage_density.x;
	float thickness = layer.altitude_thickness_coverage_density.y;
	float ground_radius = max(abs(cloud.planet_center_time.x), 1000.0);
	float height_fraction = (length(world_position - vec3(0.0, 0.0, cloud.planet_center_time.x))
		- ground_radius - altitude) / max(thickness, 1.0);
	if (height_fraction <= 0.0 || height_fraction >= 1.0) return 0.0;

	vec2 wind = cloud.wind_weather.xy * cloud.wind_weather.z * cloud.planet_center_time.y
		* layer.wind_phase.x;
	wind += cloud.wind_weather.xy * height_fraction * 500.0;
	float layer_seed = layer.ambient_multi_profile_seed.w;
	vec2 seed_offset = (fract(vec2(layer_seed * 0.6180339,
		layer_seed * 0.4142136)) - 0.5) * cloud.wind_weather.w;
	vec2 weather_uv = fract((world_position.xy + wind + seed_offset) / cloud.wind_weather.w);
	vec2 weather = texture(weather_tex, vec3(weather_uv, float(layer_index))).rg;
	float coverage = clamp(layer.altitude_thickness_coverage_density.z
		+ (weather.r - 0.5) * 0.55, 0.0, 1.0);
	float profile = cloud_height_profile(height_fraction,
		int(layer.ambient_multi_profile_seed.z + 0.5), layer.scales_erosion_anvil.w);

	vec3 shape_coord = vec3(world_position.xy + wind + seed_offset, world_position.z)
		/ max(layer.scales_erosion_anvil.x, 100.0);
	float shape = sample_volume(base_shape_tex, shape_coord, 128.0);
	shape = cloud_remap(shape * profile, 1.0 - coverage, 1.0, 0.0, 1.0);
	if (shape <= 0.0) return 0.0;
	coarse_density = shape * layer.altitude_thickness_coverage_density.w;
	if (coarse_density <= cloud.temporal_quality.w) return 0.0;
	if (!detailed) return coarse_density;

	vec3 detail_coord = vec3(world_position.xy + wind * 1.13 + seed_offset, world_position.z)
		/ max(layer.scales_erosion_anvil.y, 10.0);
	float detail = sample_volume(erosion_tex, detail_coord, 32.0);
	float eroded = max(cloud_remap(
		shape, detail * layer.scales_erosion_anvil.z, 1.0, 0.0, 1.0), 0.0);
	// Suppress isolated sub-voxel remnants without imposing another hard
	// cutoff. This keeps wisps while making their extinction approach zero
	// smoothly at heavily eroded silhouettes.
	float edge_fade = cloud.temporal_quality.z;
	float edge_weight = edge_fade > 1.0e-5
		? smoothstep(0.0, edge_fade, eroded) : 1.0;
	return eroded * edge_weight * layer.altitude_thickness_coverage_density.w;
}

float cloud_density_at(vec3 world_position, int layer_index, bool detailed)
{
	float ignored_coarse_density;
	return cloud_density_at(world_position, layer_index, detailed, ignored_coarse_density);
}

void main()
{
	vec4 clip = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
	vec4 world_h = per_frame.inv_view_projection * clip;
	vec3 ray_direction = normalize(world_h.xyz / world_h.w - per_frame.camera_position.xyz);
	vec3 ray_origin = per_frame.camera_position.xyz;
	vec3 planet_center = vec3(0.0, 0.0, cloud.planet_center_time.x);
	float ground_radius = max(abs(cloud.planet_center_time.x), 1000.0);
	float camera_radius = length(ray_origin - planet_center);
	vec3 radial_up = normalize(ray_origin - planet_center);
	float horizon_cosine = -sqrt(max(1.0 - ground_radius * ground_radius
		/ max(camera_radius * camera_radius, ground_radius * ground_radius), 0.0));
	if (dot(ray_direction, radial_up) < horizon_cosine)
	{
		out_scattering_transmittance = vec4(0.0, 0.0, 0.0, 1.0);
		out_depth_reactive = vec4(0.0);
		return;
	}
	// Position is a categorical geometry/sky boundary as well as a world-space
	// value. Never filter it across silhouettes: a blended ground/sky position
	// produces an unstable false ray limit exactly where clouds meet terrain.
	ivec2 position_size = textureSize(position_tex, 0);
	ivec2 position_pixel = clamp(
		ivec2(uv * vec2(position_size)), ivec2(0), position_size - ivec2(1));
	vec4 scene_position = texelFetch(position_tex, position_pixel, 0);
	float geometry_coverage = scene_position.w > 0.5 ? 1.0 : 0.0;
	float geometry_distance = geometry_coverage > 0.5
		? length(scene_position.xyz - ray_origin) : 1.0e8;
	vec3 sun_direction = normalize(cloud.sun_direction_layer_count.xyz);
	int layer_count = clamp(int(cloud.sun_direction_layer_count.w + 0.5), 0, MAX_CLOUD_LAYERS);

	vec3 integrated = vec3(0.0);
	float transmittance = 1.0;
	float weighted_depth = 0.0;
	float depth_weight = 0.0;
	float fallback_depth = 0.0;
	float weighted_wind_multiplier = 0.0;
	float fallback_wind_multiplier = 1.0;
	float cos_angle = dot(sun_direction, -ray_direction);
	const int MAX_STEPS_PER_LAYER = 64;
	uint frame_index = uint(max(cloud.planet_center_time.z, 0.0));

	for (int layer_index = 0; layer_index < MAX_CLOUD_LAYERS; ++layer_index)
	{
		if (layer_index >= layer_count || transmittance < 0.01) break;
		CloudLayerGpu layer = cloud.layers[layer_index];
		if (layer.altitude_thickness_coverage_density.w <= 0.0) continue;
		float inner_radius = ground_radius + layer.altitude_thickness_coverage_density.x;
		float outer_radius = inner_radius + layer.altitude_thickness_coverage_density.y;
		vec2 outer = cloud_sphere_interval(ray_origin, ray_direction, planet_center, outer_radius);
		vec2 inner = cloud_sphere_interval(ray_origin, ray_direction, planet_center, inner_radius);
		bool geometry_clips_layer = geometry_coverage > 0.5 && geometry_distance < outer.y;
		float start_distance = max(outer.x, 0.0);
		if (length(ray_origin - planet_center) > inner_radius && inner.x > start_distance)
			start_distance = inner.x;
		float end_distance = min(outer.y, geometry_distance);
		if (end_distance <= start_distance) continue;
		if (fallback_depth <= 0.0)
		{
			fallback_depth = mix(start_distance, end_distance, 0.5);
			fallback_wind_multiplier = layer.wind_phase.x;
		}

		float view_steps = clamp(cloud.march_quality.x, 12.0, 48.0);
		float dense_step_scale = clamp(cloud.march_quality.y, 0.5, 1.0);
		float empty_step_scale = clamp(cloud.march_quality.z, 1.0, 4.0);
		int sun_cone_samples = clamp(int(cloud.march_quality.w + 0.5), 1, 8);
		vec2 jitter_pixel = gl_FragCoord.xy + vec2(float(layer_index) * 17.0,
			float(layer_index) * 31.0);
		// A geometry-clipped interval changes discontinuously at the rasterized
		// silhouette. Keep its spatial jitter but freeze the temporal phase so the
		// final cloud sample does not sparkle against an otherwise stable edge.
		uint jitter_frame = geometry_clips_layer ? 0u : frame_index;
		float view_jitter = cloud_low_discrepancy_jitter(jitter_pixel, jitter_frame, 0u);
		float cone_jitter = cloud_low_discrepancy_jitter(jitter_pixel, jitter_frame, 1u);
		float minimum_density = cloud.temporal_quality.w;
		float step_size = (end_distance - start_distance) / view_steps;
		float distance_along_ray = start_distance + view_jitter * step_size;
		for (int step_index = 0;
			step_index < MAX_STEPS_PER_LAYER && distance_along_ray < end_distance;
			++step_index)
		{
			vec3 sample_position = ray_origin + ray_direction * distance_along_ray;
			float coarse_density = cloud_density_at(sample_position, layer_index, false);
			if (coarse_density <= minimum_density)
			{
				distance_along_ray += step_size * empty_step_scale;
				continue;
			}
			float density = cloud_density_at(sample_position, layer_index, true);
			if (density <= minimum_density)
			{
				distance_along_ray += step_size;
				continue;
			}

			float light_density = 0.0;
			float light_step = max(layer.altitude_thickness_coverage_density.y * 0.08, 80.0);
			for (int light_index = 0; light_index < 8; ++light_index)
			{
				if (light_index >= sun_cone_samples) break;
				float cone = float(light_index + 1);
				vec3 cone_offset = vec3(
					fract(cone_jitter * 7.13 + cone) - 0.5,
					fract(cone_jitter * 11.71 + cone * 0.37) - 0.5,
					fract(cone_jitter * 5.37 + cone * 0.61803398875) - 0.5)
					* light_step * cone * 0.18;
				light_density += cloud_density_at(sample_position + sun_direction * light_step * cone + cone_offset,
					layer_index, light_index < 3);
			}
			float light_transmittance = exp(-light_density * light_step * 0.0012);
			float phase = mix(cloud_hg(cos_angle, layer.wind_phase.z),
				cloud_hg(cos_angle, layer.wind_phase.y), layer.wind_phase.w);
			float multiple = mix(1.0, max(light_transmittance,
				exp(-light_density * light_step * 0.0003) * 0.7),
				layer.ambient_multi_profile_seed.y);

			AtmosphereParameters atmosphere = GetAtmosphere();
			vec3 atmosphere_position = GetAtmosphereCameraPosition(
				atmosphere, sample_position, cloud.planet_center_time.x);
			vec3 sun_transmittance = GetAtmosphereSunTransmittance(
				atmosphere, atmosphere_transmittance_tex, atmosphere_position, sun_direction);
			vec3 direct = cloud.sun_color_history.rgb * sun_transmittance
				* light_transmittance * multiple * phase * 8.0;
			float sample_height_fraction = clamp((length(sample_position - planet_center)
				- ground_radius - layer.altitude_thickness_coverage_density.x)
				/ max(layer.altitude_thickness_coverage_density.y, 1.0), 0.0, 1.0);
			float atmosphere_radius = length(atmosphere_position);
			float atmosphere_mu_s = dot(atmosphere_position, sun_direction)
				/ max(atmosphere_radius, 1.0e-5);
			vec3 sky_irradiance = GetIrradiance(atmosphere,
				atmosphere_irradiance_tex, atmosphere_radius, atmosphere_mu_s);
			vec3 sun_tint_scale = cloud.sun_color_history.rgb
				/ max(atmosphere.solar_irradiance, vec3(1.0e-4));
			vec3 ambient = sky_irradiance * sun_tint_scale
				* layer.ambient_multi_profile_seed.x * mix(0.3, 0.7, sample_height_fraction);
			vec3 luminance = direct + ambient;

			// Do not integrate the last stochastic sample beyond the opaque surface.
			// This matters most at long ground-plane intersections where one nominal
			// cloud step can cover hundreds of metres.
			float integration_step = min(step_size, end_distance - distance_along_ray);
			float extinction = max(density * 0.0018, 1.0e-7);
			float step_transmittance = exp(-extinction * integration_step);
			vec3 integrated_step = luminance * density
				* (1.0 - step_transmittance) / extinction;
			float opacity_contribution = transmittance * (1.0 - step_transmittance);
			integrated += transmittance * integrated_step * 0.0018;
			weighted_depth += distance_along_ray * opacity_contribution;
			weighted_wind_multiplier += layer.wind_phase.x * opacity_contribution;
			depth_weight += opacity_contribution;
			transmittance *= step_transmittance;
			if (transmittance < 0.01) break;
			distance_along_ray += step_size * dense_step_scale;
		}
	}

	// Keep a stable shell depth even when this frame's jittered samples miss a
	// wispy edge. Temporal reprojection can then accumulate the cloud/clear
	// transition instead of dropping history for one frame.
	float mean_depth = depth_weight > 1.0e-5
		? weighted_depth / depth_weight : fallback_depth;
	float effective_wind_multiplier = depth_weight > 1.0e-5
		? weighted_wind_multiplier / depth_weight : fallback_wind_multiplier;
	out_scattering_transmittance = vec4(SanitizeSceneColor(integrated), clamp(transmittance, 0.0, 1.0));
	out_depth_reactive = vec4(mean_depth, 1.0 - transmittance,
		effective_wind_multiplier, geometry_coverage);
}
