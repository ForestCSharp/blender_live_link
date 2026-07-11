#version 450

// Base copy for the wire overlay target (port of game/data/shaders/
// wire_overlay.glsl copy_fs) — the mesh wires alpha-blend on top.

layout(set = 0, binding = 0) uniform sampler2D source_color_tex;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

void main()
{
	frag_color = texture(source_color_tex, uv);
}
