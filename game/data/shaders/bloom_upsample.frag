#version 450

layout(set = 0, binding = 0) uniform sampler2D source_tex;

layout(push_constant) uniform PushConstants
{
	vec2 source_pixel_size;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag_color;

void main()
{
	vec3 result = vec3(0.0);
	result += texture(source_tex, uv + vec2(-1.0, -1.0) * pc.source_pixel_size).rgb;
	result += texture(source_tex, uv + vec2( 0.0, -1.0) * pc.source_pixel_size).rgb * 2.0;
	result += texture(source_tex, uv + vec2( 1.0, -1.0) * pc.source_pixel_size).rgb;
	result += texture(source_tex, uv + vec2(-1.0,  0.0) * pc.source_pixel_size).rgb * 2.0;
	result += texture(source_tex, uv).rgb * 4.0;
	result += texture(source_tex, uv + vec2( 1.0,  0.0) * pc.source_pixel_size).rgb * 2.0;
	result += texture(source_tex, uv + vec2(-1.0,  1.0) * pc.source_pixel_size).rgb;
	result += texture(source_tex, uv + vec2( 0.0,  1.0) * pc.source_pixel_size).rgb * 2.0;
	result += texture(source_tex, uv + vec2( 1.0,  1.0) * pc.source_pixel_size).rgb;
	frag_color = vec4(result / 16.0, 0.0);
}
