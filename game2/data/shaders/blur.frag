#version 450

// Generic separable gaussian blur (port of game/data/shaders/blur.glsl fs).
// The 2D counterpart of shadow_blur.frag.

layout(set = 0, binding = 0) uniform sampler2D color_tex;

layout(push_constant) uniform PushConstants
{
	vec2 screen_size;
	vec2 direction;
	int blur_size;
} pc;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

void main()
{
	vec2 texel_size = 1.0 / pc.screen_size;
	vec4 result = vec4(0.0);
	float total_weight = 0.0;
	float hlim = float(-pc.blur_size) * 0.5 + 0.5;
	float sigma = max(float(pc.blur_size) * 0.25, 1.0);
	float two_sigma_squared = 2.0 * sigma * sigma;
	for (int i = 0; i < pc.blur_size; ++i)
	{
		float sample_offset = hlim + float(i);
		float weight = exp(-(sample_offset * sample_offset) / two_sigma_squared);
		vec2 offset = pc.direction * sample_offset * texel_size;
		result += texture(color_tex, uv + offset) * weight;
		total_weight += weight;
	}
	frag_color = result / total_weight;
}
