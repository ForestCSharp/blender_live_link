#version 450

#include "shader_common.h"

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_normal;
layout(location = 2) in vec2 in_texcoord;
layout(location = 3) in vec4 in_joint_indices;
layout(location = 4) in vec4 in_joint_weights;

layout(push_constant) uniform PushConstants
{
	int object_index;
	int skin_matrix_offset;	// this mesh's base offset in the arena
} pc;

layout(location = 0) out vec3 out_world_normal;
layout(location = 1) out vec2 out_texcoord;
layout(location = 2) flat out int out_material_index;

void main()
{
	ObjectData obj = object_data_array[pc.object_index];

	mat4 skin_matrix = get_skin_matrix(pc.skin_matrix_offset, in_joint_indices, in_joint_weights);
	vec4 skinned_position = skin_matrix * vec4(in_position.xyz, 1.0);
	vec3 skinned_normal = normalize((skin_matrix * vec4(in_normal.xyz, 0.0)).xyz);

	gl_Position = per_frame.view_projection * obj.model_matrix * vec4(skinned_position.xyz, 1.0);
	out_world_normal = normalize(mat3(obj.rotation_matrix) * skinned_normal);
	out_texcoord = in_texcoord;
	out_material_index = obj.material_index;
}
