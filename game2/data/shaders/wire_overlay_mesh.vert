#version 450

// Wireframe mesh pass (port of game/data/shaders/wire_overlay.glsl mesh_vs):
// manual vertex pulling from the mesh's vertex/index SSBOs so barycentrics
// can be generated per-corner from the sequential gl_VertexIndex.

#include "shader_common.h"

struct WireOverlayVertex
{
	vec4 position;
	vec4 normal;
	vec2 texcoord;
	vec2 _padding;
};

layout(push_constant) uniform PushConstants
{
	int object_index;
} pc;

layout(set = 2, binding = 0) readonly buffer WireOverlayVerticesBuffer
{
	WireOverlayVertex wire_vertices[];
};

layout(set = 2, binding = 1) readonly buffer WireOverlayIndicesBuffer
{
	uint wire_indices_array[];
};

layout(location = 0) noperspective out vec3 wire_barycentric;
layout(location = 1) noperspective out vec2 wire_screen_uv;
layout(location = 2) out vec4 wire_world_position;

void main()
{
	uint corner = uint(gl_VertexIndex) % 3u;
	uint index = wire_indices_array[uint(gl_VertexIndex)];
	WireOverlayVertex wire_vertex = wire_vertices[index];

	wire_barycentric = corner == 0u
		? vec3(1.0, 0.0, 0.0)
		: corner == 1u
			? vec3(0.0, 1.0, 0.0)
			: vec3(0.0, 0.0, 1.0);

	wire_world_position = object_data_array[pc.object_index].model_matrix * wire_vertex.position;
	gl_Position = per_frame.view_projection * wire_world_position;

	float safe_w = abs(gl_Position.w) < 0.000001 ? 0.000001 : gl_Position.w;
	vec2 ndc = gl_Position.xy / safe_w;
	wire_screen_uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}
