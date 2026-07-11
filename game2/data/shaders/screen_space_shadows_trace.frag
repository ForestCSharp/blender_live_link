#version 450

// Screen-space contact shadows: short ray march toward the sun through the
// G-buffer (port of game/data/shaders/screen_space_shadows.glsl trace_fs).
// Half render resolution; the filter pass smooths the raw mask.

layout(set = 0, binding = 0) uniform trace_fs_params
{
	vec2 screen_size;
	mat4 view;
	mat4 projection;
	vec3 light_direction;
	float ray_length;
	float thickness;
	float jitter_strength;
	int max_steps;
	int enable;
};

layout(set = 0, binding = 1) uniform sampler2D position_tex;
layout(set = 0, binding = 2) uniform sampler2D normal_tex;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

const float normal_offset_bias = 0.02;
const float same_surface_normal_threshold = 0.96;
const float edge_fade_width = 0.06;

/* Gradient noise from Jorge Jimenez's presentation: */
/* http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare */
float gradient_noise(in vec2 noise_uv)
{
	return fract(52.9829189 * fract(dot(noise_uv, vec2(0.06711056, 0.00583715))));
}

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

	vec4 sampled_position = texture(position_tex, uv);
	vec4 sampled_normal = texture(normal_tex, uv);
	if (sampled_normal == vec4(0.0))
	{
		frag_color = vec4(1.0);
		return;
	}

	vec3 world_position = sampled_position.xyz;
	vec3 world_normal = normalize(sampled_normal.xyz);
	vec3 surface_to_light_world = normalize(-light_direction);
	float normal_light = dot(world_normal, surface_to_light_world);

	if (normal_light <= 0.0)
	{
		frag_color = vec4(1.0);
		return;
	}

	vec3 ray_start_world = world_position + world_normal * normal_offset_bias;
	vec3 view_ray_position = (view * vec4(ray_start_world, 1.0)).xyz;
	vec3 view_light_direction = normalize((view * vec4(surface_to_light_world, 0.0)).xyz);
	vec3 ray_step = view_light_direction * (ray_length / float(max_steps));
	float jitter = (gradient_noise(gl_FragCoord.xy) * 2.0 - 1.0) * jitter_strength;
	view_ray_position += ray_step * jitter;

	float visibility = 1.0;
	for (int i = 0; i < max_steps; ++i)
	{
		view_ray_position += ray_step;
		vec2 sample_uv = project_to_uv(view_ray_position);
		if (sample_uv.x < 0.0 || sample_uv.x > 1.0 || sample_uv.y < 0.0 || sample_uv.y > 1.0)
		{
			break;
		}

		vec4 blocker_world_position = texture(position_tex, sample_uv);
		vec4 blocker_normal = texture(normal_tex, sample_uv);
		if (blocker_normal == vec4(0.0))
		{
			continue;
		}

		vec3 blocker_world_normal = normalize(blocker_normal.xyz);
		float same_surface_distance = abs(dot(blocker_world_position.xyz - world_position, world_normal));
		if (dot(blocker_world_normal, world_normal) > same_surface_normal_threshold && same_surface_distance < normal_offset_bias * 2.0)
		{
			continue;
		}

		vec3 blocker_view_position = (view * vec4(blocker_world_position.xyz, 1.0)).xyz;
		float camera_depth_delta = blocker_view_position.z - view_ray_position.z;
		float effective_thickness = max(thickness, abs(view_ray_position.z) * 0.002);
		float depth_bias = min(effective_thickness * 0.5, max(0.005, abs(view_ray_position.z) * 0.0005));
		if (camera_depth_delta > depth_bias && camera_depth_delta < effective_thickness)
		{
			float edge_distance = min(min(sample_uv.x, 1.0 - sample_uv.x), min(sample_uv.y, 1.0 - sample_uv.y));
			visibility = 1.0 - clamp(edge_distance / edge_fade_width, 0.0, 1.0);
			break;
		}
	}

	frag_color = vec4(vec3(visibility), 1.0);
}
