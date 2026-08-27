#version 450

#include "gi_helpers.h"
#include "gi_debug_common.h"

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
	probe_index = gl_InstanceIndex;
	GI_Probe probe = probes[probe_index];
	bool visible = probe_level_filter_enable != 0
		? (probe_level_filter_selection == 0
			? probe.octree_level < 0
			: probe.octree_level == probe_level_filter_selection - 1)
		: probe.octree_level >= 0;
	if (!visible)
	{
		world_position = vec4(0.0);
		world_normal = vec4(0.0);
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		return;
	}

	int levels_above_deepest = probe.octree_level < 0
		? octree_depth + 1
		: clamp(octree_depth - probe.octree_level, 0, octree_depth);
	float radius = probe_debug_radius * pow(1.1, float(levels_above_deepest));
	world_position = vec4(probe.position.xyz + position.xyz * radius, 1.0);
	world_normal = normal;
	gl_Position = view_projection * world_position;
}
