#version 450

// Writes EVSM4 moments for lighting. The depth attachment is used only while
// rendering this pass and is not sampled afterward.

#include "evsm.h"

layout(location = 0) out vec4 frag_color;

void main()
{
	// Reverse-Z
	float depth = 1.0 - gl_FragCoord.z;

	vec2 warped_depth = evsm_warp_depth(depth);
	vec2 dx = dFdx(warped_depth);
	vec2 dy = dFdy(warped_depth);
	const float moment_bias = 0.00001;
	vec2 second_moments = warped_depth * warped_depth + 0.25 * (dx * dx + dy * dy) + vec2(moment_bias);
	frag_color = vec4(warped_depth.x, second_moments.x, warped_depth.y, second_moments.y);
}
