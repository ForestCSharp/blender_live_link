// Common Shader Code
#include "shader_common.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.h"

@fs fs

layout(binding=0) uniform fs_params {
	mat4 previous_view_projection;
	vec2 screen_size;
	vec2 sharpen_axis;
	float blend_alpha;
	float sharpen_strength;
	float rejection_threshold;
	int history_valid;
	int debug_mode;
};

layout(binding=0) uniform texture2D current_color_tex;
layout(binding=1) uniform texture2D position_tex;
layout(binding=2) uniform texture2D history_tex;

layout(binding=0) uniform sampler linear_sampler;
@sampler_type nearest_sampler nonfiltering
layout(binding=1) uniform sampler nearest_sampler;

in vec2 uv;

out vec4 out_resolved;
out vec4 out_history;

vec3 neighborhood_min(vec2 in_uv)
{
	vec2 texel_size = 1.0 / screen_size;
	vec3 result = texture(sampler2D(current_color_tex, linear_sampler), in_uv).rgb;
	result = min(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2(-1.0, -1.0)).rgb);
	result = min(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2( 0.0, -1.0)).rgb);
	result = min(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2( 1.0, -1.0)).rgb);
	result = min(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2(-1.0,  0.0)).rgb);
	result = min(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2( 1.0,  0.0)).rgb);
	result = min(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2(-1.0,  1.0)).rgb);
	result = min(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2( 0.0,  1.0)).rgb);
	result = min(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2( 1.0,  1.0)).rgb);
	return result;
}

vec3 neighborhood_max(vec2 in_uv)
{
	vec2 texel_size = 1.0 / screen_size;
	vec3 result = texture(sampler2D(current_color_tex, linear_sampler), in_uv).rgb;
	result = max(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2(-1.0, -1.0)).rgb);
	result = max(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2( 0.0, -1.0)).rgb);
	result = max(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2( 1.0, -1.0)).rgb);
	result = max(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2(-1.0,  0.0)).rgb);
	result = max(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2( 1.0,  0.0)).rgb);
	result = max(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2(-1.0,  1.0)).rgb);
	result = max(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2( 0.0,  1.0)).rgb);
	result = max(result, texture(sampler2D(current_color_tex, linear_sampler), in_uv + texel_size * vec2( 1.0,  1.0)).rgb);
	return result;
}

vec3 sharpen_current(vec2 in_uv, vec3 in_current_color, vec3 in_min_color, vec3 in_max_color)
{
	vec2 texel_size = 1.0 / screen_size;
	vec2 axis = sharpen_axis * texel_size;
	vec3 near_avg = 0.5 * (
		texture(sampler2D(current_color_tex, linear_sampler), in_uv - axis).rgb +
		texture(sampler2D(current_color_tex, linear_sampler), in_uv + axis).rgb
	);
	vec3 far_avg = 0.5 * (
		texture(sampler2D(current_color_tex, linear_sampler), in_uv - axis * 2.0).rgb +
		texture(sampler2D(current_color_tex, linear_sampler), in_uv + axis * 2.0).rgb
	);
	vec3 sharpened = in_current_color
		+ sharpen_strength * (in_current_color - near_avg)
		+ sharpen_strength * 0.25 * (near_avg - far_avg);
	return clamp(sharpened, in_min_color, in_max_color);
}

bool get_previous_uv(vec3 world_position, out vec2 previous_uv)
{
	vec4 previous_clip = previous_view_projection * vec4(world_position, 1.0);
	if (previous_clip.w <= 0.0)
	{
		return false;
	}

	vec3 previous_ndc = previous_clip.xyz / previous_clip.w;
	previous_uv = vec2(previous_ndc.x * 0.5 + 0.5, 0.5 - previous_ndc.y * 0.5);
	return previous_uv.x >= 0.0 && previous_uv.x <= 1.0 && previous_uv.y >= 0.0 && previous_uv.y <= 1.0;
}

void main()
{
	vec4 current_color = texture(sampler2D(current_color_tex, linear_sampler), uv);
	vec3 min_color = neighborhood_min(uv);
	vec3 max_color = neighborhood_max(uv);
	vec3 sharpened_color = sharpen_current(uv, current_color.rgb, min_color, max_color);

	vec2 previous_uv = uv;
	bool accepted_history = history_valid != 0;
	vec4 world_position = texture(sampler2D(position_tex, nearest_sampler), uv);
	accepted_history = accepted_history && world_position.w > 0.0;
	accepted_history = accepted_history && get_previous_uv(world_position.xyz, previous_uv);

	vec3 history_color = vec3(0.0);
	if (accepted_history)
	{
		history_color = texture(sampler2D(history_tex, linear_sampler), previous_uv).rgb;
		vec3 clamped_history = clamp(history_color, min_color, max_color);
		float color_delta = length(history_color - clamped_history);
		float scaled_threshold = rejection_threshold * (1.0 + length(current_color.rgb));
		accepted_history = color_delta <= scaled_threshold;
		history_color = clamped_history;
	}

	vec3 resolved_color = accepted_history
		? mix(sharpened_color, history_color, blend_alpha)
		: current_color.rgb;

	if (debug_mode == 1)
	{
		resolved_color = accepted_history ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	}
	else if (debug_mode == 2)
	{
		resolved_color = vec3(previous_uv, accepted_history ? 1.0 : 0.0);
	}

	out_resolved = vec4(resolved_color, current_color.a);
	out_history = vec4(sharpened_color, current_color.a);
}

@end

@program temporal_aa vs fs
