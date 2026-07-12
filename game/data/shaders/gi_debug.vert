#version 450

#include "gi_helpers.h"

layout(push_constant) uniform DebugParams
{
	mat4 view_projection;
	int debug_probe_start_index;
	float probe_debug_radius;
	int atlas_total_size;
	int atlas_entry_size;
	int probe_vis_mode;
	int isolated_probe_index;
	int padding0;
	int padding1;
};

layout(set = 0, binding = 0, std430) readonly buffer ProbeVertexBlock
{
	GI_Probe probes[];
};

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 normal;
layout(location = 0) out vec4 world_position;
layout(location = 1) out vec4 world_normal;
layout(location = 2) flat out int probe_index;

void main()
{
	probe_index = debug_probe_start_index + gl_InstanceIndex;
	GI_Probe probe = probes[probe_index];
	float radius = max((probe.max_radial_depth / GI_RADIAL_DEPTH_CELL_SCALE) * 0.1, probe_debug_radius);
	world_position = vec4(probe.position.xyz + position.xyz * radius, 1.0);
	world_normal = normal;
	gl_Position = view_projection * world_position;
}
