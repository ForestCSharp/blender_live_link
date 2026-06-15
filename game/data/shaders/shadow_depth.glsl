// Common Shader Code
#include "shader_common.h"

@vs vs

layout(binding=0) uniform vs_params {
    mat4 view;
	mat4 projection;
};

@include_block object_data

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 normal;
layout(location = 2) in vec2 texcoord;

void main() {
	vec4 world_position = object_data_array[0].model_matrix * position;
    gl_Position = projection * view * world_position;
}
@end

@vs skinned_vs

layout(binding=0) uniform vs_params {
    mat4 view;
	mat4 projection;
};

@include_block object_data
@include_block skinning_data

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 normal;
layout(location = 2) in vec2 texcoord;
layout(location = 3) in vec4 joint_indices;
layout(location = 4) in vec4 joint_weights;

void main() {
	mat4 skin_matrix = get_skin_matrix(joint_indices, joint_weights);
	vec4 skinned_position = skin_matrix * position;
	vec4 world_position = object_data_array[0].model_matrix * skinned_position;
    gl_Position = projection * view * world_position;
}
@end

@fs fs
out vec4 frag_color;

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

void main() {
#if USE_INVERSE_DEPTH
	float depth = 1.0 - gl_FragCoord.z;
#else
	float depth = gl_FragCoord.z;
#endif

	vec2 warped_depth = evsm_warp_depth(depth);
	vec2 dx = dFdx(warped_depth);
	vec2 dy = dFdy(warped_depth);
	const float moment_bias = 0.00001;
	vec2 second_moments = warped_depth * warped_depth + 0.25 * (dx * dx + dy * dy) + vec2(moment_bias);
	frag_color = vec4(warped_depth.x, second_moments.x, warped_depth.y, second_moments.y);
}
@end

@program shadow_depth vs fs
@program skinned_shadow_depth skinned_vs fs
