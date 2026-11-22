#ifndef GI_HELPERS_H
#define GI_HELPERS_H

// Shared GI structs and octree traversal helpers. Structs use byte-identical
// C++ and GLSL std430 layouts so buffers can be shared without
// translation.

#include "shared/shader_common.h"

#define GI_MAX_OCTREE_SEARCH_DEPTH 32
#define GI_RADIAL_DEPTH_CELL_SCALE 4.0

struct GI_Coords
{
	int x, y, z;
};

struct GI_Cell
{
	int probe_indices[8];
};

struct GI_Probe
{
	vec4 position;
	int atlas_idx;
	float max_radial_depth;
	int octree_level;
	int specular_atlas_idx;	// slot in the specular atlas; -1 = diffuse only
};

struct GI_OctreeNode
{
	vec4 min;
	vec4 max;
	int is_leaf;
	int child_indices[8];
	int payload_index;
	int specular_payload_index;	// coarse cell (8 specular corners) on depth D-1 nodes; else -1
	int padding;
};

#if !defined(__cplusplus)

bool gi_octree_is_valid_position(GI_OctreeNode node, vec3 position)
{
	return all(greaterThanEqual(position, node.min.xyz)) && all(lessThanEqual(position, node.max.xyz));
}

int gi_octree_child_slot(GI_OctreeNode node, vec3 position)
{
	vec3 center = (node.min.xyz + node.max.xyz) * 0.5;
	int x = position.x < center.x ? 0 : 1;
	int y = position.y < center.y ? 0 : 1;
	int z = position.z < center.z ? 0 : 1;
	return x + y * 2 + z * 4;
}

#endif // !defined(__cplusplus)

#endif // GI_HELPERS_H
