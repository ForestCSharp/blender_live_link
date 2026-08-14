#version 450

#include "display_encoding.h"

layout(set = 0, binding = 0) uniform sampler2D scene_color;

layout(push_constant) uniform PushConstants
{
	int output_mode; // One of DISPLAY_OUTPUT_MODE_* from shader_common.h.
	int sdr_attachment_is_srgb;
	int grayscale_input;
} pc;

layout(location = 0) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

void main()
{
	vec3 scene = texture(scene_color, in_uv).rgb;
	if (pc.grayscale_input != 0)
		scene = vec3(scene.r);
	out_color = vec4(display_encode(
		scene, pc.output_mode, pc.sdr_attachment_is_srgb, gl_FragCoord.xy), 1.0);
}
