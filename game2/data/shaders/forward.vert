#version 450

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_normal;
layout(location = 2) in vec2 in_texcoord;

// Per-object data. 128 bytes = the guaranteed push constant minimum.
// First upgrade slot: move view_proj/camera/sun into a per-frame UBO
// (descriptor set 0) and keep only model here.
layout(push_constant) uniform PushConstants
{
	mat4 mvp;
	mat4 model;
} pc;

layout(location = 0) out vec3 out_world_normal;

void main()
{
	gl_Position = pc.mvp * vec4(in_position.xyz, 1.0);
	out_world_normal = normalize(mat3(pc.model) * in_normal.xyz);
}
