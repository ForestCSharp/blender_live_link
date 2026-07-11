#version 450

// Probe-capture variant of geometry_skinned.vert (push-constant camera)

#include "shader_common.h"

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_normal;
layout(location = 2) in vec2 in_texcoord;
layout(location = 3) in vec4 in_joint_indices;
layout(location = 4) in vec4 in_joint_weights;

layout(push_constant) uniform PushConstants
{
	mat4 view_projection;
	int object_index;
	int skin_matrix_offset;
} pc;

layout(location = 0) out vec4 out_world_position;
layout(location = 1) out vec4 out_world_normal;
layout(location = 2) out vec2 out_texcoord;
layout(location = 3) flat out int out_material_index;

void main()
{
	ObjectData obj = object_data_array[pc.object_index];

	mat4 skin_matrix = get_skin_matrix(pc.skin_matrix_offset, in_joint_indices, in_joint_weights);
	vec4 skinned_position = skin_matrix * in_position;
	vec4 skinned_normal = vec4(normalize((skin_matrix * vec4(in_normal.xyz, 0.0)).xyz), 0.0);

	out_world_position = obj.model_matrix * skinned_position;
	out_world_normal = obj.rotation_matrix * skinned_normal;
	out_texcoord = in_texcoord;
	out_material_index = obj.material_index;

	gl_Position = pc.view_projection * out_world_position;
}
