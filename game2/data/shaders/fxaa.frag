#version 450

// Simple luma-based FXAA over the tonemapped LDR target (port of
// game/data/shaders/fxaa.glsl fs).

layout(set = 0, binding = 0) uniform sampler2D color_tex;

layout(push_constant) uniform PushConstants
{
	vec2 screen_size;
	float contrast_threshold;
	float relative_threshold;
} pc;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

float luma(vec3 color)
{
	return dot(color, vec3(0.299, 0.587, 0.114));
}

void main()
{
	vec2 texel_size = 1.0 / pc.screen_size;

	vec3 color_m = texture(color_tex, uv).rgb;
	float luma_m = luma(color_m);

	float luma_n = luma(texture(color_tex, uv + vec2(0.0, -texel_size.y)).rgb);
	float luma_s = luma(texture(color_tex, uv + vec2(0.0,  texel_size.y)).rgb);
	float luma_w = luma(texture(color_tex, uv + vec2(-texel_size.x, 0.0)).rgb);
	float luma_e = luma(texture(color_tex, uv + vec2( texel_size.x, 0.0)).rgb);

	float luma_min = min(luma_m, min(min(luma_n, luma_s), min(luma_w, luma_e)));
	float luma_max = max(luma_m, max(max(luma_n, luma_s), max(luma_w, luma_e)));
	float luma_range = luma_max - luma_min;
	float threshold = max(pc.contrast_threshold, luma_max * pc.relative_threshold);

	if (luma_range < threshold)
	{
		frag_color = vec4(color_m, 1.0);
		return;
	}

	float horizontal_edge = abs(luma_n + luma_s - 2.0 * luma_m);
	float vertical_edge = abs(luma_w + luma_e - 2.0 * luma_m);
	bool horizontal = horizontal_edge >= vertical_edge;

	vec2 direction = horizontal ? vec2(texel_size.x, 0.0) : vec2(0.0, texel_size.y);
	vec3 color_a = 0.5 * (
		texture(color_tex, uv - direction * 0.5).rgb +
		texture(color_tex, uv + direction * 0.5).rgb
	);
	vec3 color_b = 0.5 * color_a + 0.25 * (
		texture(color_tex, uv - direction).rgb +
		texture(color_tex, uv + direction).rgb
	);

	float luma_b = luma(color_b);
	vec3 out_color = (luma_b < luma_min || luma_b > luma_max) ? color_a : color_b;
	frag_color = vec4(out_color, 1.0);
}
