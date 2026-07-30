#version 450

// Copies the base color into the wire-overlay target before mesh wires
// alpha-blend on top.

layout(set = 0, binding = 0) uniform sampler2D source_color_tex;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

void main()
{
	frag_color = texture(source_color_tex, uv);
}
