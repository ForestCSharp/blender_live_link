#version 450

// Generic separable Gaussian blur for 2D images.
// The array-texture counterpart is shadow_blur.frag.

layout(set = 0, binding = 0) uniform sampler2D color_tex;

layout(push_constant) uniform PushConstants
{
	vec2 screen_size;
	vec2 direction;
	int blur_size;
} pc;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

#define GAUSSIAN_BLUR_FETCH(sample_uv) texture(color_tex, (sample_uv))
#include "gaussian_blur.h"
#undef GAUSSIAN_BLUR_FETCH

void main()
{
	frag_color = gaussian_blur_apply(
		uv, pc.screen_size, pc.direction, pc.blur_size);
}
