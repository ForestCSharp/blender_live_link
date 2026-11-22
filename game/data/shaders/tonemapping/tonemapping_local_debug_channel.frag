#version 450

layout(set = 0, binding = 0) uniform sampler2D packed_input;

layout(push_constant) uniform PushConstants
{
	int channel;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 channel_output;

void main()
{
	float value = texture(packed_input, uv)[clamp(pc.channel, 0, 3)];
	channel_output = vec4(vec3(value), 1.0);
}
