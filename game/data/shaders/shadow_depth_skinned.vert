#version 450

#include "shader_common.h"

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

	mat4 skin_matrix = get_skin_matrix(pc.skin_matrix_offset, in_joint_indices, in_joint_weights);
	vec4 skinned_position = skin_matrix * in_position;

	gl_Position = pc.light_view_projection * obj.model_matrix * skinned_position;
}
