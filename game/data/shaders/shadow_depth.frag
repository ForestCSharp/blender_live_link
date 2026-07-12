#version 450

// EVSM4 moments (port of game/data/shaders/shadow_depth.glsl fs). The depth
// attachment is throwaway; lighting samples these moments.

layout(location = 0) out vec4 frag_color;

const float EVSM_POSITIVE_EXPONENT = 5.0;
const float EVSM_NEGATIVE_EXPONENT = 5.0;

vec2 evsm_warp_depth(float depth)
{
	float centered_depth = depth * 2.0 - 1.0;
	return vec2(
		exp(EVSM_POSITIVE_EXPONENT * centered_depth),
		-exp(-EVSM_NEGATIVE_EXPONENT * centered_depth)
	);
}

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
