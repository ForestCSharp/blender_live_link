#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>

#include "ankerl/unordered_dense.h"
#include "core/types.h"
#include "core/dynamic_array.h"
#include "game_object/game_object.h"
#include "render/core/geometry_arena_freelist.h"
#include "render/passes/gi/gi_bake.h"
#include "render/passes/gi/lighting_capture.h"

#include "gi_helpers.h"

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
	static constexpr i32 atlas_total_size = 2048;
	static constexpr i32 atlas_entry_size = 16;
	static constexpr i32 specular_atlas_entry_size = 48;
	static constexpr i32 specular_atlas_mip_count = 4;
	// Probe budget: every slot of the octahedral atlas (128^2). The octree depth
	// is capped at the deepest level whose probes fit (gi_scene_rebuild_layout).
	// At the full budget the specular atlas is 6144^2 (~400 MiB RGBA16F with
	// mips), past Vulkan's guaranteed 4096; gi_scene_init checks the device.
	static constexpr i32 maximum_lattice_probe_count =
		(atlas_total_size / atlas_entry_size) * (atlas_total_size / atlas_entry_size);
	static constexpr i32 maximum_specular_slots_per_dimension = atlas_total_size / atlas_entry_size;
	static constexpr i32 maximum_specular_atlas_size =
		maximum_specular_slots_per_dimension * specular_atlas_entry_size;
	static constexpr i32 probes_to_update_per_frame = 4;
};
static_assert(
	GI_Scene::maximum_specular_slots_per_dimension * GI_Scene::maximum_specular_slots_per_dimension
		>= GI_Scene::maximum_lattice_probe_count,
	"Specular atlas grid must hold the probe budget."
);

#define GI_LOG_SCENE_INIT 0
#define GI_LOG_SCENE_UPDATE 0

i32 gi_scene_atlas_capacity()
{
	const i32 atlas_entries_per_dimension = GI_Scene::atlas_total_size / GI_Scene::atlas_entry_size;
	return atlas_entries_per_dimension * atlas_entries_per_dimension;
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

	for (auto const& [unique_id, object] : in_state.scene.objects)
	{
		if (!object_contributes_to_gi_scene(object))
		{
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
	probe.padding = 0;
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

bool gi_scene_bounds_intersect(const BoundingBox& in_a, const BoundingBox& in_b)
{
	return in_a.min.X <= in_b.max.X && in_a.max.X >= in_b.min.X &&
		in_a.min.Y <= in_b.max.Y && in_a.max.Y >= in_b.min.Y &&
		in_a.min.Z <= in_b.max.Z && in_a.max.Z >= in_b.min.Z;
}

bool gi_scene_bounds_intersect_any(const BoundingBox& in_bounds, const DynamicArray<BoundingBox>& in_geometry_bounds)
{
	for (const BoundingBox& geometry_bounds : in_geometry_bounds)
	{
		if (gi_scene_bounds_intersect(in_bounds, geometry_bounds))
		{
			return true;
		}
	}
	return false;
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
	ankerl::unordered_dense::map<u64, i32>& out_probe_indices_by_corner,
	const GI_Coords in_max_depth_coords,
	const f32 in_required_radial_depth,
	const i32 in_octree_level)
{
	const u64 probe_key = gi_scene_probe_corner_key(in_max_depth_coords);
	auto existing_probe = out_probe_indices_by_corner.find(probe_key);
	if (existing_probe != out_probe_indices_by_corner.end())
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
	out_probe_indices_by_corner[probe_key] = probe_index;
	return probe_index;
}

i32 gi_scene_add_node_payload(
	GI_Scene& out_gi_scene,
	ankerl::unordered_dense::map<u64, i32>& out_probe_indices_by_corner,
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
					out_probe_indices_by_corner,
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
	node.padding[0] = node.padding[1] = 0;
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

i32 gi_scene_build_sparse_octree_node(
	GI_Scene& out_gi_scene,
	const DynamicArray<BoundingBox>& in_geometry_bounds,
	ankerl::unordered_dense::map<u64, i32>& out_probe_indices_by_corner,
	const BoundingBox& in_bounds,
	const i32 in_depth,
	const GI_Coords in_coords)
{
	GI_OctreeNode node = gi_scene_make_octree_node(in_bounds);
	node.payload_index = gi_scene_add_node_payload(out_gi_scene, out_probe_indices_by_corner, in_depth, in_coords);

	const i32 node_index = (i32) out_gi_scene.octree_nodes.length();
	out_gi_scene.octree_nodes.add(node);

	if (in_depth >= out_gi_scene.octree_depth)
	{
		return node_index;
	}

	i32 child_count = 0;
	for (i32 child_z = 0; child_z < 2; ++child_z)
	{
		for (i32 child_y = 0; child_y < 2; ++child_y)
		{
			for (i32 child_x = 0; child_x < 2; ++child_x)
			{
				const BoundingBox child_bounds = gi_scene_child_bounds(in_bounds, child_x, child_y, child_z);
				if (!gi_scene_bounds_intersect_any(child_bounds, in_geometry_bounds))
				{
					continue;
				}

				const GI_Coords child_coords = {
					in_coords.x * 2 + child_x,
					in_coords.y * 2 + child_y,
					in_coords.z * 2 + child_z,
				};
				const i32 child_slot = child_x + child_y * 2 + child_z * 4;
				const i32 child_index = gi_scene_build_sparse_octree_node(out_gi_scene, in_geometry_bounds, out_probe_indices_by_corner, child_bounds, in_depth + 1, child_coords);
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
		case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
		case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
		default: assert(false && "GI bake: unhandled atlas format"); return 0;
	}
}

// Atlas index i is probe i, row-major, so probes occupy the leading rows.
u32 gi_bake_octahedral_rows(const GI_Scene& in_gi_scene)
{
	const i32 entries_per_row = GI_Scene::atlas_total_size / GI_Scene::atlas_entry_size;
	const i32 entry_rows = ((i32) in_gi_scene.probes.length() + entries_per_row - 1) / entries_per_row;
	return (u32) (entry_rows * GI_Scene::atlas_entry_size);
}

GiBakeHeader gi_bake_expected_header(GI_Scene& in_gi_scene, const State& in_state)
{
	LightingCapture& capture = in_gi_scene.lighting_capture;
	// Everything that changes how the stored bytes are laid out or interpreted.
	const i32 params[] = {
		in_gi_scene.octree_depth,
		capture.desc.octahedral_total_size,
		capture.desc.octahedral_entry_size,
		capture.desc.specular_entry_size,
		capture.desc.specular_mip_count,
		capture.specular_atlas_total_size,
		(i32) capture.color_format,
		(i32) capture.specular_atlas.format,
		(i32) in_state.gi.probe_occlusion_mode,
	};
	const GeometryContentHash hash = geometry_arena_hash_bytes(
		params, sizeof(params), in_gi_scene.probe_layout_hash.low ^ in_gi_scene.probe_layout_hash.high);

	GiBakeHeader header;
	header.layout_hash[0] = hash.low;
	header.layout_hash[1] = hash.high;
	header.probe_count = in_gi_scene.probes.length();
	const u64 octahedral_bytes = (u64) capture.desc.octahedral_total_size
		* gi_bake_octahedral_rows(in_gi_scene) * gi_bake_format_bytes(capture.color_format);
	header.blob_bytes[GI_BAKE_BLOB_IRRADIANCE] = octahedral_bytes;
	header.blob_bytes[GI_BAKE_BLOB_MOMENTS] = octahedral_bytes;
	for (i32 mip = 0; mip < capture.desc.specular_mip_count; ++mip)
	{
		const u64 size = (u64) capture.specular_atlas_total_size >> mip;
		header.blob_bytes[GI_BAKE_BLOB_SPECULAR] += size * size * gi_bake_format_bytes(capture.specular_atlas.format);
	}
	header.blob_bytes[GI_BAKE_BLOB_SH9] = sizeof(HMM_Vec4) * in_gi_scene.sh9_coefficients.length();
	header.blob_bytes[GI_BAKE_BLOB_SG9] = sizeof(GI_SG9_Lobe) * in_gi_scene.sg9_lobes.length();
	return header;
}

// Records copies between the three atlases and a staging buffer laid out like
// the file's image blobs; in_upload picks the direction. Leaves the atlases
// SHADER_READ_ONLY as the lighting pass expects.
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

	const u32 rows = gi_bake_octahedral_rows(in_gi_scene);
	const u32 octahedral_size = (u32) capture.desc.octahedral_total_size;
	for (i32 output_idx = 0; output_idx < 2; ++output_idx)
	{
		GpuImage& atlas = capture.cube_to_oct_pass.get_color_output(output_idx);
		gpu_image_transition(in_command_buffer, atlas, transfer_layout);
		copy(atlas, 0, octahedral_size, rows);
		gpu_image_transition(in_command_buffer, atlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}
	gpu_image_transition(in_command_buffer, capture.specular_atlas, transfer_layout);
	for (i32 mip = 0; mip < capture.desc.specular_mip_count; ++mip)
	{
		const u32 size = (u32) capture.specular_atlas_total_size >> mip;
		copy(capture.specular_atlas, (u32) mip, size, size);
	}
	gpu_image_transition(in_command_buffer, capture.specular_atlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
		// Projection compute wrote the SH9/SG9 buffers in earlier frames.
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
		const VkBufferCopy sh9_copy = {
			.dstOffset = gi_bake_blob_offset(header, GI_BAKE_BLOB_SH9),
			.size = header.blob_bytes[GI_BAKE_BLOB_SH9],
		};
		vkCmdCopyBuffer(in_command_buffer, in_gi_scene.sh9_coefficients_buffer.get_gpu_buffer(), staging.buffer, 1, &sh9_copy);
		const VkBufferCopy sg9_copy = {
			.dstOffset = gi_bake_blob_offset(header, GI_BAKE_BLOB_SG9),
			.size = header.blob_bytes[GI_BAKE_BLOB_SG9],
		};
		vkCmdCopyBuffer(in_command_buffer, in_gi_scene.sg9_lobes_buffer.get_gpu_buffer(), staging.buffer, 1, &sg9_copy);

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

// Looks for a bake matching the freshly built layout. On a hit, fills the CPU
// SH9/SG9 arrays and probe atlas indices (the GPU buffers are created from
// them afterwards; they are static, so they cannot be updated in place) and
// returns the file's blobs (atlas blobs first) for gi_scene_upload_bake_atlases.
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

	out_atlas_bytes = gi_bake_blob_offset(header, GI_BAKE_BLOB_SH9);
	memcpy(in_gi_scene.sh9_coefficients.data(), out_blobs.data() + out_atlas_bytes,
		header.blob_bytes[GI_BAKE_BLOB_SH9]);
	memcpy(in_gi_scene.sg9_lobes.data(), out_blobs.data() + gi_bake_blob_offset(header, GI_BAKE_BLOB_SG9),
		header.blob_bytes[GI_BAKE_BLOB_SG9]);
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

	// Probe count depends on how the geometry fills the octree, so a depth can
	// only be checked against the probe budget by building it. Build upward and
	// stop before the first depth over budget: building the requested depth
	// first could mean millions of nodes at depth 12. The slider then shows the
	// depth actually in use.
	const auto build_layout = [&](const i32 in_depth)
	{
		out_gi_scene.octree_depth = in_depth;
		out_gi_scene.leaf_divisions = gi_scene_leaf_divisions_from_depth(in_depth);
		out_gi_scene.leaf_cell_extent = (out_gi_scene.scene_bounds.max.X - out_gi_scene.scene_bounds.min.X) / (f32) out_gi_scene.leaf_divisions;
		gi_scene_reset_layout_buffers(out_gi_scene);
		if (has_visible_geometry)
		{
			ankerl::unordered_dense::map<u64, i32> probe_indices_by_corner;
			gi_scene_build_sparse_octree_node(out_gi_scene, geometry_bounds, probe_indices_by_corner, out_gi_scene.scene_bounds, 0, (GI_Coords){0, 0, 0});
		}
		else
		{
			gi_scene_init_empty_octree(out_gi_scene);
		}
		gi_scene_add_fallback_probe(out_gi_scene);
		return out_gi_scene.probes.length();
	};
	const i32 requested_depth = out_gi_scene.octree_depth;
	i32 depth = GI_Scene::min_octree_depth;
	i32 built_depth = -1;	// the layout currently in out_gi_scene, which may be the rejected one
	while (depth < requested_depth)
	{
		const size_t probe_count = build_layout(depth + 1);
		built_depth = depth + 1;
		if (probe_count > (size_t) GI_Scene::maximum_lattice_probe_count)
		{
			printf("GI octree depth %d needs %zu probes (budget %d); using depth %d\n",
				depth + 1, probe_count, GI_Scene::maximum_lattice_probe_count, depth);
			break;
		}
		depth += 1;
	}
	if (built_depth != depth)
	{
		build_layout(depth);
	}
	in_state.gi.octree_depth = out_gi_scene.octree_depth;
	gi_scene_init_radiance_buffers(out_gi_scene);

	assert(out_gi_scene.probes.length() <= (size_t) GI_Scene::maximum_lattice_probe_count);
	assert(out_gi_scene.probes.length() <= (size_t) gi_scene_atlas_capacity());
	assert(out_gi_scene.probes.length() > 0);
	assert(out_gi_scene.octree_nodes.length() > 0);
	assert(out_gi_scene.cells.length() > 0);
	out_gi_scene.probe_layout_hash = geometry_arena_hash_bytes(
		out_gi_scene.probes.data(), sizeof(GI_Probe) * out_gi_scene.probes.length(), 0x4749424bull);
	// Sized first: the bake header covers the specular atlas size and format.
	out_gi_scene.lighting_capture.resize_specular_atlas(ctx, (i32)out_gi_scene.probes.length());
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
	in_state.gi.sky_probe_dirty = true;

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

f32 gi_scene_debug_probe_radius_for_probe(const GI_Scene& in_gi_scene, const i32 in_probe_index)
{
	assert(in_gi_scene.probes.is_valid_index(in_probe_index));
	const i32 probe_level = in_gi_scene.probes[in_probe_index].octree_level;
	const i32 levels_above_deepest = probe_level < 0
		? in_gi_scene.octree_depth + 1
		: std::clamp(in_gi_scene.octree_depth - probe_level, 0, in_gi_scene.octree_depth);
	return gi_scene_debug_probe_radius(in_gi_scene)
		* std::pow(1.1f, (f32) levels_above_deepest);
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
		.octahedral_total_size = GI_Scene::atlas_total_size,
		.octahedral_entry_size = GI_Scene::atlas_entry_size,
		.specular_entry_size = GI_Scene::specular_atlas_entry_size,
		.specular_mip_count = GI_Scene::specular_atlas_mip_count,
	};
	assert((i32)ctx->capabilities.properties.limits.maxImageDimension2D
		>= GI_Scene::maximum_specular_atlas_size);
	out_gi_scene.lighting_capture.init(ctx, lighting_capture_desc);

	gi_scene_rebuild_layout(ctx, out_gi_scene, in_state);
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

	if (in_state.gi.layout_dirty)
	{
		printf("GI Scene Layout Dirty. Rebuilding...\n");
		// Replaced buffers/images retire against the current frame fence.
		gi_scene_rebuild_layout(ctx, in_gi_scene, in_state);
	}

	if (in_state.gi.bake_requested)
	{
		in_state.gi.bake_requested = false;
		in_state.gi.is_updating = true;
		in_state.gi.sky_probe_dirty = false;	// the bake captures the fallback too
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
			in_probe_index,
			probe.max_radial_depth,
			in_gi_scene.sh9_coefficients_buffer.get_gpu_buffer(),
			in_gi_scene.sg9_lobes_buffer.get_gpu_buffer()
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
				break;
			}
		}
	}
	else if (in_state.gi.sky_probe_dirty && in_gi_scene.fallback_probe_index >= 0)
	{
		in_state.gi.sky_probe_dirty = false;
		capture_probe(in_gi_scene.fallback_probe_index);
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
	return in_gi_scene.lighting_capture.cube_to_oct_pass.get_color_output(1).view;
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
