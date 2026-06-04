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
	int probe_occlusion_mode;
	int force_fully_visible;
	float max_radial_depth;
};

layout(binding=0) uniform texture2D world_position_texture;
layout(binding=0) uniform sampler tex_sampler;

in vec2 uv;

out vec4 frag_color;

const int PROBE_OCCLUSION_MODE_CHEBYSHEV = 0;
const int PROBE_OCCLUSION_MODE_EVRP4 = 1;

const float EVRP_POSITIVE_EXPONENT = 5.0;
const float EVRP_NEGATIVE_EXPONENT = 5.0;
const float EVRP_MOMENT_BIAS = 0.00001;

float sample_radial_depth(vec2 sample_uv)
{
	const vec4 world_position = texture(sampler2D(world_position_texture, tex_sampler), sample_uv);
	return world_position.w > 0.0
		? min(length(world_position.xyz - capture_location), max_radial_depth)
		: max_radial_depth;
}

void main()
{
	// Variance Shadow Map second-moment stabilization term from GPU Gems:
	// m2 += 0.25 * (ddx(d)^2 + ddy(d)^2)
	const float center_depth = force_fully_visible != 0
		? max_radial_depth
		: sample_radial_depth(uv);
	if (probe_occlusion_mode == PROBE_OCCLUSION_MODE_EVRP4)
	{
		const float normalized_depth = clamp(center_depth / max_radial_depth, 0.0, 1.0);
		const vec2 warped_depth = vec2(
			exp(EVRP_POSITIVE_EXPONENT * normalized_depth),
			-exp(-EVRP_NEGATIVE_EXPONENT * normalized_depth)
		);

		const vec2 dx = dFdx(warped_depth);
		const vec2 dy = dFdy(warped_depth);
		const vec2 derivative_bias = 0.25 * ((dx * dx) + (dy * dy));
		const vec2 second_moments = warped_depth * warped_depth + derivative_bias + vec2(EVRP_MOMENT_BIAS);

		frag_color = vec4(warped_depth.x, second_moments.x, warped_depth.y, second_moments.y);
	}
	else
	{
		const float depth_dx = dFdx(center_depth);
		const float depth_dy = dFdy(center_depth);
		const float derivative_bias = 0.25 * ((depth_dx * depth_dx) + (depth_dy * depth_dy));

		frag_color = vec4(center_depth, (center_depth * center_depth) + derivative_bias, 0.0, 0.0);
	}
}

@end

@program radial_depth vs fs
