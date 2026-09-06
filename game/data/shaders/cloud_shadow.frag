#version 450

#include "shader_common.h"
#include "cloud_common.h"

layout(set = 1, binding = 1) uniform sampler3D base_shape_tex;
layout(set = 1, binding = 2) uniform sampler3D erosion_tex;
layout(set = 1, binding = 3) uniform sampler2DArray weather_tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_transmittance;

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	int phase = int(cloud.planet_center_time.z) & 3;
	if (((pixel.x & 1) | ((pixel.y & 1) << 1)) != phase) discard;

	float extent = max(cloud.shadow_extent_misc.x, 1000.0);
	vec2 world_xy = per_frame.camera_position.xy + (uv - 0.5) * extent;
	vec3 sun_direction = normalize(cloud.sun_direction_layer_count.xyz);
	if (sun_direction.z <= 0.01)
	{
		out_transmittance = vec4(1.0);
		return;
	}
	int layer_count = clamp(int(cloud.sun_direction_layer_count.w + 0.5), 0, MAX_CLOUD_LAYERS);
	float optical_depth = 0.0;
	for (int layer_index = 0; layer_index < MAX_CLOUD_LAYERS; ++layer_index)
	{
		if (layer_index >= layer_count) break;
		CloudLayerGpu layer = cloud.layers[layer_index];
		float thickness = layer.altitude_thickness_coverage_density.y;
		for (int sample_index = 0; sample_index < 8; ++sample_index)
		{
			float fraction = (float(sample_index) + 0.5) / 8.0;
			float altitude = layer.altitude_thickness_coverage_density.x + fraction * thickness;
			vec3 world_position = vec3(world_xy, 0.0)
				+ sun_direction * altitude / max(sun_direction.z, 0.08);
			vec2 wind = cloud.wind_weather.xy * cloud.wind_weather.z * cloud.planet_center_time.y
				* layer.wind_phase.x;
			float layer_seed = layer.ambient_multi_profile_seed.w;
			vec2 seed_offset = (fract(vec2(layer_seed * 0.6180339,
				layer_seed * 0.4142136)) - 0.5) * cloud.wind_weather.w;
			vec2 weather_uv = fract((world_position.xy + wind + seed_offset) / cloud.wind_weather.w);
			float weather = textureLod(weather_tex, vec3(weather_uv, float(layer_index)), 0.0).r;
			float coverage = clamp(layer.altitude_thickness_coverage_density.z
				+ (weather - 0.5) * 0.55, 0.0, 1.0);
			float profile = cloud_height_profile(fraction,
				int(layer.ambient_multi_profile_seed.z + 0.5), layer.scales_erosion_anvil.w);
			// LOD 0: a fixed 512^2 map over a fixed extent has no view-dependent
			// footprint, and it accumulates across four frames, so the level it
			// samples must not shift underneath itself.
			float shape = textureLod(base_shape_tex,
				vec3(world_position.xy + wind + seed_offset, world_position.z)
				/ max(layer.scales_erosion_anvil.x, 100.0), 0.0).r;
			float coarse_density = cloud_remap(
				shape * profile, 1.0 - coverage, 1.0, 0.0, 1.0);
			float density = 0.0;
			if (coarse_density > 0.0)
			{
				vec3 detail_coord = vec3(
					world_position.xy + wind * 1.13 + seed_offset, world_position.z)
					/ max(layer.scales_erosion_anvil.y, 10.0);
				float detail = textureLod(erosion_tex, detail_coord, 0.0).r;
				density = cloud_remap(coarse_density,
					detail * layer.scales_erosion_anvil.z, 1.0, 0.0, 1.0)
					* layer.altitude_thickness_coverage_density.w;
			}
			optical_depth += max(density, 0.0) * thickness / 8.0 * 0.0018;
		}
	}
	float edge = smoothstep(0.0, 0.1, min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y)));
	float transmittance = mix(1.0, exp(-optical_depth), edge);
	out_transmittance = vec4(transmittance, transmittance, transmittance, 1.0);
}
