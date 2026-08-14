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
	world_position.xy -= cloud.wind_weather.xy * cloud.wind_weather.z * cloud.planet_center_time.w;
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
	for (int y = -1; y <= 1; ++y)
	for (int x = -1; x <= 1; ++x)
	{
		vec4 sample_value = texture(current_cloud_tex, uv + vec2(x, y) * texel);
		neighborhood_min = min(neighborhood_min, sample_value);
		neighborhood_max = max(neighborhood_max, sample_value);
	}
	history = clamp(history, neighborhood_min, neighborhood_max);
	float depth_error = abs(history_depth.x - current_depth.x) / max(current_depth.x, 1.0);
	float history_weight = depth_error < cloud.temporal_quality.y
		? cloud.temporal_quality.x : 0.0;
	out_cloud = mix(current, history, history_weight);
	out_depth = mix(current_depth, history_depth, history_weight);
}
