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

float sample_radial_depth(vec2 sample_uv)
{
	const vec4 world_position = texture(sampler2D(world_position_texture, tex_sampler), sample_uv);
	return world_position.w > 0.0
		? min(length(world_position.xyz - capture_location), GI_MAX_RADIAL_DEPTH)
		: GI_MAX_RADIAL_DEPTH;
}

void main()
{
	// Variance Shadow Map second-moment stabilization term from GPU Gems:
	// m2 += 0.25 * (ddx(d)^2 + ddy(d)^2)
	const float center_depth = sample_radial_depth(uv);
	const float depth_dx = dFdx(center_depth);
	const float depth_dy = dFdy(center_depth);
	const float derivative_bias = 0.25 * ((depth_dx * depth_dx) + (depth_dy * depth_dy));

	frag_color.x = center_depth;
	frag_color.y = (center_depth * center_depth) + derivative_bias;
}

@end

@program radial_depth vs fs
