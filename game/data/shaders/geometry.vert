#version 450

#include "geometry_common.h"

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_normal;
layout(location = 2) in vec2 in_texcoord;

// Pass-constant only. The object index arrives as gl_InstanceIndex, which
// equals the draw's firstInstance because instanceCount is always 1.
layout(push_constant) uniform PushConstants
{
	int skinning_debug_view;
} pc;

layout(location = 0) out vec4 out_world_position;
layout(location = 1) out vec4 out_world_normal;
layout(location = 2) out vec2 out_texcoord;
layout(location = 3) flat out int out_material_index;
layout(location = 4) out vec4 out_skin_debug_color;
layout(location = 5) flat out int out_is_skinned_mesh;

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
	out_skin_debug_color = vertex.skin_debug_color;
	out_is_skinned_mesh = vertex.is_skinned;

	gl_Position = per_frame.view_projection * out_world_position;
}
