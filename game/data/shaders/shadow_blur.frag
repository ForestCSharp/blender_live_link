#version 450

// Applies a separable Gaussian blur to the EVSM moments array, one cascade
// layer per slice. Blurred moments give EVSM its soft penumbra, while
// Chebyshev evaluation over unfiltered moments produces an almost binary
// result.

layout(set = 0, binding = 0) uniform sampler2DArray color_tex;

layout(push_constant) uniform PushConstants
{
	vec2 screen_size;
	vec2 direction;
	int blur_size;
	int array_layer;
} pc;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

#define GAUSSIAN_BLUR_FETCH(sample_uv) \
	texture(color_tex, vec3((sample_uv), float(pc.array_layer)))
#include "gaussian_blur.h"
#undef GAUSSIAN_BLUR_FETCH

void main()
{
	frag_color = gaussian_blur_apply(
		uv, pc.screen_size, pc.direction, pc.blur_size);
}
