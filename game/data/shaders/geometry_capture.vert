#version 450

// Probe-capture variant of geometry.vert: the camera changes per cube face,
// so view_projection rides in push constants instead of the per-frame UBO.

#include "shader_common.h"

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_normal;
layout(location = 2) in vec2 in_texcoord;

layout(push_constant) uniform PushConstants
{
	mat4 view_projection;
	int object_index;
	int skin_matrix_offset;	// unused in the static path
} pc;

layout(location = 0) out vec4 out_world_position;
layout(location = 1) out vec4 out_world_normal;
layout(location = 2) out vec2 out_texcoord;
layout(location = 3) flat out int out_material_index;

void main()
{
	ObjectData obj = object_data_array[pc.object_index];

	out_world_position = obj.model_matrix * in_position;
	out_world_normal = obj.rotation_matrix * in_normal;
	out_texcoord = in_texcoord;
	out_material_index = obj.material_index;

	gl_Position = pc.view_projection * out_world_position;
}
