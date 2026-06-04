#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>

#include "core/types.h"
#include "core/stretchy_buffer.h"
#include "game_object/game_object.h"
#include "render/lighting_capture.h"

#include "render/gi_debug_pass.h"
#include "render/cubemap_debug_pass.h"

#include "shaders/gi_helpers.h"

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
	StretchyBuffer<GI_OctreeNode> octree_nodes;
	StretchyBuffer<GI_Cell> cells;
	StretchyBuffer<GI_Probe> probes;
	StretchyBuffer<HMM_Vec4> sh9_coefficients;
	StretchyBuffer<GI_SG9_Lobe> sg9_lobes;

	GpuBuffer<GI_OctreeNode> octree_nodes_buffer;
	GpuBuffer<GI_Probe> probes_buffer;
	GpuBuffer<GI_Cell> cells_buffer;
	GpuBuffer<HMM_Vec4> sh9_coefficients_buffer;
	GpuBuffer<GI_SG9_Lobe> sg9_lobes_buffer;

	// Probe Update State
	LightingCapture lighting_capture;
	i32 probe_idx_to_update = 0;
	i32 next_atlas_index = 0;
	bool has_rendered_default_probe = false;

	// Dynamic Layout
	BoundingBox scene_bounds = {};
	i32 octree_depth = 4;
	i32 leaf_divisions = 16;
	i32 fallback_probe_index = -1;
	i32 non_fallback_probe_count = 0;
	f32 leaf_cell_extent = 1.0f;
	f32 max_radial_depth = 4.0f;

	// Debug Data
	Mesh debug_sphere;
	sg_pipeline gi_debug_pipeline;

	// Static Parameters
	static constexpr int min_octree_depth = 1;
	static constexpr int max_octree_depth = 4;
	static constexpr int default_octree_depth = 4;
	static constexpr f32 fallback_scene_extent = 30.0f;
	static constexpr f32 minimum_scene_extent = 1.0f;
	static constexpr f32 radial_depth_cell_scale = 4.0f;
	static constexpr int cubemap_capture_size = 256;
	static constexpr int atlas_total_size = 2048;
	static constexpr int atlas_entry_size = 16;
	static constexpr int probes_to_update_per_frame = 4;
};

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

i32 gi_scene_cell_index_from_coords(const GI_Coords in_coords, const i32 in_leaf_divisions)
{
	return in_coords.x +
		in_coords.y * in_leaf_divisions +
		in_coords.z * in_leaf_divisions * in_leaf_divisions;
}

i32 gi_scene_probe_index_from_coords(const GI_Coords in_coords, const i32 in_probe_divisions)
{
	return in_coords.x +
		in_coords.y * in_probe_divisions +
		in_coords.z * in_probe_divisions * in_probe_divisions;
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

BoundingBox gi_scene_compute_scene_bounds(const State& in_state)
{
	BoundingBox scene_bounds = bounding_box_init();
	bool has_visible_mesh = false;

	for (auto const& [unique_id, object] : in_state.scene.objects)
	{
		if (!object.visibility || !object.has_mesh)
		{
			continue;
		}

		const BoundingBox object_bounds = object_get_bounding_box(object);
		scene_bounds.min = HMM_MinV3(scene_bounds.min, object_bounds.min);
		scene_bounds.max = HMM_MaxV3(scene_bounds.max, object_bounds.max);
		has_visible_mesh = true;
	}

	if (!has_visible_mesh)
	{
		return gi_scene_cube_bounds_from_center_extent(HMM_V3(0.0f, 0.0f, 0.0f), GI_Scene::fallback_scene_extent);
	}

	return gi_scene_expand_bounds_to_cube(scene_bounds);
}

HMM_Vec3 gi_scene_lattice_position(const BoundingBox& in_bounds, const i32 in_leaf_divisions, const GI_Coords in_coords)
{
	const f32 cell_extent = (in_bounds.max.X - in_bounds.min.X) / (f32)in_leaf_divisions;
	return in_bounds.min + HMM_V3(
		(f32)in_coords.x * cell_extent,
		(f32)in_coords.y * cell_extent,
		(f32)in_coords.z * cell_extent
	);
}

void gi_scene_reset_layout_buffers(GI_Scene& in_gi_scene)
{
	in_gi_scene.octree_nodes.reset();
	in_gi_scene.cells.reset();
	in_gi_scene.probes.reset();
	in_gi_scene.sh9_coefficients.reset();
	in_gi_scene.sg9_lobes.reset();
}

void gi_scene_destroy_layout_gpu_resources(GI_Scene& in_gi_scene)
{
	in_gi_scene.octree_nodes_buffer.destroy_gpu_buffer();
	in_gi_scene.probes_buffer.destroy_gpu_buffer();
	in_gi_scene.cells_buffer.destroy_gpu_buffer();
	in_gi_scene.sh9_coefficients_buffer.destroy_gpu_buffer();
	in_gi_scene.sg9_lobes_buffer.destroy_gpu_buffer();
}

void gi_scene_init_probe_lattice(GI_Scene& out_gi_scene)
{
	const i32 probe_divisions = out_gi_scene.leaf_divisions + 1;
	const i32 probe_count = probe_divisions * probe_divisions * probe_divisions;
	out_gi_scene.non_fallback_probe_count = probe_count;
	out_gi_scene.fallback_probe_index = probe_count;

	out_gi_scene.probes.add_uninitialized(probe_count + 1);

	for (i32 z = 0; z < probe_divisions; ++z)
	{
		for (i32 y = 0; y < probe_divisions; ++y)
		{
			for (i32 x = 0; x < probe_divisions; ++x)
			{
				const GI_Coords probe_coords = { x, y, z };
				const i32 probe_index = gi_scene_probe_index_from_coords(probe_coords, probe_divisions);
				GI_Probe& probe = out_gi_scene.probes[probe_index];
				probe.position = HMM_V4V(gi_scene_lattice_position(out_gi_scene.scene_bounds, out_gi_scene.leaf_divisions, probe_coords), 1.0f);
				probe.atlas_idx = -1;
				probe.padding[0] = probe.padding[1] = probe.padding[2] = 0;
			}
		}
	}

	GI_Probe& fallback_probe = out_gi_scene.probes[out_gi_scene.fallback_probe_index];
	fallback_probe.position = HMM_V4V(gi_scene_bounds_center(out_gi_scene.scene_bounds), 1.0f);
	fallback_probe.atlas_idx = -1;
	fallback_probe.padding[0] = fallback_probe.padding[1] = fallback_probe.padding[2] = 0;
}

void gi_scene_init_leaf_payloads(GI_Scene& out_gi_scene)
{
	const i32 leaf_count = out_gi_scene.leaf_divisions * out_gi_scene.leaf_divisions * out_gi_scene.leaf_divisions;
	const i32 probe_divisions = out_gi_scene.leaf_divisions + 1;
	out_gi_scene.cells.add_uninitialized(leaf_count);

	for (i32 z = 0; z < out_gi_scene.leaf_divisions; ++z)
	{
		for (i32 y = 0; y < out_gi_scene.leaf_divisions; ++y)
		{
			for (i32 x = 0; x < out_gi_scene.leaf_divisions; ++x)
			{
				const GI_Coords cell_coords = { x, y, z };
				const i32 cell_index = gi_scene_cell_index_from_coords(cell_coords, out_gi_scene.leaf_divisions);
				GI_Cell& cell = out_gi_scene.cells[cell_index];

				for (i32 corner_z = 0; corner_z < 2; ++corner_z)
				{
					for (i32 corner_y = 0; corner_y < 2; ++corner_y)
					{
						for (i32 corner_x = 0; corner_x < 2; ++corner_x)
						{
							const GI_Coords probe_coords = {
								x + corner_x,
								y + corner_y,
								z + corner_z,
							};
							const i32 corner_index = corner_x + corner_y * 2 + corner_z * 4;
							cell.probe_indices[corner_index] = gi_scene_probe_index_from_coords(probe_coords, probe_divisions);
						}
					}
				}
			}
		}
	}
}

i32 gi_scene_build_dense_octree_node(
	GI_Scene& out_gi_scene,
	const HMM_Vec3 in_min,
	const HMM_Vec3 in_max,
	const i32 in_depth,
	const GI_Coords in_coords)
{
	GI_OctreeNode node = {};
	node.min = HMM_V4V(in_min, 1.0f);
	node.max = HMM_V4V(in_max, 1.0f);
	node.is_leaf = in_depth >= out_gi_scene.octree_depth ? 1 : 0;
	node.payload_index = -1;
	node.padding[0] = node.padding[1] = 0;
	for (i32 i = 0; i < 8; ++i)
	{
		node.child_indices[i] = -1;
	}

	const i32 node_index = (i32)out_gi_scene.octree_nodes.length();
	out_gi_scene.octree_nodes.add(node);

	if (node.is_leaf)
	{
		out_gi_scene.octree_nodes[node_index].payload_index = gi_scene_cell_index_from_coords(in_coords, out_gi_scene.leaf_divisions);
		return node_index;
	}

	const HMM_Vec3 center = (in_min + in_max) * 0.5f;
	for (i32 child_z = 0; child_z < 2; ++child_z)
	{
		for (i32 child_y = 0; child_y < 2; ++child_y)
		{
			for (i32 child_x = 0; child_x < 2; ++child_x)
			{
				const HMM_Vec3 child_min = HMM_V3(
					child_x == 0 ? in_min.X : center.X,
					child_y == 0 ? in_min.Y : center.Y,
					child_z == 0 ? in_min.Z : center.Z
				);
				const HMM_Vec3 child_max = HMM_V3(
					child_x == 0 ? center.X : in_max.X,
					child_y == 0 ? center.Y : in_max.Y,
					child_z == 0 ? center.Z : in_max.Z
				);
				const GI_Coords child_coords = {
					in_coords.x * 2 + child_x,
					in_coords.y * 2 + child_y,
					in_coords.z * 2 + child_z,
				};
				const i32 child_slot = child_x + child_y * 2 + child_z * 4;
				const i32 child_index = gi_scene_build_dense_octree_node(out_gi_scene, child_min, child_max, in_depth + 1, child_coords);
				out_gi_scene.octree_nodes[node_index].child_indices[child_slot] = child_index;
			}
		}
	}

	return node_index;
}

void gi_scene_init_radiance_buffers(GI_Scene& out_gi_scene)
{
	out_gi_scene.sh9_coefficients.add_uninitialized(out_gi_scene.probes.length() * 9);
	out_gi_scene.sg9_lobes.add_uninitialized(out_gi_scene.probes.length() * 9);

	for (HMM_Vec4& coefficient : out_gi_scene.sh9_coefficients)
	{
		coefficient = HMM_V4(0,0,0,0);
	}

	for (GI_SG9_Lobe& lobe : out_gi_scene.sg9_lobes)
	{
		lobe.params = HMM_V4(0,0,1,1);
		lobe.amplitude = HMM_V4(0,0,0,0);
	}
}

void gi_scene_recreate_gpu_buffers(GI_Scene& out_gi_scene)
{
	gi_scene_destroy_layout_gpu_resources(out_gi_scene);

	out_gi_scene.octree_nodes_buffer = GpuBuffer<GI_OctreeNode>((GpuBufferDesc<GI_OctreeNode>){
		.size = sizeof(GI_OctreeNode) * out_gi_scene.octree_nodes.length(),
		.usage = {
			.storage_buffer = true,
			.stream_update = true,
		},
		.label = "GI Octree Nodes Buffer",
	});
	out_gi_scene.octree_nodes_buffer.update_gpu_buffer((sg_range){
		.ptr = out_gi_scene.octree_nodes.data(),
		.size = sizeof(GI_OctreeNode) * out_gi_scene.octree_nodes.length(),
	});

	out_gi_scene.probes_buffer = GpuBuffer<GI_Probe>((GpuBufferDesc<GI_Probe>){
		.size = sizeof(GI_Probe) * out_gi_scene.probes.length(),
		.usage = {
			.storage_buffer = true,
			.stream_update = true,
		},
		.label = "GI Probes Buffer",
	});

	out_gi_scene.cells_buffer = GpuBuffer<GI_Cell>((GpuBufferDesc<GI_Cell>){
		.size = sizeof(GI_Cell) * out_gi_scene.cells.length(),
		.usage = {
			.storage_buffer = true,
			.stream_update = true,
		},
		.label = "GI Cells Buffer",
	});
	out_gi_scene.cells_buffer.update_gpu_buffer((sg_range){
		.ptr = out_gi_scene.cells.data(),
		.size = sizeof(GI_Cell) * out_gi_scene.cells.length(),
	});

	out_gi_scene.sh9_coefficients_buffer = GpuBuffer<HMM_Vec4>((GpuBufferDesc<HMM_Vec4>){
		.data = out_gi_scene.sh9_coefficients.data(),
		.size = sizeof(HMM_Vec4) * out_gi_scene.sh9_coefficients.length(),
		.usage = {
			.storage_buffer = true,
		},
		.label = "GI SH9 Coefficients Buffer",
	});

	out_gi_scene.sg9_lobes_buffer = GpuBuffer<GI_SG9_Lobe>((GpuBufferDesc<GI_SG9_Lobe>){
		.data = out_gi_scene.sg9_lobes.data(),
		.size = sizeof(GI_SG9_Lobe) * out_gi_scene.sg9_lobes.length(),
		.usage = {
			.storage_buffer = true,
		},
		.label = "GI SG9 Lobes Buffer",
	});
}

void gi_scene_rebuild_layout(GI_Scene& out_gi_scene, State& in_state)
{
	out_gi_scene.octree_depth = std::clamp(in_state.gi.octree_depth, GI_Scene::min_octree_depth, GI_Scene::max_octree_depth);
	in_state.gi.octree_depth = out_gi_scene.octree_depth;
	out_gi_scene.leaf_divisions = gi_scene_leaf_divisions_from_depth(out_gi_scene.octree_depth);
	out_gi_scene.scene_bounds = gi_scene_compute_scene_bounds(in_state);
	out_gi_scene.leaf_cell_extent = (out_gi_scene.scene_bounds.max.X - out_gi_scene.scene_bounds.min.X) / (f32)out_gi_scene.leaf_divisions;
	out_gi_scene.max_radial_depth = std::max(out_gi_scene.leaf_cell_extent * GI_Scene::radial_depth_cell_scale, GI_Scene::minimum_scene_extent);

	gi_scene_reset_layout_buffers(out_gi_scene);
	gi_scene_init_probe_lattice(out_gi_scene);
	gi_scene_init_leaf_payloads(out_gi_scene);
	gi_scene_build_dense_octree_node(out_gi_scene, out_gi_scene.scene_bounds.min, out_gi_scene.scene_bounds.max, 0, (GI_Coords){0,0,0});
	gi_scene_init_radiance_buffers(out_gi_scene);

	assert(out_gi_scene.probes.length() <= (size_t)gi_scene_atlas_capacity());
	gi_scene_recreate_gpu_buffers(out_gi_scene);

	out_gi_scene.probe_idx_to_update = 0;
	out_gi_scene.next_atlas_index = 0;
	out_gi_scene.has_rendered_default_probe = false;
	in_state.gi.layout_dirty = false;
	in_state.gi.is_updating = true;
	in_state.gi.isolated_probe_index = -1;

	if (GI_LOG_SCENE_INIT)
	{
		bounding_box_print(out_gi_scene.scene_bounds, "GI Scene Bounds");
		printf(
			"GI octree depth: %d nodes: %zu cells: %zu probes: %zu leaf extent: %f max radial depth: %f\n",
			out_gi_scene.octree_depth,
			out_gi_scene.octree_nodes.length(),
			out_gi_scene.cells.length(),
			out_gi_scene.probes.length(),
			out_gi_scene.leaf_cell_extent,
			out_gi_scene.max_radial_depth
		);
	}
}

f32 gi_scene_debug_probe_radius(const GI_Scene& in_gi_scene)
{
	return std::max(in_gi_scene.leaf_cell_extent * 0.1f, 0.025f);
}

HMM_Vec3 gi_scene_probe_position_from_index(GI_Scene& in_gi_scene, const i32 in_probe_index)
{
	assert(in_gi_scene.probes.is_valid_index(in_probe_index));
	return in_gi_scene.probes[in_probe_index].position.XYZ;
}

void gi_scene_init(GI_Scene& out_gi_scene, State& in_state)
{
	const LightingCaptureDesc lighting_capture_desc = {
		// Cubemap Capture Setup
		.cubemap_render_size = GI_Scene::cubemap_capture_size,
		// Octahedral Atlas Setup
		.octahedral_total_size = GI_Scene::atlas_total_size,
		.octahedral_entry_size = GI_Scene::atlas_entry_size,
	};
	out_gi_scene.lighting_capture.init(lighting_capture_desc);

	// Setup Debug Data. Radius is provided as a shader uniform so layout rebuilds do not recreate geometry.
	out_gi_scene.debug_sphere = make_mesh(mesh_init_data_uv_sphere(1.0f,24,24));
	out_gi_scene.gi_debug_pipeline = sg_make_pipeline(GIDebugPass::make_pipeline_desc(sglue_swapchain().depth_format));

	gi_scene_rebuild_layout(out_gi_scene, in_state);
}

void gi_scene_update(GI_Scene& in_gi_scene, State& in_state)
{
	if (in_state.gi.layout_dirty)
	{
		gi_scene_rebuild_layout(in_gi_scene, in_state);
	}

	if (!in_state.gi.is_updating)
	{
		return;
	}

	assert(in_gi_scene.probes.is_valid_index(in_gi_scene.probe_idx_to_update));

	bool needs_gpu_update = false;
	for (i32 i = 0; i < GI_Scene::probes_to_update_per_frame; ++i)
	{
		GI_Probe& probe_to_update = in_gi_scene.probes[in_gi_scene.probe_idx_to_update];

		if (probe_to_update.atlas_idx < 0)
		{
			assert(in_gi_scene.next_atlas_index < gi_scene_atlas_capacity());
			probe_to_update.atlas_idx = in_gi_scene.next_atlas_index;
			in_gi_scene.next_atlas_index += 1;
			needs_gpu_update = true;
		}

		const HMM_Vec3 lighting_capture_position = probe_to_update.position.XYZ;
		const bool should_render_geometry = in_gi_scene.probe_idx_to_update != in_gi_scene.fallback_probe_index;
		in_gi_scene.lighting_capture.render(
				in_state,
				lighting_capture_position,
				probe_to_update.atlas_idx,
				should_render_geometry,
				in_gi_scene.probe_idx_to_update,
				in_gi_scene.max_radial_depth,
				in_gi_scene.sh9_coefficients_buffer.get_storage_view(),
				in_gi_scene.sg9_lobes_buffer.get_storage_view()
		);

		if (GI_LOG_SCENE_UPDATE)
		{
			printf(
				"Updating GI Probe %i/%zu at Position: %f, %f, %f\n",
				in_gi_scene.probe_idx_to_update,
				in_gi_scene.probes.length(),
				lighting_capture_position.X,
				lighting_capture_position.Y,
				lighting_capture_position.Z
			);
		}

		in_gi_scene.probe_idx_to_update = (in_gi_scene.probe_idx_to_update + 1) % in_gi_scene.probes.length();
		if (in_gi_scene.probe_idx_to_update == 0)
		{
			in_state.gi.is_updating = false;
			break;
		}
	}

	if (needs_gpu_update)
	{
		in_gi_scene.probes_buffer.update_gpu_buffer(
			(sg_range){
				.ptr = in_gi_scene.probes.data(),
				.size = sizeof(GI_Probe) * in_gi_scene.probes.length(),
			}
		);
	}
}

sg_view gi_scene_get_octahedral_lighting_view(GI_Scene& in_gi_scene)
{
	return in_gi_scene.lighting_capture.cube_to_oct_pass.get_color_output(0).get_texture_view(0);
}

sg_view gi_scene_get_octahedral_depth_view(GI_Scene& in_gi_scene)
{
	return in_gi_scene.lighting_capture.cube_to_oct_pass.get_color_output(1).get_texture_view(0);
}

void gi_scene_render_debug(GI_Scene& in_gi_scene, const HMM_Mat4& in_view_matrix, const HMM_Mat4& in_projection_matrix)
{
	sg_apply_pipeline(in_gi_scene.gi_debug_pipeline);

	gi_debug_vs_params_t vs_params = {
		.view = in_view_matrix,
		.projection = in_projection_matrix,
		.debug_probe_start_index = 0,
		.probe_debug_radius = gi_scene_debug_probe_radius(in_gi_scene),
	};
	// Apply Vertex Uniforms
	sg_apply_uniforms(0, SG_RANGE(vs_params));

	gi_debug_fs_params_t fs_params = {
		.atlas_total_size = in_gi_scene.lighting_capture.desc.octahedral_total_size,
		.atlas_entry_size = in_gi_scene.lighting_capture.desc.octahedral_entry_size,
		.probe_vis_mode = static_cast<i32>(state.gi.probe_vis_mode),
		.isolated_probe_index = state.gi.isolated_probe_index,
		.max_radial_depth = in_gi_scene.max_radial_depth,
	};
	// Apply Fragment Uniforms
	sg_apply_uniforms(1, SG_RANGE(fs_params));

	sg_bindings bindings = {
		.vertex_buffers[0] = in_gi_scene.debug_sphere.vertex_buffer.get_gpu_buffer(),
		.index_buffer = in_gi_scene.debug_sphere.index_buffer.get_gpu_buffer(),
		.views = {
			[0] = gi_scene_get_octahedral_lighting_view(in_gi_scene),
			[1] = gi_scene_get_octahedral_depth_view(in_gi_scene),
			[2] = in_gi_scene.probes_buffer.get_storage_view(),
			[3] = in_gi_scene.sh9_coefficients_buffer.get_storage_view(),
			[4] = in_gi_scene.sg9_lobes_buffer.get_storage_view(),
			[5] = in_gi_scene.probes_buffer.get_storage_view(),
		},
		.samplers = {
			[0] = state.gpu.linear_sampler,
			[1] = state.gpu.nearest_sampler,
		},
	};
	sg_apply_bindings(&bindings);

	sg_draw(0, in_gi_scene.debug_sphere.index_count, in_gi_scene.non_fallback_probe_count);
}
