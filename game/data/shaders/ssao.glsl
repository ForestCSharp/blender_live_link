@ctype mat4 HMM_Mat4

// Fullscreen Vertex Shader
#include "fullscreen_vs.glslh"

@fs fs

layout(binding=0) uniform texture2D color_tex;
layout(binding=1) uniform texture2D normal_tex;
layout(binding=2) uniform texture2D position_tex;

layout(binding=0) uniform sampler tex_sampler;

in vec2 uv;

out vec4 frag_color;

void main()
{
	vec4 sampled_color		= texture(sampler2D(color_tex, tex_sampler), uv);
	vec4 sampled_normal		= texture(sampler2D(normal_tex, tex_sampler), uv);
	vec4 sampled_position	= texture(sampler2D(position_tex, tex_sampler), uv);

	//TODO: SSAO

	frag_color = sampled_color;
}

@end

@program ssao vs fs
