#pragma once

#include <cmath>

#include "core/types.h"

// Clipmap placement for SDF GI (sdf_gi.h; tests/sdf_gi_math_tests.cpp).
// Mirrored by data/shaders/sdf_gi/sdf_common.h's SDF_LEVEL_* constants.
namespace SdfGi
{
	constexpr i32 LEVEL_COUNT = 4;		// SDF_LEVEL_COUNT
	constexpr i32 LEVEL_RES = 128;		// SDF_LEVEL_RES
	constexpr i32 SNAP_VOXELS = 8;		// level origins move in steps of this many voxels

	// Camera-centred, snapped so a level only moves (and rebuilds) every SNAP_VOXELS voxels.
	inline HMM_Vec3 level_target_origin(HMM_Vec3 in_camera, f32 in_voxel_size)
	{
		const f32 snap = in_voxel_size * (f32) SNAP_VOXELS;
		const f32 half = in_voxel_size * (f32) (LEVEL_RES / 2);
		return HMM_V3(
			floorf(in_camera.X / snap) * snap - half,
			floorf(in_camera.Y / snap) * snap - half,
			floorf(in_camera.Z / snap) * snap - half);
	}

	inline bool level_overlaps(const HMM_Vec3 in_origin, f32 in_voxel_size, const BoundingBox& in_box)
	{
		const f32 extent = in_voxel_size * (f32) LEVEL_RES;
		return in_box.min.X <= in_origin.X + extent && in_box.max.X >= in_origin.X
			&& in_box.min.Y <= in_origin.Y + extent && in_box.max.Y >= in_origin.Y
			&& in_box.min.Z <= in_origin.Z + extent && in_box.max.Z >= in_origin.Z;
	}
}
