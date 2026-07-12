#version 450

#include "shader_common.h"

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_normal;
layout(location = 2) in vec2 in_texcoord;

// Light view-projection comes via push constants (per cascade); object
// transform from the ObjectData SSBO like the geometry pass
layout(push_constant) uniform PushConstants
{
	mat4 light_view_projection;
	int object_index;
	int skin_matrix_offset;
} pc;

void main()
{
	ObjectData obj = object_data_array[pc.object_index];
	gl_Position = pc.light_view_projection * obj.model_matrix * in_position;
}
