
// Common Shader Code
#include "shader_common.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.h"

@fs fs

@image_sample_type input_texture_a unfilterable_float
layout(binding=0) uniform texture2D input_texture_a;

@image_sample_type input_texture_b unfilterable_float
layout(binding=1) uniform texture2D input_texture_b;

@sampler_type tex_sampler nonfiltering
layout(binding=0) uniform sampler tex_sampler;

in vec2 uv;

out vec4 frag_color;

void main()
{
	vec4 out_color = texture(sampler2D(input_texture_a, tex_sampler), uv);
	vec4 color_b = texture(sampler2D(input_texture_b, tex_sampler), uv);
	if (color_b.a > 0.0f)
	{
		out_color = color_b;
	}
	frag_color = out_color;
}

@end

@program overlay_texture vs fs
