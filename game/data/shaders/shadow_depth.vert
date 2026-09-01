#version 450

#include "geometry_common.h"

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_normal;
layout(location = 2) in vec2 in_texcoord;

// Light view-projection comes via push constants, pushed once per cascade;
// the object index arrives as gl_InstanceIndex like the geometry pass.
layout(push_constant) uniform PushConstants
{
	mat4 light_view_projection;
} pc;

void main()
{
	ObjectData obj = object_data_array[gl_InstanceIndex];
	gl_Position = pc.light_view_projection * obj.model_matrix * in_position;
}
