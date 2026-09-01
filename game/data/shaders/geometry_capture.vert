#version 450

// Probe-capture variant of geometry.vert: the camera changes per cube face,
// so view_projection rides in push constants instead of the per-frame UBO.

#include "geometry_common.h"

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_normal;
layout(location = 2) in vec2 in_texcoord;

// Pushed once per cube face; the object index arrives as gl_InstanceIndex.
layout(push_constant) uniform PushConstants
{
	mat4 view_projection;
	vec4 capture_position_and_radius;	// used by geometry_capture.frag
} pc;

layout(location = 0) out vec4 out_world_position;
layout(location = 1) out vec4 out_world_normal;
layout(location = 2) out vec2 out_texcoord;
layout(location = 3) flat out int out_material_index;

void main()
{
	ObjectData obj = object_data_array[gl_InstanceIndex];
	GeometryVertexSample vertex = geometry_static_vertex(in_position, in_normal);
	GeometryWorldVertex world_vertex = geometry_world_vertex(
		obj.model_matrix,
		obj.rotation_matrix,
		vertex.local_position,
		vertex.local_normal);

	out_world_position = world_vertex.position;
	out_world_normal = world_vertex.normal;
	out_texcoord = in_texcoord;
	out_material_index = obj.material_index;

	gl_Position = pc.view_projection * out_world_position;
}
