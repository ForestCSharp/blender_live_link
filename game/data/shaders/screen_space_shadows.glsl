// Common Shader Code
#include "shader_common.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.h"

@fs trace_fs

layout(binding=0) uniform texture2D position_tex;
layout(binding=1) uniform texture2D normal_tex;

layout(binding=0) uniform sampler tex_sampler;

layout(binding=0) uniform trace_fs_params {
	vec2 screen_size;
	mat4 view;
	mat4 projection;
	vec3 light_direction;
	float ray_length;
	float thickness;
	int max_steps;
	int enable;
};

in vec2 uv;

out vec4 frag_color;

vec2 project_to_uv(vec3 in_view_position)
{
	vec4 clip_position = projection * vec4(in_view_position, 1.0);
	if (clip_position.w <= 0.0)
	{
		return vec2(-1.0);
	}

	vec3 ndc = clip_position.xyz / clip_position.w;
	vec2 out_uv = ndc.xy * 0.5 + 0.5;
	out_uv.y = 1.0 - out_uv.y;
	return out_uv;
}

void main()
{
	if (enable == 0 || max_steps <= 0 || ray_length <= 0.0)
	{
		frag_color = vec4(1.0);
		return;
	}

	vec4 sampled_position = texture(sampler2D(position_tex, tex_sampler), uv);
	vec4 sampled_normal = texture(sampler2D(normal_tex, tex_sampler), uv);
	if (sampled_normal == vec4(0.0))
	{
		frag_color = vec4(1.0);
		return;
	}

	vec3 world_position = sampled_position.xyz;
	vec3 world_normal = normalize(sampled_normal.xyz);
	vec3 surface_to_light_world = normalize(-light_direction);

	if (dot(world_normal, surface_to_light_world) <= 0.0)
	{
		frag_color = vec4(1.0);
		return;
	}

	vec3 view_start = (view * vec4(world_position + world_normal * 0.02, 1.0)).xyz;
	vec3 view_end = (view * vec4(world_position + surface_to_light_world * ray_length, 1.0)).xyz;

	float visibility = 1.0;
	for (int i = 1; i <= max_steps; ++i)
	{
		float ray_alpha = float(i) / float(max_steps);
		vec3 view_ray_position = mix(view_start, view_end, ray_alpha);
		vec2 sample_uv = project_to_uv(view_ray_position);
		if (sample_uv.x < 0.0 || sample_uv.x > 1.0 || sample_uv.y < 0.0 || sample_uv.y > 1.0)
		{
			break;
		}

		vec4 blocker_world_position = texture(sampler2D(position_tex, tex_sampler), sample_uv);
		vec4 blocker_normal = texture(sampler2D(normal_tex, tex_sampler), sample_uv);
		if (blocker_normal == vec4(0.0))
		{
			continue;
		}

		vec3 blocker_view_position = (view * vec4(blocker_world_position.xyz, 1.0)).xyz;
		float camera_depth_delta = blocker_view_position.z - view_ray_position.z;
		float effective_thickness = max(thickness, abs(view_ray_position.z) * 0.002);
		if (camera_depth_delta > 0.0 && camera_depth_delta < effective_thickness)
		{
			visibility = 0.0;
			break;
		}
	}

	frag_color = vec4(vec3(visibility), 1.0);
}

@end

@fs filter_fs

layout(binding=0) uniform texture2D raw_shadow_tex;
layout(binding=1) uniform texture2D filter_position_tex;
layout(binding=2) uniform texture2D filter_normal_tex;

layout(binding=0) uniform sampler tex_sampler;

layout(binding=0) uniform filter_fs_params {
	vec2 screen_size;
	int filter_radius;
};

in vec2 uv;

out vec4 frag_color;

void main()
{
	vec4 center_position = texture(sampler2D(filter_position_tex, tex_sampler), uv);
	vec4 center_normal = texture(sampler2D(filter_normal_tex, tex_sampler), uv);
	if (center_normal == vec4(0.0))
	{
		frag_color = vec4(1.0);
		return;
	}

	vec2 texel_size = 1.0 / screen_size;
	vec3 normal = normalize(center_normal.xyz);
	float total_visibility = 0.0;
	float total_weight = 0.0;
	int radius = clamp(filter_radius, 0, 2);

	for (int y = -2; y <= 2; ++y)
	{
		for (int x = -2; x <= 2; ++x)
		{
			if (abs(x) > radius || abs(y) > radius)
			{
				continue;
			}

			vec2 sample_uv = uv + vec2(float(x), float(y)) * texel_size;
			vec4 sample_position = texture(sampler2D(filter_position_tex, tex_sampler), sample_uv);
			vec4 sample_normal = texture(sampler2D(filter_normal_tex, tex_sampler), sample_uv);
			float visibility = texture(sampler2D(raw_shadow_tex, tex_sampler), sample_uv).r;

			float spatial_weight = exp(-0.5 * float(x * x + y * y));
			float normal_weight = sample_normal == vec4(0.0)
				? 0.0
				: pow(max(dot(normal, normalize(sample_normal.xyz)), 0.0), 16.0);
			float position_weight = exp(-length(sample_position.xyz - center_position.xyz) * 4.0);
			float weight = spatial_weight * normal_weight * position_weight;

			total_visibility += visibility * weight;
			total_weight += weight;
		}
	}

	float filtered_visibility = total_weight > 0.0
		? total_visibility / total_weight
		: texture(sampler2D(raw_shadow_tex, tex_sampler), uv).r;
	frag_color = vec4(vec3(filtered_visibility), 1.0);
}

@end

@program trace vs trace_fs
@program filter vs filter_fs
