#version 450

#include "geometry_common.h"

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_normal;
layout(location = 2) in vec2 in_texcoord;
layout(location = 3) in vec4 in_joint_indices;
layout(location = 4) in vec4 in_joint_weights;

layout(push_constant) uniform PushConstants
{
	mat4 light_view_projection;
	int object_index;
	int skin_matrix_offset;
} pc;

void main()
{
	ObjectData obj = object_data_array[pc.object_index];

	vec4 skinned_position = geometry_skinned_position(
		in_position,
		pc.skin_matrix_offset,
		in_joint_indices,
		in_joint_weights);

	gl_Position = pc.light_view_projection * obj.model_matrix * skinned_position;
}
