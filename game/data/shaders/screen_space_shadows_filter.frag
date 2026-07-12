#version 450

// Edge-aware smoothing of the raw contact-shadow mask (port of
// game/data/shaders/screen_space_shadows.glsl filter_fs).

layout(set = 0, binding = 0) uniform filter_fs_params
{
	vec2 screen_size;
	int filter_radius;
};

layout(set = 0, binding = 1) uniform sampler2D raw_shadow_tex;
layout(set = 0, binding = 2) uniform sampler2D filter_position_tex;
layout(set = 0, binding = 3) uniform sampler2D filter_normal_tex;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

void main()
{
	vec4 center_position = texture(filter_position_tex, uv);
	vec4 center_normal = texture(filter_normal_tex, uv);
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
			vec4 sample_position = texture(filter_position_tex, sample_uv);
			vec4 sample_normal = texture(filter_normal_tex, sample_uv);
			float visibility = texture(raw_shadow_tex, sample_uv).r;

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
		: texture(raw_shadow_tex, uv).r;
	frag_color = vec4(vec3(filtered_visibility), 1.0);
}
