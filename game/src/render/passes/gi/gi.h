#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>

#include "ankerl/unordered_dense.h"
#include "core/types.h"
#include "core/dynamic_array.h"
#include "game_object/game_object.h"
#include "render/core/geometry_arena_freelist.h"
#include "render/passes/gi/gi_bake.h"
#include "render/passes/gi/gi_triangle_box.h"
#include "render/passes/gi/lighting_capture.h"

#include "gi/gi_helpers.h"

// GI probe scene: a sparse octree over the
// static scene geometry, probes at occupied-cell corners, radiance captured
// a few probes per frame into an octahedral atlas (+ optional SH9/SG9).
// The debug probe visualization lives separately in gi_debug_pass.h.

static_assert(sizeof(GI_Cell) == 32, "GI_Cell must match shader storage layout.");
static_assert(sizeof(GI_Probe) == 32, "GI_Probe must match shader storage layout.");
static_assert(sizeof(GI_OctreeNode) == 80, "GI_OctreeNode must match shader storage layout.");

struct GI_SG9_Lobe
{
	HMM_Vec4 params;
	HMM_Vec4 amplitude;
};

struct GI_Scene
{
	DynamicArray<GI_OctreeNode> octree_nodes;
	DynamicArray<GI_Cell> cells;
	DynamicArray<GI_Probe> probes;
	DynamicArray<HMM_Vec4> sh9_coefficients;
	DynamicArray<GI_SG9_Lobe> sg9_lobes;

	GpuBuffer<GI_OctreeNode> octree_nodes_buffer;
	GpuBuffer<GI_Probe> probes_buffer;
	GpuBuffer<GI_Cell> cells_buffer;
	GpuBuffer<HMM_Vec4> sh9_coefficients_buffer;
	GpuBuffer<GI_SG9_Lobe> sg9_lobes_buffer;

	// Probe Update State
	LightingCapture lighting_capture;
	i32 probe_idx_to_update = 0;
	bool pending_save = false;	// bake finished last frame; read back and save

	// Probes to recapture after an object edit (State::GiState::dirty_regions),
	// drained a few per frame; pending flags keep a drag from re-queuing them.
	DynamicArray<u8> probe_recapture_pending;
	DynamicArray<i32> recapture_queue;
	i32 recapture_cursor = 0;

	// Relighting: slot-row cursor over all probes, plus a cursor over the
	// specular probes (specular_probes[slot] = probe index).
	i32 relight_row_cursor = 0;
	i32 specular_relight_cursor = 0;
	DynamicArray<i32> specular_probes;
	DynamicArray<i32> specular_slots;	// 0..n-1, so a cursor range is a contiguous pointer pair
	GeometryContentHash probe_layout_hash;	// probes as built, before capture

	// Dynamic Layout
	BoundingBox scene_bounds = {};
	i32 octree_depth = 4;
	i32 leaf_divisions = 16;
	i32 fallback_probe_index = -1;
	i32 non_fallback_probe_count = 0;
	i32 payload_count = 0;
	f32 leaf_cell_extent = 1.0f;
	f32 max_radial_depth = 4.0f;
	f32 min_occupied_cell_extent = 0.0f;
	f32 max_occupied_cell_extent = 0.0f;

	// Static Parameters
	static constexpr i32 min_octree_depth = 2;
	static constexpr i32 max_octree_depth = 12;
	static constexpr f32 fallback_scene_extent = 30.0f;
	static constexpr f32 minimum_scene_extent = 1.0f;
	static constexpr f32 radial_depth_cell_scale = (f32) GI_RADIAL_DEPTH_CELL_SCALE;
	static constexpr i32 cubemap_capture_size = 64; // ~2x the face size a 48^2 octahedral specular entry needs
	static constexpr i32 irradiance_atlas_entry_size = 10;	// 6^2 interior inside the 2-texel gutter
	static constexpr i32 moments_atlas_entry_size = 16;
	static constexpr i32 specular_atlas_entry_size = 48;
	static constexpr i32 specular_atlas_mip_count = 4;
	// Every atlas is sized to its probe count at layout rebuild and kept within
	// 4096^2, Vulkan's guaranteed maximum. The octree depth is capped at the
	// deepest level that fits both budgets (gi_scene_rebuild_layout).
	static constexpr i32 maximum_image_size = 4096;
	// Diffuse budget: the moments atlas (the larger entry) caps the slot grid.
	static constexpr i32 maximum_octahedral_slots_per_dimension = maximum_image_size / moments_atlas_entry_size;
	static constexpr i32 maximum_lattice_probe_count =
		maximum_octahedral_slots_per_dimension * maximum_octahedral_slots_per_dimension;
	// Specular budget: probes on coarse (depth D-1) cell corners, plus the fallback.
	static constexpr i32 maximum_specular_slots_per_dimension = maximum_image_size / specular_atlas_entry_size;
	static constexpr i32 maximum_specular_probe_count =
		maximum_specular_slots_per_dimension * maximum_specular_slots_per_dimension;
	static constexpr i32 maximum_specular_atlas_size =
		maximum_specular_slots_per_dimension * specular_atlas_entry_size;
	static constexpr i32 probes_to_update_per_frame = 4;		// geometry captures (bake)
	static constexpr i32 relight_probes_per_frame = 2048;		// whole slot rows, rounded up
	static constexpr i32 relight_specular_probes_per_frame = 256;
};
static_assert(GI_Scene::maximum_octahedral_slots_per_dimension * GI_Scene::irradiance_atlas_entry_size
	<= GI_Scene::maximum_image_size, "Irradiance atlas must fit the image size limit.");
static_assert(GI_Scene::maximum_specular_atlas_size <= GI_Scene::maximum_image_size,
	"Specular atlas must fit the image size limit.");

#define GI_LOG_SCENE_INIT 0
#define GI_LOG_SCENE_UPDATE 0

i32 gi_scene_atlas_capacity()
{
	return GI_Scene::maximum_lattice_probe_count;
}

i32 gi_scene_leaf_divisions_from_depth(const i32 in_octree_depth)
{
	return 1 << in_octree_depth;
}

HMM_Vec3 gi_scene_bounds_center(const BoundingBox& in_bounds)
{
	return (in_bounds.min + in_bounds.max) * 0.5f;
}

BoundingBox gi_scene_cube_bounds_from_center_extent(const HMM_Vec3 in_center, const f32 in_extent)
{
	const HMM_Vec3 half_extent = HMM_V3(in_extent, in_extent, in_extent) * 0.5f;
	return (BoundingBox) {
		.min = in_center - half_extent,
		.max = in_center + half_extent,
	};
}

BoundingBox gi_scene_expand_bounds_to_cube(const BoundingBox& in_bounds)
{
	const HMM_Vec3 extent = in_bounds.max - in_bounds.min;
	f32 cube_extent = std::max(extent.X, std::max(extent.Y, extent.Z));
	if (!std::isfinite(cube_extent) || cube_extent < GI_Scene::minimum_scene_extent)
	{
		cube_extent = GI_Scene::minimum_scene_extent;
	}
	return gi_scene_cube_bounds_from_center_extent(gi_scene_bounds_center(in_bounds), cube_extent);
}

bool gi_scene_collect_visible_mesh_bounds(
	const State& in_state,
	DynamicArray<BoundingBox>& out_geometry_bounds,
	BoundingBox& out_scene_bounds)
{
	out_geometry_bounds.reset();
	out_scene_bounds = bounding_box_init();

	// Objects marked exclude_from_gi_bounds (e.g. a huge ground plane) don't
	// size the volume; they still place probes inside it, are captured and
	// cast GI shadows. If every GI object is excluded, include them all, so
	// excluding the only object can't empty the GI.
	bool any_included = false;
	for (auto const& [unique_id, object] : in_state.scene.objects)
	{
		any_included = any_included || (object_contributes_to_gi_scene(object)
			&& !object.rendering_properties.exclude_from_gi_bounds);
	}

	for (auto const& [unique_id, object] : in_state.scene.objects)
	{
		if (!object_contributes_to_gi_scene(object))
		{
			continue;
		}
		if (any_included && object.rendering_properties.exclude_from_gi_bounds)
		{
			printf("GI bounds: excluding '%s'\n", object.name ? object.name : "?");
			continue;
		}

		const BoundingBox object_bounds = object_get_bounding_box(object);
		out_geometry_bounds.add(object_bounds);
		bounding_box_expand(out_scene_bounds, object_bounds.min);
		bounding_box_expand(out_scene_bounds, object_bounds.max);
	}

	if (out_geometry_bounds.length() == 0)
	{
		out_scene_bounds = gi_scene_cube_bounds_from_center_extent(HMM_V3(0.0f, 0.0f, 0.0f), GI_Scene::fallback_scene_extent);
		return false;
	}

	out_scene_bounds = gi_scene_expand_bounds_to_cube(out_scene_bounds);
	return true;
}

HMM_Vec3 gi_scene_lattice_position(const BoundingBox& in_bounds, const i32 in_leaf_divisions, const GI_Coords in_coords)
{
	const f32 cell_extent = (in_bounds.max.X - in_bounds.min.X) / (f32) in_leaf_divisions;
	return in_bounds.min + HMM_V3(
		(f32) in_coords.x * cell_extent,
		(f32) in_coords.y * cell_extent,
		(f32) in_coords.z * cell_extent
	);
}

void gi_scene_reset_layout_buffers(GI_Scene& in_gi_scene)
{
	in_gi_scene.octree_nodes.reset();
	in_gi_scene.cells.reset();
	in_gi_scene.probes.reset();
	in_gi_scene.sh9_coefficients.reset();
	in_gi_scene.sg9_lobes.reset();
	in_gi_scene.fallback_probe_index = -1;
	in_gi_scene.non_fallback_probe_count = 0;
	in_gi_scene.payload_count = 0;
	in_gi_scene.min_occupied_cell_extent = 0.0f;
	in_gi_scene.max_occupied_cell_extent = 0.0f;
	in_gi_scene.max_radial_depth = 0.0f;
}

void gi_scene_destroy_layout_gpu_resources(GI_Scene& in_gi_scene)
{
	in_gi_scene.octree_nodes_buffer.destroy_gpu_buffer();
	in_gi_scene.probes_buffer.destroy_gpu_buffer();
	in_gi_scene.cells_buffer.destroy_gpu_buffer();
	in_gi_scene.sh9_coefficients_buffer.destroy_gpu_buffer();
	in_gi_scene.sg9_lobes_buffer.destroy_gpu_buffer();
}

u64 gi_scene_probe_corner_key(const GI_Coords in_coords)
{
	assert(in_coords.x >= 0 && in_coords.y >= 0 && in_coords.z >= 0);
	constexpr u64 coord_mask = (1ull << 21ull) - 1ull;
	return (((u64) in_coords.x & coord_mask) << 42ull) |
		(((u64) in_coords.y & coord_mask) << 21ull) |
		((u64) in_coords.z & coord_mask);
}

f32 gi_scene_cell_extent_from_depth(const GI_Scene& in_gi_scene, const i32 in_depth)
{
	const i32 divisions = 1 << in_depth;
	return (in_gi_scene.scene_bounds.max.X - in_gi_scene.scene_bounds.min.X) / (f32) divisions;
}

f32 gi_scene_max_radial_depth_from_cell_extent(const f32 in_cell_extent)
{
	return std::max(in_cell_extent * GI_Scene::radial_depth_cell_scale, GI_Scene::minimum_scene_extent);
}

GI_Probe gi_scene_make_probe(
	const HMM_Vec3 in_position,
	const f32 in_max_radial_depth,
	const i32 in_octree_level)
{
	GI_Probe probe = {};
	probe.position = HMM_V4V(in_position, 1.0f);
	probe.atlas_idx = -1;
	probe.max_radial_depth = in_max_radial_depth;
	probe.octree_level = in_octree_level;
	probe.specular_atlas_idx = -1;
	return probe;
}

void gi_scene_init_empty_cell_sentinel(GI_Scene& out_gi_scene)
{
	GI_Cell cell = {};
	for (i32 i = 0; i < 8; ++i)
	{
		cell.probe_indices[i] = -1;
	}
	out_gi_scene.cells.add(cell);
}

void gi_scene_add_fallback_probe(GI_Scene& out_gi_scene)
{
	out_gi_scene.non_fallback_probe_count = (i32) out_gi_scene.probes.length();
	out_gi_scene.fallback_probe_index = out_gi_scene.non_fallback_probe_count;

	const f32 fallback_radial_depth = gi_scene_max_radial_depth_from_cell_extent(out_gi_scene.scene_bounds.max.X - out_gi_scene.scene_bounds.min.X);
	out_gi_scene.probes.add(gi_scene_make_probe(
		gi_scene_bounds_center(out_gi_scene.scene_bounds), fallback_radial_depth, -1));
	out_gi_scene.max_radial_depth = std::max(out_gi_scene.max_radial_depth, fallback_radial_depth);
}

// World-space triangles the probe octree subdivides on; padding widens the
// cells a triangle may claim (tessellation displacement, as in culling).
struct GiTriangle
{
	HMM_Vec3 v[3];
	HMM_Vec3 min;	// bounds of v, widened by padding
	HMM_Vec3 max;
	f32 padding;
};

void gi_scene_collect_triangles(const State& in_state, DynamicArray<GiTriangle>& out_triangles)
{
	out_triangles.reset();
	const f32 tessellation_padding = in_state.tessellation.enabled ? in_state.tessellation.bounds_padding : 0.0f;
	for (auto const& [unique_id, object] : in_state.scene.objects)
	{
		if (!object_contributes_to_gi_scene(object) || !object.mesh.vertices || !object.mesh.indices)
		{
			continue;
		}
		const Mesh& mesh = object.mesh;
		const HMM_Mat4 model = object_get_model_matrix(object);
		const f32 padding = object.has_tessellation ? tessellation_padding : 0.0f;
		for (u32 index = 0; index + 2 < mesh.index_count; index += 3)
		{
			GiTriangle triangle;
			for (i32 corner = 0; corner < 3; ++corner)
			{
				triangle.v[corner] = HMM_MulM4V4(model, mesh.vertices[mesh.indices[index + corner]].position).XYZ;
			}
			triangle.padding = padding;
			const HMM_Vec3 pad = HMM_V3(padding, padding, padding);
			triangle.min = HMM_V3(
				std::min({ triangle.v[0].X, triangle.v[1].X, triangle.v[2].X }),
				std::min({ triangle.v[0].Y, triangle.v[1].Y, triangle.v[2].Y }),
				std::min({ triangle.v[0].Z, triangle.v[1].Z, triangle.v[2].Z })) - pad;
			triangle.max = HMM_V3(
				std::max({ triangle.v[0].X, triangle.v[1].X, triangle.v[2].X }),
				std::max({ triangle.v[0].Y, triangle.v[1].Y, triangle.v[2].Y }),
				std::max({ triangle.v[0].Z, triangle.v[1].Z, triangle.v[2].Z })) + pad;
			out_triangles.add(triangle);
		}
	}
}

// One octree build attempt at a fixed depth.
struct GiOctreeBuild
{
	const DynamicArray<GiTriangle>* triangles = nullptr;
	ankerl::unordered_dense::map<u64, i32> probe_indices_by_corner;
	f32 cell_epsilon = 0.0f;	// slack so off-plane G-buffer positions stay in an occupied cell
	i32 specular_depth = 0;		// depth of the coarse cells whose corners carry specular
	i32 specular_count = 0;		// specular slots handed out so far (coarse cell corners)
	bool over_budget = false;	// set past either probe budget; unwinds the build
	bool over_specular_budget = false;	// which budget tripped (else diffuse)
};

bool gi_scene_triangle_overlaps_cell(const GiOctreeBuild& in_build, const GiTriangle& in_triangle, const BoundingBox& in_cell)
{
	const f32 slack = in_triangle.padding + in_build.cell_epsilon;
	return gi_triangle_box_overlap(
		(in_cell.min + in_cell.max) * 0.5f,
		(in_cell.max - in_cell.min) * 0.5f + HMM_V3(slack, slack, slack),
		in_triangle.v[0], in_triangle.v[1], in_triangle.v[2]);
}

BoundingBox gi_scene_child_bounds(const BoundingBox& in_bounds, const i32 in_child_x, const i32 in_child_y, const i32 in_child_z)
{
	const HMM_Vec3 center = (in_bounds.min + in_bounds.max) * 0.5f;
	return (BoundingBox) {
		.min = HMM_V3(
			in_child_x == 0 ? in_bounds.min.X : center.X,
			in_child_y == 0 ? in_bounds.min.Y : center.Y,
			in_child_z == 0 ? in_bounds.min.Z : center.Z
		),
		.max = HMM_V3(
			in_child_x == 0 ? center.X : in_bounds.max.X,
			in_child_y == 0 ? center.Y : in_bounds.max.Y,
			in_child_z == 0 ? center.Z : in_bounds.max.Z
		),
	};
}

i32 gi_scene_get_or_create_probe(
	GI_Scene& out_gi_scene,
	GiOctreeBuild& io_build,
	const GI_Coords in_max_depth_coords,
	const f32 in_required_radial_depth,
	const i32 in_octree_level)
{
	const u64 probe_key = gi_scene_probe_corner_key(in_max_depth_coords);
	auto existing_probe = io_build.probe_indices_by_corner.find(probe_key);
	if (existing_probe != io_build.probe_indices_by_corner.end())
	{
		GI_Probe& probe = out_gi_scene.probes[existing_probe->second];
		probe.max_radial_depth = std::max(probe.max_radial_depth, in_required_radial_depth);
		probe.octree_level = std::min(probe.octree_level, in_octree_level);
		return existing_probe->second;
	}

	const i32 probe_index = (i32) out_gi_scene.probes.length();
	const HMM_Vec3 probe_position = gi_scene_lattice_position(out_gi_scene.scene_bounds, out_gi_scene.leaf_divisions, in_max_depth_coords);
	out_gi_scene.probes.add(gi_scene_make_probe(
		probe_position, in_required_radial_depth, in_octree_level));
	io_build.probe_indices_by_corner[probe_key] = probe_index;
	// Leave a slot for the fallback probe added after the build.
	if (out_gi_scene.probes.length() >= (size_t) GI_Scene::maximum_lattice_probe_count)
	{
		io_build.over_budget = true;
	}
	return probe_index;
}

i32 gi_scene_add_node_payload(
	GI_Scene& out_gi_scene,
	GiOctreeBuild& io_build,
	const i32 in_depth,
	const GI_Coords in_coords)
{
	const i32 coord_scale = 1 << (out_gi_scene.octree_depth - in_depth);
	const f32 cell_extent = gi_scene_cell_extent_from_depth(out_gi_scene, in_depth);
	const f32 required_radial_depth = gi_scene_max_radial_depth_from_cell_extent(cell_extent);

	if (out_gi_scene.payload_count == 0)
	{
		out_gi_scene.min_occupied_cell_extent = cell_extent;
		out_gi_scene.max_occupied_cell_extent = cell_extent;
	}
	else
	{
		out_gi_scene.min_occupied_cell_extent = std::min(out_gi_scene.min_occupied_cell_extent, cell_extent);
		out_gi_scene.max_occupied_cell_extent = std::max(out_gi_scene.max_occupied_cell_extent, cell_extent);
	}
	out_gi_scene.max_radial_depth = std::max(out_gi_scene.max_radial_depth, required_radial_depth);

	GI_Cell cell = {};
	for (i32 corner_z = 0; corner_z < 2; ++corner_z)
	{
		for (i32 corner_y = 0; corner_y < 2; ++corner_y)
		{
			for (i32 corner_x = 0; corner_x < 2; ++corner_x)
			{
				const GI_Coords probe_coords = {
					(in_coords.x + corner_x) * coord_scale,
					(in_coords.y + corner_y) * coord_scale,
					(in_coords.z + corner_z) * coord_scale,
				};
				const i32 corner_index = corner_x + corner_y * 2 + corner_z * 4;
				cell.probe_indices[corner_index] = gi_scene_get_or_create_probe(
					out_gi_scene,
					io_build,
					probe_coords,
					required_radial_depth,
					in_depth);
			}
		}
	}

	const i32 payload_index = (i32) out_gi_scene.cells.length();
	out_gi_scene.cells.add(cell);
	out_gi_scene.payload_count += 1;
	return payload_index;
}

GI_OctreeNode gi_scene_make_octree_node(const BoundingBox& in_bounds)
{
	GI_OctreeNode node = {};
	node.min = HMM_V4V(in_bounds.min, 1.0f);
	node.max = HMM_V4V(in_bounds.max, 1.0f);
	node.is_leaf = 1;
	node.payload_index = -1;
	node.specular_payload_index = -1;
	node.padding = 0;
	for (i32 i = 0; i < 8; ++i)
	{
		node.child_indices[i] = -1;
	}
	return node;
}

void gi_scene_init_empty_octree(GI_Scene& out_gi_scene)
{
	out_gi_scene.octree_nodes.add(gi_scene_make_octree_node(out_gi_scene.scene_bounds));
	gi_scene_init_empty_cell_sentinel(out_gi_scene);
}

// Subdivides only cells that a triangle overlaps; in_triangle_indices are the
// triangles overlapping in_bounds. Only max-depth cells get a payload (8 corner
// probes): every surface point lies in one, and the shader lookup returns -1
// (sky fallback probe) for points that stop in an empty part of the tree.
i32 gi_scene_build_sparse_octree_node(
	GI_Scene& out_gi_scene,
	GiOctreeBuild& io_build,
	const DynamicArray<u32>& in_triangle_indices,
	const BoundingBox& in_bounds,
	const i32 in_depth,
	const GI_Coords in_coords)
{
	GI_OctreeNode node = gi_scene_make_octree_node(in_bounds);
	if (in_depth >= out_gi_scene.octree_depth)
	{
		node.payload_index = gi_scene_add_node_payload(out_gi_scene, io_build, in_depth, in_coords);
	}
	else if (in_depth == io_build.specular_depth)
	{
		// Specular lives on coarser cells than diffuse (one level up unless the
		// specular budget pushed it further; see gi_scene_rebuild_layout): these 8 corners carry it
		// (slots assigned below). They are full probes because the
		// shader weights specular by each corner's radial-depth moments too.
		node.specular_payload_index = gi_scene_add_node_payload(out_gi_scene, io_build, in_depth, in_coords);
		// Slots in build order: deterministic, so the layout hash and bakes are stable.
		for (const i32 probe_index : out_gi_scene.cells[node.specular_payload_index].probe_indices)
		{
			if (out_gi_scene.probes[probe_index].specular_atlas_idx < 0)
			{
				out_gi_scene.probes[probe_index].specular_atlas_idx = io_build.specular_count++;
			}
		}
		// Leave a slot for the fallback probe.
		if (io_build.specular_count >= GI_Scene::maximum_specular_probe_count)
		{
			io_build.over_budget = true;
			io_build.over_specular_budget = true;
		}
	}

	const i32 node_index = (i32) out_gi_scene.octree_nodes.length();
	out_gi_scene.octree_nodes.add(node);

	if (in_depth >= out_gi_scene.octree_depth)
	{
		return node_index;
	}

	// Sort triangles into children by which side of the center planes their
	// (slack-widened) bounds fall. A triangle confined to one octant overlaps
	// that child for certain (it overlaps this cell, and its overlap lies in the
	// octant); only triangles straddling a center plane need the exact test.
	const DynamicArray<GiTriangle>& triangles = *io_build.triangles;
	const HMM_Vec3 center = (in_bounds.min + in_bounds.max) * 0.5f;
	DynamicArray<u32> child_triangle_indices[8];
	for (u32 triangle_index : in_triangle_indices)
	{
		const GiTriangle& triangle = triangles[triangle_index];
		const HMM_Vec3 slack = HMM_V3(io_build.cell_epsilon, io_build.cell_epsilon, io_build.cell_epsilon);
		const HMM_Vec3 lo = triangle.min - slack;
		const HMM_Vec3 hi = triangle.max + slack;
		const i32 x0 = lo.X > center.X ? 1 : 0, x1 = hi.X >= center.X ? 1 : 0;
		const i32 y0 = lo.Y > center.Y ? 1 : 0, y1 = hi.Y >= center.Y ? 1 : 0;
		const i32 z0 = lo.Z > center.Z ? 1 : 0, z1 = hi.Z >= center.Z ? 1 : 0;
		const bool single_octant = x0 == x1 && y0 == y1 && z0 == z1;
		for (i32 child_z = z0; child_z <= z1; ++child_z)
		{
			for (i32 child_y = y0; child_y <= y1; ++child_y)
			{
				for (i32 child_x = x0; child_x <= x1; ++child_x)
				{
					if (single_octant || gi_scene_triangle_overlaps_cell(
						io_build, triangle, gi_scene_child_bounds(in_bounds, child_x, child_y, child_z)))
					{
						child_triangle_indices[child_x + child_y * 2 + child_z * 4].add(triangle_index);
					}
				}
			}
		}
	}

	i32 child_count = 0;
	for (i32 child_z = 0; child_z < 2; ++child_z)
	{
		for (i32 child_y = 0; child_y < 2; ++child_y)
		{
			for (i32 child_x = 0; child_x < 2; ++child_x)
			{
				if (io_build.over_budget)
				{
					return node_index;
				}
				const i32 child_slot = child_x + child_y * 2 + child_z * 4;
				if (child_triangle_indices[child_slot].length() == 0)
				{
					continue;
				}
				const BoundingBox child_bounds = gi_scene_child_bounds(in_bounds, child_x, child_y, child_z);

				const GI_Coords child_coords = {
					in_coords.x * 2 + child_x,
					in_coords.y * 2 + child_y,
					in_coords.z * 2 + child_z,
				};
				const i32 child_index = gi_scene_build_sparse_octree_node(
					out_gi_scene, io_build, child_triangle_indices[child_slot], child_bounds, in_depth + 1, child_coords);
				out_gi_scene.octree_nodes[node_index].child_indices[child_slot] = child_index;
				child_count += 1;
			}
		}
	}

	out_gi_scene.octree_nodes[node_index].is_leaf = child_count == 0 ? 1 : 0;
	return node_index;
}

void gi_scene_init_radiance_buffers(GI_Scene& out_gi_scene)
{
	out_gi_scene.sh9_coefficients.add_uninitialized(out_gi_scene.probes.length() * 9);
	out_gi_scene.sg9_lobes.add_uninitialized(out_gi_scene.probes.length() * 9);

	for (HMM_Vec4& coefficient : out_gi_scene.sh9_coefficients)
	{
		coefficient = HMM_V4(0, 0, 0, 0);
	}

	for (GI_SG9_Lobe& lobe : out_gi_scene.sg9_lobes)
	{
		lobe.params = HMM_V4(0, 0, 1, 1);
		lobe.amplitude = HMM_V4(0, 0, 0, 0);
	}
}

void gi_scene_recreate_gpu_buffers(GI_Scene& out_gi_scene)
{
	gi_scene_destroy_layout_gpu_resources(out_gi_scene);

	out_gi_scene.octree_nodes_buffer = GpuBuffer((GpuBufferDesc<GI_OctreeNode>){
		.data = out_gi_scene.octree_nodes.data(),
		.size = sizeof(GI_OctreeNode) * out_gi_scene.octree_nodes.length(),
		.usage = { .storage_buffer = true },
		.label = "GI Octree Nodes Buffer",
	});

	out_gi_scene.probes_buffer = GpuBuffer((GpuBufferDesc<GI_Probe>){
		.data = out_gi_scene.probes.data(),
		.size = sizeof(GI_Probe) * out_gi_scene.probes.length(),
		.usage = {
			.storage_buffer = true,
			.stream_update = true,	// atlas indices assigned as probes update
		},
		.label = "GI Probes Buffer",
	});

	out_gi_scene.cells_buffer = GpuBuffer((GpuBufferDesc<GI_Cell>){
		.data = out_gi_scene.cells.data(),
		.size = sizeof(GI_Cell) * out_gi_scene.cells.length(),
		.usage = { .storage_buffer = true },
		.label = "GI Cells Buffer",
	});

	out_gi_scene.sh9_coefficients_buffer = GpuBuffer((GpuBufferDesc<HMM_Vec4>){
		.data = out_gi_scene.sh9_coefficients.data(),
		.size = sizeof(HMM_Vec4) * out_gi_scene.sh9_coefficients.length(),
		.usage = { .storage_buffer = true, .transfer_src = true },	// GI bake readback
		.label = "GI SH9 Coefficients Buffer",
	});

	out_gi_scene.sg9_lobes_buffer = GpuBuffer((GpuBufferDesc<GI_SG9_Lobe>){
		.data = out_gi_scene.sg9_lobes.data(),
		.size = sizeof(GI_SG9_Lobe) * out_gi_scene.sg9_lobes.length(),
		.usage = { .storage_buffer = true, .transfer_src = true },	// GI bake readback
		.label = "GI SG9 Lobes Buffer",
	});
}

// ---- GI bake persistence (file format in gi_bake.h) ----

u32 gi_bake_format_bytes(const VkFormat in_format)
{
	switch (in_format)
	{
		case VK_FORMAT_R8G8B8A8_UNORM: return 4;
		case VK_FORMAT_R32G32_SFLOAT: return 8;
		case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
		case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
		default: assert(false && "GI bake: unhandled atlas format"); return 0;
	}
}

// Atlas index i is probe i, row-major, so probes occupy the leading rows.
// Both octahedral atlases share the slots-per-dimension grid.
u32 gi_bake_octahedral_rows(const GI_Scene& in_gi_scene, const i32 in_entry_size)
{
	const i32 entries_per_row = in_gi_scene.lighting_capture.octahedral_slots_per_dimension;
	const i32 entry_rows = ((i32) in_gi_scene.probes.length() + entries_per_row - 1) / entries_per_row;
	return (u32) (entry_rows * in_entry_size);
}

GiBakeHeader gi_bake_expected_header(GI_Scene& in_gi_scene, const State& in_state)
{
	LightingCapture& capture = in_gi_scene.lighting_capture;
	// Everything that changes how the stored bytes are laid out or interpreted.
	const i32 params[] = {
		in_gi_scene.octree_depth,
		capture.moments_atlas_total_size,
		capture.desc.moments_entry_size,
		(i32) capture.color_format,
		(i32) GI_CACHE_SURFACE_FORMAT,
		(i32) GI_CACHE_NORMAL_FORMAT,
		(i32) GI_CACHE_HIT_FORMAT,
		(i32) in_state.gi.probe_occlusion_mode,
	};
	const GeometryContentHash hash = geometry_arena_hash_bytes(
		params, sizeof(params), in_gi_scene.probe_layout_hash.low ^ in_gi_scene.probe_layout_hash.high);

	GiBakeHeader header;
	header.layout_hash[0] = hash.low;
	header.layout_hash[1] = hash.high;
	header.probe_count = in_gi_scene.probes.length();
	const u64 moments_grid_texels = (u64) capture.moments_atlas_total_size
		* gi_bake_octahedral_rows(in_gi_scene, capture.desc.moments_entry_size);
	header.blob_bytes[GI_BAKE_BLOB_MOMENTS] = moments_grid_texels * gi_bake_format_bytes(capture.color_format);
	header.blob_bytes[GI_BAKE_BLOB_CACHE_SURFACE] = moments_grid_texels * gi_bake_format_bytes(GI_CACHE_SURFACE_FORMAT);
	header.blob_bytes[GI_BAKE_BLOB_CACHE_NORMAL] = moments_grid_texels * gi_bake_format_bytes(GI_CACHE_NORMAL_FORMAT);
	header.blob_bytes[GI_BAKE_BLOB_CACHE_HIT] = moments_grid_texels * gi_bake_format_bytes(GI_CACHE_HIT_FORMAT);
	return header;
}

// Records copies between the four moments-grid atlases (moments + geometry
// cache) and a staging buffer laid out like the file's blobs; in_upload picks
// the direction. Leaves the atlases SHADER_READ_ONLY.
void gi_bake_record_atlas_copies(
	VkCommandBuffer in_command_buffer, GI_Scene& in_gi_scene, VkBuffer in_staging, const bool in_upload)
{
	LightingCapture& capture = in_gi_scene.lighting_capture;
	const VkImageLayout transfer_layout = in_upload
		? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	u64 offset = 0;
	const auto copy = [&](GpuImage& in_image, const u32 in_mip, const u32 in_width, const u32 in_height)
	{
		const VkBufferImageCopy region = {
			.bufferOffset = offset,
			.imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = in_mip, .baseArrayLayer = 0, .layerCount = 1 },
			.imageOffset = { 0, 0, 0 },
			.imageExtent = { in_width, in_height, 1 },
		};
		if (in_upload)
			vkCmdCopyBufferToImage(in_command_buffer, in_staging, in_image.image, transfer_layout, 1, &region);
		else
			vkCmdCopyImageToBuffer(in_command_buffer, in_image.image, transfer_layout, in_staging, 1, &region);
		offset += (u64) in_width * in_height * gi_bake_format_bytes(in_image.format);
	};

	const auto copy_octahedral = [&](GpuImage& in_atlas, const i32 in_total_size, const i32 in_entry_size)
	{
		gpu_image_transition(in_command_buffer, in_atlas, transfer_layout);
		copy(in_atlas, 0, (u32) in_total_size, gi_bake_octahedral_rows(in_gi_scene, in_entry_size));
		gpu_image_transition(in_command_buffer, in_atlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	};
	for (i32 output_idx = 0; output_idx < 4; ++output_idx)
	{
		copy_octahedral(capture.moments_oct_pass.get_color_output(output_idx),
			capture.moments_atlas_total_size, capture.desc.moments_entry_size);
	}
}

struct GiBakeStaging
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	void* mapped = nullptr;
};

GiBakeStaging gi_bake_create_staging(VulkanContext* ctx, const u64 in_size, const bool in_readback)
{
	const VkBufferCreateInfo buffer_create_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = in_size,
		.usage = in_readback ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	const VmaAllocationCreateInfo allocation_create_info = {
		.flags = (VmaAllocationCreateFlags) ((in_readback
			? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
			: VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT)
			| VMA_ALLOCATION_CREATE_MAPPED_BIT),
		.usage = VMA_MEMORY_USAGE_AUTO,
	};
	GiBakeStaging staging;
	VmaAllocationInfo allocation_info = {};
	VK_CHECK(vmaCreateBuffer(ctx->allocator, &buffer_create_info, &allocation_create_info,
		&staging.buffer, &staging.allocation, &allocation_info));
	staging.mapped = allocation_info.pMappedData;
	return staging;
}

// Called at the top of the frame after a bake's last capture was recorded:
// nothing GI-related is recorded yet this frame, so a wait-idle + one-shot
// readback sees the finished bake and leaves image layout tracking consistent.
void gi_scene_save_bake(VulkanContext* ctx, GI_Scene& in_gi_scene, State& in_state)
{
	const std::string path = gi_bake_path(in_state.gi.scene_key);
	if (!in_state.gi.bake_persistence || path.empty())
	{
		in_state.gi.bake_status = EGiBakeStatus::Baked;
		return;
	}

	const GiBakeHeader header = gi_bake_expected_header(in_gi_scene, in_state);
	const u64 total_bytes = gi_bake_total_blob_bytes(header);
	GiBakeStaging staging = gi_bake_create_staging(ctx, total_bytes, /*in_readback*/ true);
	VK_CHECK(vulkan_device_wait_idle(ctx));
	vulkan_context_immediate_submit(ctx, [&](VkCommandBuffer in_command_buffer)
	{
		VkMemoryBarrier2 to_transfer = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
		};
		VkDependencyInfo dependency = {
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &to_transfer,
		};
		vkCmdPipelineBarrier2(in_command_buffer, &dependency);

		gi_bake_record_atlas_copies(in_command_buffer, in_gi_scene, staging.buffer, /*in_upload*/ false);

		VkMemoryBarrier2 to_host = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
			.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
		};
		dependency.pMemoryBarriers = &to_host;
		vkCmdPipelineBarrier2(in_command_buffer, &dependency);
	});
	VK_CHECK(vmaInvalidateAllocation(ctx->allocator, staging.allocation, 0, VK_WHOLE_SIZE));
	const bool saved = gi_bake_write(path, header, staging.mapped);
	vmaDestroyBuffer(ctx->allocator, staging.buffer, staging.allocation);

	in_state.gi.bake_status = saved ? EGiBakeStatus::Saved : EGiBakeStatus::SaveFailed;
	printf("GI bake %s: %s (%.1f MiB)\n", saved ? "saved" : "SAVE FAILED", path.c_str(),
		(f64) total_bytes / (1024.0 * 1024.0));
}

// Looks for a bake matching the freshly built layout. On a hit, enables every
// probe (atlas_idx) and returns the file's blobs for
// gi_scene_upload_bake_atlases; relighting then fills in the lighting.
bool gi_scene_read_bake(GI_Scene& in_gi_scene, const State& in_state, DynamicArray<u8>& out_blobs, u64& out_atlas_bytes)
{
	const std::string path = gi_bake_path(in_state.gi.scene_key);
	if (!in_state.gi.bake_persistence || path.empty())
	{
		return false;
	}

	const GiBakeHeader header = gi_bake_expected_header(in_gi_scene, in_state);
	if (!gi_bake_read(path, header, out_blobs))
	{
		return false;
	}

	out_atlas_bytes = gi_bake_total_blob_bytes(header);
	for (i32 probe_index = 0; probe_index < (i32) in_gi_scene.probes.length(); ++probe_index)
	{
		in_gi_scene.probes[probe_index].atlas_idx = probe_index;
	}
	printf("GI bake loaded: %s\n", path.c_str());
	return true;
}

// Must run after resize_specular_atlas: that records a clear into the frame
// command buffer, so the upload is recorded after it in the same buffer.
void gi_scene_upload_bake_atlases(
	VulkanContext* ctx, GI_Scene& in_gi_scene, const DynamicArray<u8>& in_blobs, const u64 in_atlas_bytes)
{
	GiBakeStaging staging = gi_bake_create_staging(ctx, in_atlas_bytes, /*in_readback*/ false);
	memcpy(staging.mapped, in_blobs.data(), in_atlas_bytes);
	VK_CHECK(vmaFlushAllocation(ctx->allocator, staging.allocation, 0, in_atlas_bytes));
	if (vulkan_current_frame(ctx).recording)
	{
		gi_bake_record_atlas_copies(vulkan_current_command_buffer(ctx), in_gi_scene, staging.buffer, /*in_upload*/ true);
		vulkan_context_retire(ctx, (RetiredResource) {
			.type = RetiredResourceType::Buffer,
			.buffer = staging.buffer,
			.allocation = staging.allocation,
		});
	}
	else
	{
		vulkan_context_immediate_submit(ctx, [&](VkCommandBuffer in_command_buffer)
		{
			gi_bake_record_atlas_copies(in_command_buffer, in_gi_scene, staging.buffer, /*in_upload*/ true);
		});
		vmaDestroyBuffer(ctx->allocator, staging.buffer, staging.allocation);
	}
}

void gi_scene_rebuild_layout(VulkanContext* ctx, GI_Scene& out_gi_scene, State& in_state)
{
	out_gi_scene.octree_depth = std::clamp(in_state.gi.octree_depth, GI_Scene::min_octree_depth, GI_Scene::max_octree_depth);
	DynamicArray<BoundingBox> geometry_bounds;
	const bool has_visible_geometry = gi_scene_collect_visible_mesh_bounds(in_state, geometry_bounds, out_gi_scene.scene_bounds);

	DynamicArray<GiTriangle> triangles;
	gi_scene_collect_triangles(in_state, triangles);
	// The octree build assumes every triangle handed to a node overlaps it, so
	// the root list keeps only triangles touching the GI cube; objects excluded
	// from the bounds can extend far outside it.
	DynamicArray<u32> all_triangle_indices;
	{
		const HMM_Vec3 cube_center = (out_gi_scene.scene_bounds.min + out_gi_scene.scene_bounds.max) * 0.5f;
		const HMM_Vec3 cube_half_extent = (out_gi_scene.scene_bounds.max - out_gi_scene.scene_bounds.min) * 0.5f;
		for (u32 triangle_index = 0; triangle_index < (u32) triangles.length(); ++triangle_index)
		{
			const GiTriangle& triangle = triangles[triangle_index];
			const HMM_Vec3 slack = HMM_V3(triangle.padding, triangle.padding, triangle.padding);
			if (gi_triangle_box_overlap(cube_center, cube_half_extent + slack, triangle.v[0], triangle.v[1], triangle.v[2]))
			{
				all_triangle_indices.add(triangle_index);
			}
		}
	}

	// Probe count depends on how the surfaces fill the octree, so a depth can
	// only be checked against the budget by building it. Try the requested
	// depth; a build aborts as soon as it passes the budget, so a failed attempt
	// costs about one budget's worth of probes, then step one level shallower.
	// The slider then shows the depth actually in use.
	const auto build_start = std::chrono::steady_clock::now();
	const i32 requested_depth = out_gi_scene.octree_depth;
	i32 specular_probe_count = 0;
	// Specular starts one level above the leaves and moves coarser while it is
	// what overflows; only a diffuse overflow lowers the octree depth.
	i32 specular_levels_up = 1;
	for (;;)
	{
		out_gi_scene.leaf_divisions = gi_scene_leaf_divisions_from_depth(out_gi_scene.octree_depth);
		out_gi_scene.leaf_cell_extent = (out_gi_scene.scene_bounds.max.X - out_gi_scene.scene_bounds.min.X) / (f32) out_gi_scene.leaf_divisions;
		gi_scene_reset_layout_buffers(out_gi_scene);
		GiOctreeBuild build;
		build.triangles = &triangles;
		build.specular_depth = out_gi_scene.octree_depth - specular_levels_up;
		build.cell_epsilon = 0.05f * out_gi_scene.leaf_cell_extent;
		if (has_visible_geometry && triangles.length() > 0)
		{
			gi_scene_build_sparse_octree_node(out_gi_scene, build, all_triangle_indices, out_gi_scene.scene_bounds, 0, (GI_Coords){0, 0, 0});
		}
		else
		{
			gi_scene_init_empty_octree(out_gi_scene);
		}
		specular_probe_count = build.specular_count;
		if (!build.over_budget || out_gi_scene.octree_depth == GI_Scene::min_octree_depth)
		{
			break;
		}
		if (build.over_specular_budget && build.specular_depth > 1)
		{
			printf("GI specular at depth %d needs more than %d probes; trying depth %d\n",
				build.specular_depth, GI_Scene::maximum_specular_probe_count, build.specular_depth - 1);
			specular_levels_up += 1;
			continue;
		}
		printf("GI octree depth %d needs more than %d probes; trying depth %d\n",
			out_gi_scene.octree_depth, GI_Scene::maximum_lattice_probe_count, out_gi_scene.octree_depth - 1);
		out_gi_scene.octree_depth -= 1;
		specular_levels_up = 1;
	}
	if (out_gi_scene.cells.length() == 0)
	{
		gi_scene_init_empty_cell_sentinel(out_gi_scene);	// keeps the cells buffer non-empty
	}
	gi_scene_add_fallback_probe(out_gi_scene);
	out_gi_scene.probes[out_gi_scene.fallback_probe_index].specular_atlas_idx = specular_probe_count++;
	// The fallback sees only sky, which is exactly what a cleared cache holds,
	// so it needs no capture: relighting alone keeps it current.
	out_gi_scene.probes[out_gi_scene.fallback_probe_index].atlas_idx = out_gi_scene.fallback_probe_index;
	out_gi_scene.specular_probes.reset();
	out_gi_scene.specular_slots.reset();
	out_gi_scene.specular_probes.resize(specular_probe_count, -1);
	for (i32 slot = 0; slot < specular_probe_count; ++slot)
	{
		out_gi_scene.specular_slots.add(slot);
	}
	for (i32 probe_index = 0; probe_index < (i32) out_gi_scene.probes.length(); ++probe_index)
	{
		const i32 specular_slot = out_gi_scene.probes[probe_index].specular_atlas_idx;
		if (specular_slot >= 0)
		{
			out_gi_scene.specular_probes[specular_slot] = probe_index;
		}
	}
	printf("GI layout: depth %d (requested %d), %d cells, %zu probes (%d with specular at depth %d), %zu/%zu triangles in volume, extent %.1f, %.1f ms\n",
		out_gi_scene.octree_depth, requested_depth, out_gi_scene.payload_count, out_gi_scene.probes.length(),
		specular_probe_count, out_gi_scene.octree_depth - specular_levels_up,
		all_triangle_indices.length(), triangles.length(),
		out_gi_scene.scene_bounds.max.X - out_gi_scene.scene_bounds.min.X,
		std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - build_start).count());
	in_state.gi.octree_depth = out_gi_scene.octree_depth;
	gi_scene_init_radiance_buffers(out_gi_scene);

	assert(out_gi_scene.probes.length() <= (size_t) GI_Scene::maximum_lattice_probe_count);
	assert(out_gi_scene.probes.length() <= (size_t) gi_scene_atlas_capacity());
	assert(specular_probe_count <= GI_Scene::maximum_specular_probe_count);
	assert(out_gi_scene.probes.length() > 0);
	assert(out_gi_scene.octree_nodes.length() > 0);
	assert(out_gi_scene.cells.length() > 0);
	out_gi_scene.probe_layout_hash = geometry_arena_hash_bytes(
		out_gi_scene.probes.data(), sizeof(GI_Probe) * out_gi_scene.probes.length(), 0x4749424bull);
	// Sized first: the bake header covers the atlas sizes and formats.
	out_gi_scene.lighting_capture.resize_octahedral_atlases(ctx, (i32) out_gi_scene.probes.length());
	out_gi_scene.lighting_capture.resize_specular_atlas(ctx, specular_probe_count);
	// Read before the GPU buffers exist so they are created with the baked data.
	DynamicArray<u8> bake_blobs;
	u64 bake_atlas_bytes = 0;
	const bool bake_loaded = gi_scene_read_bake(out_gi_scene, in_state, bake_blobs, bake_atlas_bytes);
	gi_scene_recreate_gpu_buffers(out_gi_scene);
	if (bake_loaded)
	{
		gi_scene_upload_bake_atlases(ctx, out_gi_scene, bake_blobs, bake_atlas_bytes);
	}

	// A rebuild invalidates any bake in progress; probes stay unbaked unless a
	// saved bake matches this exact layout.
	out_gi_scene.probe_idx_to_update = 0;
	out_gi_scene.pending_save = false;
	in_state.gi.layout_dirty = false;
	in_state.gi.is_updating = false;
	in_state.gi.isolated_probe_index = -1;
	in_state.gi.bake_status = bake_loaded ? EGiBakeStatus::Loaded : EGiBakeStatus::NotBaked;
	out_gi_scene.relight_row_cursor = 0;
	out_gi_scene.specular_relight_cursor = 0;
	in_state.gi.relight_passes_remaining = State::GiState::relight_pass_count;
	// Fresh layout: earlier edits are already in it.
	in_state.gi.dirty_regions.reset();
	out_gi_scene.recapture_queue.reset();
	out_gi_scene.recapture_cursor = 0;
	out_gi_scene.probe_recapture_pending.reset();
	for (size_t i = 0; i < out_gi_scene.probes.length(); ++i)
	{
		out_gi_scene.probe_recapture_pending.add(0);
	}

	if (GI_LOG_SCENE_INIT)
	{
		printf(
			"GI octree depth: %d nodes: %zu payloads: %d probes: %zu min/max cell extent: %f/%f max radial depth: %f\n",
			out_gi_scene.octree_depth,
			out_gi_scene.octree_nodes.length(),
			out_gi_scene.payload_count,
			out_gi_scene.probes.length(),
			out_gi_scene.min_occupied_cell_extent,
			out_gi_scene.max_occupied_cell_extent,
			out_gi_scene.max_radial_depth
		);
	}
}

f32 gi_scene_debug_probe_radius(const GI_Scene& in_gi_scene)
{
	return std::max(in_gi_scene.leaf_cell_extent * 0.1f, 0.025f);
}

bool gi_scene_debug_probe_matches_level_filter(
	const GI_Scene& in_gi_scene,
	const i32 in_probe_index,
	const bool in_filter_enabled,
	const i32 in_filter_selection)
{
	assert(in_gi_scene.probes.is_valid_index(in_probe_index));
	const i32 probe_level = in_gi_scene.probes[in_probe_index].octree_level;
	if (!in_filter_enabled)
	{
		return probe_level >= 0;
	}
	const i32 filter_selection = std::clamp(
		in_filter_selection, 0, in_gi_scene.octree_depth + 1);
	return filter_selection == 0
		? probe_level < 0
		: probe_level == filter_selection - 1;
}

HMM_Vec3 gi_scene_probe_position_from_index(GI_Scene& in_gi_scene, const i32 in_probe_index)
{
	assert(in_gi_scene.probes.is_valid_index(in_probe_index));
	return in_gi_scene.probes[in_probe_index].position.XYZ;
}

void gi_scene_init(VulkanContext* ctx, GI_Scene& out_gi_scene, State& in_state)
{
	const LightingCaptureDesc lighting_capture_desc = {
		.cubemap_render_size = GI_Scene::cubemap_capture_size,
		.irradiance_entry_size = GI_Scene::irradiance_atlas_entry_size,
		.moments_entry_size = GI_Scene::moments_atlas_entry_size,
		.specular_entry_size = GI_Scene::specular_atlas_entry_size,
		.specular_mip_count = GI_Scene::specular_atlas_mip_count,
	};
	assert((i32)ctx->capabilities.properties.limits.maxImageDimension2D
		>= GI_Scene::maximum_specular_atlas_size);
	out_gi_scene.lighting_capture.init(ctx, lighting_capture_desc);

	gi_scene_rebuild_layout(ctx, out_gi_scene, in_state);
}

// GI fields of lighting.frag's fs_params, shared by the main lighting pass
// and probe relighting.
void gi_scene_fill_lighting_fs_params(LightingFsParams& out_fs_params, GI_Scene& in_gi_scene, const State& in_state)
{
	LightingCapture& capture = in_gi_scene.lighting_capture;
	out_fs_params.gi_enable = in_state.gi.enable ? 1 : 0;
	out_fs_params.gi_probe_occlusion = in_state.gi.probe_occlusion ? 1 : 0;
	out_fs_params.probe_occlusion_mode = (i32) in_state.gi.probe_occlusion_mode;
	out_fs_params.probe_radiance_mode = (i32) in_state.gi.probe_radiance_mode;
	out_fs_params.probe_specular_enable = in_state.gi.probe_specular_enable ? 1 : 0;
	out_fs_params.specular_atlas_total_size = capture.specular_atlas_total_size;
	out_fs_params.specular_atlas_entry_size = capture.desc.specular_entry_size;
	out_fs_params.specular_mip_count = capture.desc.specular_mip_count;
	out_fs_params.gi_intensity = in_state.gi.intensity;
	out_fs_params.atlas_total_size = capture.irradiance_atlas_total_size;
	out_fs_params.atlas_entry_size = capture.desc.irradiance_entry_size;
	out_fs_params.moments_atlas_total_size = capture.moments_atlas_total_size;
	out_fs_params.moments_atlas_entry_size = capture.desc.moments_entry_size;
	out_fs_params.gi_fallback_probe_index = in_gi_scene.fallback_probe_index;
	out_fs_params.gi_octree_node_count = (i32) in_gi_scene.octree_nodes.length();
	out_fs_params.isolated_probe_index = in_state.gi.probe_isolation_enable
		? (in_state.gi.isolated_probe_index >= 0 ? in_state.gi.isolated_probe_index : -2)
		: -1;
}

// Relights the next band of slot rows (and the next batch of specular probes).
// A wrap of the row cursor completes one pass; each pass adds a bounce,
// because relighting samples the probes' current irradiance at each hit.
void gi_scene_relight_step(FrameRenderGraph& graph, VulkanContext* ctx, GI_Scene& in_gi_scene, State& in_state)
{
	LightingCapture& capture = in_gi_scene.lighting_capture;
	const i32 probe_count = (i32) in_gi_scene.probes.length();
	const i32 slots_per_row = capture.octahedral_slots_per_dimension;
	if (probe_count <= 0 || slots_per_row <= 0)
	{
		return;
	}
	const i32 total_rows = (probe_count + slots_per_row - 1) / slots_per_row;
	const i32 rows_per_frame = std::max(1, (GI_Scene::relight_probes_per_frame + slots_per_row - 1) / slots_per_row);
	in_gi_scene.relight_row_cursor = std::clamp(in_gi_scene.relight_row_cursor, 0, total_rows - 1);

	ProbeRelightBand band = {};
	band.first_row = in_gi_scene.relight_row_cursor;
	band.row_count = std::min(rows_per_frame, total_rows - band.first_row);
	band.first_probe = band.first_row * slots_per_row;
	band.probe_count = std::min(band.row_count * slots_per_row, probe_count - band.first_probe);

	const i32 specular_total = (i32) in_gi_scene.specular_probes.length();
	in_gi_scene.specular_relight_cursor = specular_total > 0 ? in_gi_scene.specular_relight_cursor % specular_total : 0;
	band.specular_count = std::min(GI_Scene::relight_specular_probes_per_frame, specular_total - in_gi_scene.specular_relight_cursor);
	band.specular_probes = in_gi_scene.specular_probes.data() + in_gi_scene.specular_relight_cursor;
	band.specular_slots = in_gi_scene.specular_slots.data() + in_gi_scene.specular_relight_cursor;

	LightingFsParams& fs = band.fs_params;
	gi_scene_fill_lighting_fs_params(fs, in_gi_scene, in_state);
	fs.probe_relight = 1;
	fs.gi_enable = 1;						// hit points sample the probes: one more bounce per pass
	fs.isolated_probe_index = -1;
	fs.ssao_enable = 0;
	fs.direct_lighting_enable = 1;
	fs.num_point_lights = (i32) in_state.lighting.point_lights.length();
	fs.num_spot_lights = (i32) in_state.lighting.spot_lights.length();
	fs.num_sun_lights = (i32) in_state.lighting.sun_lights.length();
	fs.atmosphere_sun_index = in_state.lighting.active_atmosphere_sun_index;
	fs.atmosphere_enabled = sky_pass.has_active_atmosphere
		&& bruneton_atmosphere_pass.has_precomputed
		&& in_state.lighting.active_atmosphere_sun_index >= 0 ? 1 : 0;
	fs.atmosphere_planet_center_z = sky_pass.active_parameters.planet_center_z_m;
	fs.cloud_shadow_extent_enabled = HMM_V4(8000.0f, 0.0f, 0.0f, 0.0f);
	fs.shadow_bias = 0.001f;
	fs.shadow_map_texel_size = HMM_V2(1.0f / GIShadowMap::resolution, 1.0f / GIShadowMap::resolution);
	if (GIShadowMap::valid)
	{
		// One centered-square cascade covering the whole GI scene.
		fs.shadow_map_enable = 1;
		fs.shadow_num_cascades = 1;
		fs.shadow_cascade_placement_mode = (i32) EShadowCascadePlacementMode::CenteredSquares;
		fs.shadow_view_projections[0] = GIShadowMap::view_projection;
	}

	band.probes_buffer = in_gi_scene.probes_buffer.get_gpu_buffer();
	band.cells_buffer = in_gi_scene.cells_buffer.get_gpu_buffer();
	band.octree_buffer = in_gi_scene.octree_nodes_buffer.get_gpu_buffer();
	band.sh9_buffer = in_gi_scene.sh9_coefficients_buffer.get_gpu_buffer();
	band.sg9_buffer = in_gi_scene.sg9_lobes_buffer.get_gpu_buffer();
	capture.relight_band(graph, ctx, in_state, band);

	in_gi_scene.specular_relight_cursor += band.specular_count;
	in_gi_scene.relight_row_cursor += band.row_count;
	if (in_gi_scene.relight_row_cursor >= total_rows)
	{
		in_gi_scene.relight_row_cursor = 0;
		in_state.gi.relight_passes_remaining -= 1;
	}
}

// Records this frame's probe captures into the command buffer: a few probes
// per frame while a requested bake runs, otherwise only the sky fallback probe
// when the sky changed. Call after begin_frame + descriptor updates, before the
// main pass chain executes.
void gi_scene_update(
	FrameRenderGraph& graph, VulkanContext* ctx, GI_Scene& in_gi_scene, State& in_state)
{
	in_gi_scene.lighting_capture.begin_frame(ctx);

	// Before anything GI-related is recorded this frame (see gi_scene_save_bake).
	if (in_gi_scene.pending_save)
	{
		in_gi_scene.pending_save = false;
		gi_scene_save_bake(ctx, in_gi_scene, in_state);
	}

	const bool layout_rebuilt = in_state.gi.layout_dirty;
	if (in_state.gi.layout_dirty)
	{
		printf("GI Scene Layout Dirty. Rebuilding...\n");
		// Replaced buffers/images retire against the current frame fence.
		gi_scene_rebuild_layout(ctx, in_gi_scene, in_state);
	}
	// Edited objects: queue every probe whose capture sphere reaches them
	// (the same test capture culling uses, so no probe that saw it is missed).
	const bool geometry_edited = in_state.gi.dirty_regions.length() > 0;
	if (geometry_edited)
	{
		i32 queued = 0;
		for (const BoundingBox& region : in_state.gi.dirty_regions)
		{
			for (i32 probe_index = 0; probe_index < (i32) in_gi_scene.probes.length(); ++probe_index)
			{
				const GI_Probe& probe = in_gi_scene.probes[probe_index];
				if (probe_index == in_gi_scene.fallback_probe_index
					|| in_gi_scene.probe_recapture_pending[probe_index]
					|| bounding_box_outside_sphere(region, { probe.position.XYZ, probe.max_radial_depth }))
				{
					continue;
				}
				in_gi_scene.probe_recapture_pending[probe_index] = 1;
				in_gi_scene.recapture_queue.add(probe_index);
				queued += 1;
			}
		}
		in_state.gi.dirty_regions.reset();
		in_state.gi.bake_status = EGiBakeStatus::Edited;
		printf("GI: %d probes queued for recapture (%zu pending)\n",
			queued, in_gi_scene.recapture_queue.length() - (size_t) in_gi_scene.recapture_cursor);
	}

	// The GI shadow map holds static casters, so an edit re-renders it too.
	if (GIShadowMap::update(graph, ctx, in_state, in_gi_scene.scene_bounds, layout_rebuilt || geometry_edited))
	{
		in_state.gi.relight_passes_remaining = State::GiState::relight_pass_count;
	}

	if (in_state.gi.bake_requested)
	{
		in_state.gi.bake_requested = false;
		in_state.gi.is_updating = true;
		in_state.gi.bake_status = EGiBakeStatus::Baking;
		in_gi_scene.probe_idx_to_update = 0;
	}

	bool needs_gpu_update = false;
	const auto capture_probe = [&](const i32 in_probe_index)
	{
		GI_Probe& probe = in_gi_scene.probes[in_probe_index];
		if (probe.atlas_idx < 0)
		{
			// Atlas slot == probe index, so a saved bake maps back without a table.
			probe.atlas_idx = in_probe_index;
			needs_gpu_update = true;
		}
		in_gi_scene.lighting_capture.render(
			graph, ctx,
			in_state,
			probe.position.XYZ,
			probe.atlas_idx,
			in_probe_index != in_gi_scene.fallback_probe_index,
			probe.max_radial_depth
		);
		if (GI_LOG_SCENE_UPDATE)
		{
			printf("Updating GI Probe %i/%zu at Position: %f, %f, %f\n",
				in_probe_index, in_gi_scene.probes.length(),
				probe.position.X, probe.position.Y, probe.position.Z);
		}
	};

	if (in_state.gi.is_updating)
	{
		assert(in_gi_scene.probes.is_valid_index(in_gi_scene.probe_idx_to_update));
		for (i32 i = 0; i < GI_Scene::probes_to_update_per_frame; ++i)
		{
			capture_probe(in_gi_scene.probe_idx_to_update);
			in_gi_scene.probe_idx_to_update = (in_gi_scene.probe_idx_to_update + 1) % (i32) in_gi_scene.probes.length();
			if (in_gi_scene.probe_idx_to_update == 0)
			{
				in_state.gi.is_updating = false;
				in_gi_scene.pending_save = true;
				// Geometry changed under every probe: light it from scratch.
				in_state.gi.relight_passes_remaining = State::GiState::relight_pass_count;
				in_gi_scene.relight_row_cursor = 0;
				break;
			}
		}
	}

	// Edit recaptures wait for a running bake, which captures every probe anyway.
	if (!in_state.gi.is_updating && in_gi_scene.recapture_cursor < (i32) in_gi_scene.recapture_queue.length())
	{
		for (i32 i = 0; i < GI_Scene::probes_to_update_per_frame
			&& in_gi_scene.recapture_cursor < (i32) in_gi_scene.recapture_queue.length(); ++i)
		{
			const i32 probe_index = in_gi_scene.recapture_queue[in_gi_scene.recapture_cursor++];
			in_gi_scene.probe_recapture_pending[probe_index] = 0;
			capture_probe(probe_index);
		}
		if (in_gi_scene.recapture_cursor == (i32) in_gi_scene.recapture_queue.length())
		{
			in_gi_scene.recapture_queue.reset();
			in_gi_scene.recapture_cursor = 0;
			// New geometry, old lighting until relit; restart for the bounces.
			in_state.gi.relight_passes_remaining = State::GiState::relight_pass_count;
		}
	}

	if (in_state.gi.relight_passes_remaining > 0)
	{
		gi_scene_relight_step(graph, ctx, in_gi_scene, in_state);
	}

	if (needs_gpu_update)
	{
		in_gi_scene.probes_buffer.update_gpu_buffer(
			in_gi_scene.probes.data(),
			sizeof(GI_Probe) * in_gi_scene.probes.length()
		);
	}
}

VkImageView gi_scene_get_octahedral_lighting_view(GI_Scene& in_gi_scene)
{
	return in_gi_scene.lighting_capture.cube_to_oct_pass.get_color_output(0).view;
}

VkImageView gi_scene_get_octahedral_depth_view(GI_Scene& in_gi_scene)
{
	return in_gi_scene.lighting_capture.moments_oct_pass.get_color_output(0).view;
}

VkImageView gi_scene_get_specular_lighting_view(GI_Scene& in_gi_scene)
{
	return in_gi_scene.lighting_capture.specular_atlas.view;
}

VkImageView gi_scene_get_brdf_lut_view(GI_Scene& in_gi_scene)
{
	return in_gi_scene.lighting_capture.brdf_lut.view;
}

void gi_scene_cleanup(VulkanContext* ctx, GI_Scene& in_gi_scene)
{
	gi_scene_destroy_layout_gpu_resources(in_gi_scene);
	in_gi_scene.lighting_capture.cleanup(ctx);
}
