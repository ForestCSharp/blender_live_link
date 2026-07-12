
// Common Shader Code
#include "shader_common.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.h"

@fs fs

layout(binding=0) uniform texture2D color_tex;
layout(binding=0) uniform sampler tex_sampler;

layout(binding=0) uniform fs_params {
	vec2 screen_size;
	vec2 direction;
	int blur_size;
};

in vec2 uv;

out vec4 frag_color;

void main()
{
    vec2 texelSize = 1.0 / screen_size;
	vec4 result = vec4(0.0);
	float total_weight = 0.0;
	float hlim = float(-blur_size) * 0.5 + 0.5;
	float sigma = max(float(blur_size) * 0.25, 1.0);
	float two_sigma_squared = 2.0 * sigma * sigma;
	for (int i = 0; i < blur_size; ++i)
	{
		float sample_offset = hlim + float(i);
		float weight = exp(-(sample_offset * sample_offset) / two_sigma_squared);
		vec2 offset = direction * sample_offset * texelSize;
		result += texture(sampler2D(color_tex, tex_sampler), uv + offset) * weight;
		total_weight += weight;
	}	
	frag_color = result / total_weight;
}

@end

@program blur vs fs
