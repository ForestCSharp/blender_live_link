
// Common Shader Code
#include "shader_common.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.glslh"

@fs fs

layout(binding=0) uniform texture2D color_tex;
layout(binding=0) uniform sampler tex_sampler;

layout(binding=0) uniform fs_params {
	vec2 screen_size;
	int blur_size;
};

in vec2 uv;

out vec4 frag_color;

void main()
{
    vec2 texelSize = 1.0 / screen_size;
	vec4 result = vec4(0.0);
	vec2 hlim = vec2(float(-blur_size) * 0.5 + 0.5);
	for (int i = 0; i < blur_size; ++i)
	{
		for (int j = 0; j < blur_size; ++j)
		{
	    	vec2 offset = (hlim + vec2(float(i), float(j))) * texelSize;
			result += texture(sampler2D(color_tex, tex_sampler), uv + offset);
		}
	}	
	frag_color = result / float(blur_size * blur_size);
}

@end

@program blur vs fs
