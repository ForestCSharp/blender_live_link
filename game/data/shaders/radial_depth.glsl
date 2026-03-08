// Common Shader Code
#include "shader_common.h"

// GI Helpers
#include "gi_helpers.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.h"

@fs fs

@include_block remap
@include_block gi_helpers

layout(binding=0) uniform fs_params {
	mat4 inverse_view_projection;
	vec3 capture_location;
};

layout(binding=0) uniform texture2D world_position_texture;
layout(binding=0) uniform sampler tex_sampler;

in vec2 uv;

out vec2 frag_color;

// Compile-time blur controls for Chebyshev moment prefiltering.
#define RADIAL_DEPTH_BLUR 0
#define RADIAL_DEPTH_BLUR_RADIUS 8
#define RADIAL_DEPTH_BLUR_TAP_STRIDE 3

float sample_radial_depth(vec2 sample_uv)
{
	const vec4 world_position = texture(sampler2D(world_position_texture, tex_sampler), sample_uv);
	return world_position.w > 0.0
		? min(length(world_position.xyz - capture_location), GI_MAX_RADIAL_DEPTH)
		: GI_MAX_RADIAL_DEPTH;
}

void main()
{
#if RADIAL_DEPTH_BLUR
	const vec2 texel_size = 1.0 / vec2(textureSize(sampler2D(world_position_texture, tex_sampler), 0));
	const int blur_radius = max(RADIAL_DEPTH_BLUR_RADIUS, 0);
	const float tap_stride = max(float(RADIAL_DEPTH_BLUR_TAP_STRIDE), 1.0);
	vec2 moments = vec2(0.0);
	float total_weight = 0.0;

	// Radius-driven tent kernel. Increase RADIAL_DEPTH_BLUR_RADIUS for more blur.
	for (int y = -blur_radius; y <= blur_radius; ++y)
	{
		for (int x = -blur_radius; x <= blur_radius; ++x)
		{
			const float wx = float(blur_radius + 1 - abs(x));
			const float wy = float(blur_radius + 1 - abs(y));
			const float weight = wx * wy;
			const vec2 offset = vec2(float(x), float(y)) * tap_stride * texel_size;
			const vec2 sample_uv = clamp(uv + offset, vec2(0.0), vec2(1.0));
			const float radial_depth = sample_radial_depth(sample_uv);
			moments += vec2(radial_depth, radial_depth * radial_depth) * weight;
			total_weight += weight;
		}
	}

	frag_color = total_weight > 0.0 ? (moments / total_weight) : moments;
#else
	const float radial_depth = sample_radial_depth(uv);
	frag_color.x = radial_depth;
	frag_color.y = radial_depth * radial_depth;
#endif
}

@end

@program radial_depth vs fs
