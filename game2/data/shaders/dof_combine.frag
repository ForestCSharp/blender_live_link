#version 450

// Single-pass gather depth-of-field (port of game/data/shaders/
// dof_combine.glsl fs): signed circle-of-confusion from the G-buffer world
// position, 16-tap poisson disk gather with cross-depth bleed suppression.

layout(set = 0, binding = 0) uniform fs_params
{
	vec4 cam_pos;
	vec4 cam_forward;
	vec2 screen_size;
	float focus_distance;
	float focus_range;
	float max_coc_radius;
	float foreground_blur_scale;
	float background_blur_scale;
	int debug_mode;
};

layout(set = 0, binding = 1) uniform sampler2D color_tex;
layout(set = 0, binding = 2) uniform sampler2D position_tex;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

float view_depth(vec4 world_position)
{
	return dot(world_position.xyz - cam_pos.xyz, normalize(cam_forward.xyz));
}

float signed_coc_radius(vec4 world_position)
{
	if (world_position.a == 0.0)
	{
		return 0.0;
	}

	float depth = view_depth(world_position);
	float focus_delta = depth - focus_distance;
	float blur_scale = focus_delta < 0.0 ? foreground_blur_scale : background_blur_scale;
	float normalized_coc = abs(focus_delta) / max(focus_range, 0.001);
	float coc_radius = min(normalized_coc * max_coc_radius * blur_scale, max_coc_radius);
	return focus_delta < 0.0 ? -coc_radius : coc_radius;
}

void main()
{
	vec4 color = texture(color_tex, uv);
	vec4 world_position = texture(position_tex, uv);
	float center_signed_coc = signed_coc_radius(world_position);
	float center_coc = abs(center_signed_coc);

	if (debug_mode == 1)
	{
		float debug_amount = max_coc_radius > 0.0 ? clamp(center_coc / max_coc_radius, 0.0, 1.0) : 0.0;
		vec3 debug_color = center_signed_coc < 0.0
			? mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 0.2, 0.0), debug_amount)
			: mix(vec3(0.0, 1.0, 0.0), vec3(0.1, 0.35, 1.0), debug_amount);
		frag_color = vec4(debug_color, 1.0);
		return;
	}

	if (world_position.a == 0.0 || center_coc < 0.5 || max_coc_radius <= 0.0)
	{
		frag_color = color;
		return;
	}

	const vec2 disk_offsets[16] = vec2[](
		vec2( 0.0000,  0.0000),
		vec2( 0.5381,  0.1856),
		vec2(-0.3774,  0.3518),
		vec2( 0.1436, -0.5163),
		vec2(-0.5596, -0.1349),
		vec2( 0.3865, -0.4288),
		vec2(-0.1294,  0.5862),
		vec2( 0.7200, -0.0340),
		vec2(-0.7060,  0.1260),
		vec2( 0.3220,  0.6530),
		vec2(-0.3200, -0.6420),
		vec2( 0.9160,  0.3830),
		vec2(-0.9090, -0.3920),
		vec2( 0.1290, -0.9740),
		vec2(-0.1120,  0.9710),
		vec2( 0.0000,  0.8200)
	);

	vec2 texel_size = 1.0 / screen_size;
	float center_depth = view_depth(world_position);
	float depth_epsilon = max(focus_range * 0.05, 0.5);
	vec4 result = color;
	float total_weight = 1.0;

	for (int i = 1; i < 16; ++i)
	{
		vec2 sample_uv = uv + disk_offsets[i] * center_coc * texel_size;
		vec4 sample_position = texture(position_tex, sample_uv);
		if (sample_position.a == 0.0)
		{
			continue;
		}

		float sample_signed_coc = signed_coc_radius(sample_position);
		float sample_coc = abs(sample_signed_coc);
		float sample_depth = view_depth(sample_position);
		float sample_weight = 1.0;

		sample_weight *= smoothstep(0.0, center_coc + 1.0, sample_coc + 1.0);

		// Suppress cross-depth bleeding at silhouettes.
		if (center_signed_coc < 0.0 && sample_depth > center_depth + depth_epsilon)
		{
			sample_weight *= 0.15;
		}
		else if (center_signed_coc > 0.0 && sample_depth < center_depth - depth_epsilon)
		{
			sample_weight *= 0.15;
		}

		result += texture(color_tex, sample_uv) * sample_weight;
		total_weight += sample_weight;
	}

	frag_color = result / total_weight;
}
