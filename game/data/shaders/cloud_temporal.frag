#version 450

#include "shader_common.h"
#include "cloud_common.h"

layout(set = 1, binding = 1) uniform sampler2D current_cloud_tex;
layout(set = 1, binding = 2) uniform sampler2D current_depth_tex;
layout(set = 1, binding = 3) uniform sampler2D history_cloud_tex;
layout(set = 1, binding = 4) uniform sampler2D history_depth_tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_cloud;
layout(location = 1) out vec4 out_depth;

void main()
{
	vec4 current = texture(current_cloud_tex, uv);
	vec4 current_depth = texture(current_depth_tex, uv);
	if (cloud.sun_color_history.w < 0.5 || current_depth.x <= 0.0)
	{
		out_cloud = current;
		out_depth = current_depth;
		return;
	}

	vec4 clip = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
	vec4 world_h = per_frame.inv_view_projection * clip;
	vec3 ray_direction = normalize(world_h.xyz / world_h.w - per_frame.camera_position.xyz);
	vec3 world_position = per_frame.camera_position.xyz + ray_direction * current_depth.x;
	// Density samples add wind*time to their procedural coordinates, so the
	// visible pattern travels against that vector. The same feature therefore
	// lived one positive wind delta away in the previous frame.
	world_position.xy += cloud.wind_weather.xy * cloud.wind_weather.z
		* current_depth.z * cloud.planet_center_time.w;
	vec4 previous_clip = cloud.previous_view_projection * vec4(world_position, 1.0);
	vec2 history_uv = vec2(previous_clip.x / previous_clip.w * 0.5 + 0.5,
		0.5 - previous_clip.y / previous_clip.w * 0.5);
	if (previous_clip.w <= 0.0 || any(lessThan(history_uv, vec2(0.0)))
		|| any(greaterThan(history_uv, vec2(1.0))))
	{
		out_cloud = current;
		out_depth = current_depth;
		return;
	}

	vec4 history = texture(history_cloud_tex, history_uv);
	vec4 history_depth = texture(history_depth_tex, history_uv);
	vec2 texel = 1.0 / vec2(textureSize(current_cloud_tex, 0));
	vec4 neighborhood_min = current;
	vec4 neighborhood_max = current;
	vec4 neighborhood_sum = current;
	vec4 neighborhood_sum_squared = current * current;
	float neighborhood_count = 1.0;
	bool current_has_geometry = current_depth.w >= 0.5;
	for (int y = -1; y <= 1; ++y)
	for (int x = -1; x <= 1; ++x)
	{
		if (x == 0 && y == 0) continue;
		vec2 sample_uv = uv + vec2(x, y) * texel;
		vec4 sample_depth = texture(current_depth_tex, sample_uv);
		// Keep variance clipping on the same side of the opaque silhouette.
		// Mixing sky and ground neighborhoods makes a stable cloud history pulse
		// as the rasterized geometry edge changes subpixel coverage.
		if ((sample_depth.w >= 0.5) != current_has_geometry) continue;
		vec4 sample_value = texture(current_cloud_tex, sample_uv);
		neighborhood_min = min(neighborhood_min, sample_value);
		neighborhood_max = max(neighborhood_max, sample_value);
		neighborhood_sum += sample_value;
		neighborhood_sum_squared += sample_value * sample_value;
		neighborhood_count += 1.0;
	}
	vec4 neighborhood_mean = neighborhood_sum / neighborhood_count;
	vec4 neighborhood_variance = max(
		neighborhood_sum_squared / neighborhood_count - neighborhood_mean * neighborhood_mean,
		vec4(0.0));
	vec4 neighborhood_sigma = sqrt(neighborhood_variance);
	float history_clip_sigma = cloud.shadow_extent_misc.z;
	vec4 clip_padding = vec4(0.004, 0.004, 0.004, 0.002);
	vec4 clip_min = max(neighborhood_min,
		neighborhood_mean - neighborhood_sigma * history_clip_sigma - clip_padding);
	vec4 clip_max = min(neighborhood_max,
		neighborhood_mean + neighborhood_sigma * history_clip_sigma + clip_padding);
	history = clamp(history, clip_min, clip_max);
	float depth_error = abs(history_depth.x - current_depth.x) / max(current_depth.x, 1.0);
	float opacity_error = abs(history.a - current.a);
	float opacity_rejection = cloud.shadow_extent_misc.w;
	float opacity_confidence = 1.0 - smoothstep(
		opacity_rejection * 0.25, opacity_rejection, opacity_error);
	bool history_has_geometry = history_depth.w >= 0.5;
	float history_weight = depth_error < cloud.temporal_quality.y
		&& history_has_geometry == current_has_geometry
		&& history_depth.x > 0.0
		? cloud.temporal_quality.x * opacity_confidence : 0.0;
	out_cloud = mix(current, history, history_weight);
	out_depth = vec4(
		mix(current_depth.xyz, history_depth.xyz, history_weight), current_depth.w);
}
