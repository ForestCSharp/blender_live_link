#pragma once

#include "shader_common.h"

#if !defined(__cplusplus) || !defined(__STDC__)
@block gi_helpers
#endif

#define GI_CELL_EXTENT 2.0

#define GI_CELL_DIMENSIONS 8
#define GI_CELL_DIMENSIONS_SQUARED (GI_CELL_DIMENSIONS * GI_CELL_DIMENSIONS)

#define GI_PROBE_DIMENSIONS (GI_CELL_DIMENSIONS + 1)
#define GI_PROBE_DIMENSIONS_SQUARED (GI_PROBE_DIMENSIONS * GI_PROBE_DIMENSIONS)

#define GI_SCENE_EXTENT = GI_CELL_EXTENT * GI_CELL_DIMENSIONS

#define GI_CELL_COUNT (GI_CELL_DIMENSIONS * GI_CELL_DIMENSIONS * GI_CELL_DIMENSIONS)
#define GI_PROBE_COUNT (GI_PROBE_DIMENSIONS * GI_PROBE_DIMENSIONS * GI_PROBE_DIMENSIONS)

#define GI_PROBE_MAX_DIST (GI_CELL_EXTENT * 1.7320508)

const vec3 GI_SCENE_CENTER = HMM_V3(0,0,0);
const vec3 GI_SCENE_MIN = HMM_V3(
	GI_SCENE_CENTER[0] - (GI_CELL_EXTENT * GI_CELL_DIMENSIONS) / 2.0f,
	GI_SCENE_CENTER[1] - (GI_CELL_EXTENT * GI_CELL_DIMENSIONS) / 2.0f,
	GI_SCENE_CENTER[2] - (GI_CELL_EXTENT * GI_CELL_DIMENSIONS) / 2.0f
);

struct GI_Coords
{
	int x,y,z;
};

//FCS TODO: TEST ALL THESE FUNCTIONS

GI_Coords gi_cell_coords_from_position(const vec3 in_position)
{
	const vec3 adjusted_position = in_position - GI_SCENE_MIN;
	GI_Coords out_coords;
	out_coords.x = int(adjusted_position[0] / GI_CELL_EXTENT);
	out_coords.y = int(adjusted_position[1] / GI_CELL_EXTENT);
	out_coords.z = int(adjusted_position[2] / GI_CELL_EXTENT);
	return out_coords;
}

GI_Coords gi_cell_coords_from_index(const int in_index)
{
	GI_Coords out_coords;
	out_coords.x = in_index % GI_CELL_DIMENSIONS;
	out_coords.y = (in_index / GI_CELL_DIMENSIONS) % GI_CELL_DIMENSIONS;
	out_coords.z = in_index / GI_CELL_DIMENSIONS_SQUARED;
	return out_coords;
}

GI_Coords gi_probe_coords_from_index(const int in_index)
{
	GI_Coords out_coords;
	out_coords.x = in_index % GI_PROBE_DIMENSIONS;
	out_coords.y = (in_index / GI_PROBE_DIMENSIONS) % GI_PROBE_DIMENSIONS;
	out_coords.z = in_index / GI_PROBE_DIMENSIONS_SQUARED;
	return out_coords;
}

int gi_cell_index_from_coords(const GI_Coords in_coords)
{
	return in_coords.x + in_coords.y * GI_CELL_DIMENSIONS + in_coords.z * GI_CELL_DIMENSIONS_SQUARED;
}

vec3 gi_cell_center_from_coords(const GI_Coords in_coords)
{
	return GI_SCENE_MIN + HMM_V3(
		(in_coords.x + 0.5f) * GI_CELL_EXTENT,
		(in_coords.y + 0.5f) * GI_CELL_EXTENT,
		(in_coords.z + 0.5f) * GI_CELL_EXTENT
	);
}

vec3 gi_probe_position_from_coords(const GI_Coords in_coords)
{
	return GI_SCENE_MIN + HMM_V3(
		in_coords.x * GI_CELL_EXTENT,
		in_coords.y * GI_CELL_EXTENT,
		in_coords.z * GI_CELL_EXTENT
	);
}

vec3 gi_probe_position_from_index(const int in_probe_index)
{
	GI_Coords probe_coords = gi_probe_coords_from_index(in_probe_index);
	return gi_probe_position_from_coords(probe_coords);
}

struct GI_Cell
{
	int probe_indices[8];	
};

struct GI_Probe
{
	int atlas_idx;
};

#if !defined(__cplusplus) || !defined(__STDC__)
@end // @block gi_helpers
#endif
