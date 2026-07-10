#version 450

#include "shader_common.h"

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_normal;
layout(location = 2) in vec2 in_texcoord;

layout(push_constant) uniform PushConstants
{
	int object_index;
	int skin_matrix_offset;	// unused in the static path
} pc;

layout(location = 0) out vec3 out_world_normal;
layout(location = 1) out vec2 out_texcoord;
layout(location = 2) flat out int out_material_index;

void main()
{
	ObjectData obj = object_data_array[pc.object_index];
	gl_Position = per_frame.view_projection * obj.model_matrix * vec4(in_position.xyz, 1.0);

	// rotation_matrix (not model) so non-uniform scale doesn't skew normals
	out_world_normal = normalize(mat3(obj.rotation_matrix) * in_normal.xyz);
	out_texcoord = in_texcoord;
	out_material_index = obj.material_index;
}
