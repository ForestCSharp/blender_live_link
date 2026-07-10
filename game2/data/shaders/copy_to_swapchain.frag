#version 450

layout(set = 0, binding = 0) uniform sampler2D scene_color;

layout(location = 0) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

void main()
{
	// Linear scene color; hardware sRGB-encodes on the swapchain-view write.
	// Tonemapping slots in here (as its own pass) in Phase 3.
	out_color = texture(scene_color, in_uv);
}
