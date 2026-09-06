#version 450

#include "shader_common.h"
#include "cloud_common.h"
#define BRUNETON_DESCRIPTOR_SET 2
#include "bruneton_parameters.h"

layout(set = 1, binding = 1) uniform sampler3D base_shape_tex;
layout(set = 1, binding = 2) uniform sampler3D erosion_tex;
layout(set = 1, binding = 3) uniform sampler2DArray weather_tex;
layout(set = 1, binding = 4) uniform sampler2D position_tex;
layout(set = 2, binding = 1) uniform sampler2D atmosphere_transmittance_tex;
layout(set = 2, binding = 8) uniform sampler2D atmosphere_irradiance_tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_scattering_transmittance;
layout(location = 1) out vec4 out_depth_reactive;

// Selects a noise mip from the world-space footprint a sample represents.
// One voxel spans scale/size metres because the volume tiles once per scale.
float cloud_noise_lod(float footprint_m, float scale_m, float texel_count)
{
	float texel_m = scale_m / texel_count;
	return clamp(log2(max(footprint_m / texel_m, 1.0)), 0.0, cloud.lod_params.z);
}

// `footprint_m` is the world-space extent this sample stands for. Passing it in
// rather than relying on implicit derivatives is what lets grazing rays, whose
// steps and pixel cones are orders of magnitude wider than at zenith, filter the
// noise instead of point-sampling it.
float cloud_density_at(
	vec3 world_position, int layer_index, bool detailed, float footprint_m,
	out float coarse_density)
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
	vec2 weather = textureLod(weather_tex, vec3(weather_uv, float(layer_index)), 0.0).rg;
	float coverage = clamp(layer.altitude_thickness_coverage_density.z
		+ (weather.r - 0.5) * 0.55, 0.0, 1.0);
	float profile = cloud_height_profile(height_fraction,
		int(layer.ambient_multi_profile_seed.z + 0.5), layer.scales_erosion_anvil.w);

	vec3 shape_coord = vec3(world_position.xy + wind + seed_offset, world_position.z)
		/ max(layer.scales_erosion_anvil.x, 100.0);
	float shape = textureLod(base_shape_tex, shape_coord,
		cloud_noise_lod(footprint_m, max(layer.scales_erosion_anvil.x, 100.0), 128.0)).r;
	shape = cloud_remap(shape * profile, 1.0 - coverage, 1.0, 0.0, 1.0);
	if (shape <= 0.0) return 0.0;
	coarse_density = shape * layer.altitude_thickness_coverage_density.w;
	if (coarse_density <= cloud.temporal_quality.w) return 0.0;
	if (!detailed) return coarse_density;

	vec3 detail_coord = vec3(world_position.xy + wind * 1.13 + seed_offset, world_position.z)
		/ max(layer.scales_erosion_anvil.y, 10.0);
	float detail = textureLod(erosion_tex, detail_coord,
		cloud_noise_lod(footprint_m, max(layer.scales_erosion_anvil.y, 10.0), 32.0)).r;
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

float cloud_density_at(
	vec3 world_position, int layer_index, bool detailed, float footprint_m)
{
	float ignored_coarse_density;
	return cloud_density_at(
		world_position, layer_index, detailed, footprint_m, ignored_coarse_density);
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
	// Below ~5 degrees of elevation the shell chord grows past 50 km and no
	// affordable sample count can resolve the noise, so fade the layer out
	// there instead. Real clouds dissolve into aerial perspective at the same
	// place. The old hard cull at the geometric horizon becomes this fade's
	// tail rather than a binary step.
	float elevation_sine = dot(ray_direction, radial_up);
	float horizon_fade = smoothstep(
		max(cloud.march_limits.w, horizon_cosine), cloud.march_limits.z, elevation_sine);
	if (elevation_sine < horizon_cosine || horizon_fade <= 0.0)
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
	// Freezing the jitter phase stabilises rasterized silhouettes, but doing it
	// for every geometry-clipped ray freezes most of the frame near the horizon,
	// converting dither that TAA could average into permanent fixed-pattern
	// grain. Restrict it to pixels actually touching the silhouette.
	bool silhouette_adjacent = false;
	for (int sy = -1; sy <= 1 && !silhouette_adjacent; ++sy)
	{
		for (int sx = -1; sx <= 1; ++sx)
		{
			ivec2 neighbor = clamp(position_pixel + ivec2(sx, sy),
				ivec2(0), position_size - ivec2(1));
			if ((texelFetch(position_tex, neighbor, 0).w > 0.5) != (geometry_coverage > 0.5))
			{
				silhouette_adjacent = true;
				break;
			}
		}
	}
	vec3 sun_direction = normalize(cloud.sun_direction_layer_count.xyz);
	int layer_count = clamp(int(cloud.sun_direction_layer_count.w + 0.5), 0, MAX_CLOUD_LAYERS);

	vec3 integrated = vec3(0.0);
	float transmittance = 1.0;
	float weighted_depth = 0.0;
	float weighted_depth_squared = 0.0;
	float depth_weight = 0.0;
	float fallback_depth = 0.0;
	float weighted_wind_multiplier = 0.0;
	float fallback_wind_multiplier = 1.0;
	float cos_angle = dot(sun_direction, -ray_direction);
	// Was 64, which truncated the march at ~2/3 of the interval for any
	// view_steps/dense_step_scale combination needing more - so raising the
	// quality sliders used to make the horizon worse. The absolute step clamp
	// needs far more headroom than that.
	const int MAX_STEPS_PER_LAYER = 256;
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
		// Bounding total march length is what keeps the absolute step clamp
		// below affordable: without it a grazing ray would need hundreds of
		// steps to cross a 250 km chord.
		float end_distance = min(min(outer.y, geometry_distance),
			start_distance + cloud.march_limits.y);
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
		uint jitter_frame = (geometry_clips_layer && silhouette_adjacent) ? 0u : frame_index;
		float view_jitter = cloud_low_discrepancy_jitter(jitter_pixel, jitter_frame, 0u);
		float cone_jitter = cloud_low_discrepancy_jitter(jitter_pixel, jitter_frame, 1u);
		float minimum_density = cloud.temporal_quality.w;
		// Step length has to be a property of the medium, not of the interval.
		// A grazing ray crosses ~250 km of shell versus ~5 km looking up, so
		// dividing by a fixed count gives kilometre steps against a 1 km
		// erosion feature. The ceiling is expressed in units of
		// thickness/view_steps, so it only engages once the chord exceeds
		// thickness * scale and leaves near-vertical rays alone.
		float nominal_step = (end_distance - start_distance) / view_steps;
		float max_step = layer.altitude_thickness_coverage_density.y / view_steps
			* cloud.march_limits.x;
		float step_size = min(nominal_step, max_step);
		float distance_along_ray = start_distance + view_jitter * step_size;
		for (int step_index = 0;
			step_index < MAX_STEPS_PER_LAYER && distance_along_ray < end_distance;
			++step_index)
		{
			vec3 sample_position = ray_origin + ray_direction * distance_along_ray;
			// The pixel cone widens with distance; the step term matters because
			// a step integrating kilometres of atmosphere must not point-sample
			// a 60 m voxel. Weighted rather than raw, so a grazing ray does not
			// collapse straight to the volume mean.
			float view_footprint = max(distance_along_ray * cloud.lod_params.x,
				step_size * cloud.lod_params.y);
			float coarse_density = cloud_density_at(
				sample_position, layer_index, false, view_footprint);
			if (coarse_density <= minimum_density)
			{
				distance_along_ray += step_size * empty_step_scale;
				continue;
			}
			float density = cloud_density_at(
				sample_position, layer_index, true, view_footprint) * horizon_fade;
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
				// These taps are off the primary ray, spread across a jitter box
				// of full width light_step * cone * 0.36. Filtering each over the
				// footprint it actually represents is variance reduction on what
				// is already a Monte Carlo estimate.
				float cone_footprint = max(view_footprint, light_step * cone * 0.36);
				light_density += cloud_density_at(
					sample_position + sun_direction * light_step * cone + cone_offset,
					layer_index, light_index < 3, cone_footprint);
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
			weighted_depth_squared +=
				distance_along_ray * distance_along_ray * opacity_contribution;
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
	// How far the contributing volume is spread along the ray. At zenith this is
	// a couple of km and the mean is a good reprojection anchor; near the
	// horizon it is tens of km and the temporal pass has to widen its tolerance
	// accordingly instead of rejecting history outright.
	float depth_spread = depth_weight > 1.0e-5
		? sqrt(max(weighted_depth_squared / depth_weight - mean_depth * mean_depth, 0.0))
		: 0.0;
	out_scattering_transmittance = vec4(SanitizeSceneColor(integrated), clamp(transmittance, 0.0, 1.0));
	// Depth travels in kilometres. This target is fp16, which tops out at 65504,
	// and mean cloud depth near the horizon reaches ~80 km - in metres that
	// overflows to +Inf, which then turns the reprojection UV into NaN.
	// The y channel previously held (1 - transmittance) and was read by nobody.
	out_depth_reactive = vec4(mean_depth * CLOUD_DEPTH_TO_STORAGE,
		depth_spread * CLOUD_DEPTH_TO_STORAGE,
		effective_wind_multiplier, geometry_coverage);
}
