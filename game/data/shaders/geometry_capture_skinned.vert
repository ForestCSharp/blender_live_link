#version 450

// Probe-capture variant of geometry_skinned.vert (push-constant camera)

#include "geometry_common.h"

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_normal;
layout(location = 2) in vec2 in_texcoord;
layout(location = 3) in vec4 in_joint_indices;
layout(location = 4) in vec4 in_joint_weights;

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
	GeometryVertexSample vertex = geometry_skinned_vertex(
		in_position,
		in_normal,
		obj.skin_matrix_offset,
		in_joint_indices,
		in_joint_weights);
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
