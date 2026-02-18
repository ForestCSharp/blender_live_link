#pragma once

//FCS TODO: Evaluate Grid and Octree
// grid should be fine as probes will have fixed locations
// can still do LOD by just marking grid cells as having probes or not...

#include "core/types.h"
#include "core/stretchy_buffer.h"
#include "game_object/game_object.h"
#include "render/lighting_capture.h"
#include "render/gi_debug_pass.h"

#include "shaders/gi_helpers.h"

struct GI_Scene
{
	StretchyBuffer<GI_Cell> cells;
	StretchyBuffer<GI_Probe> probes;

	GpuBuffer<GI_Probe> probes_buffer;
	GpuBuffer<GI_Cell> cells_buffer;

	// Probe Update State
	LightingCapture lighting_capture;
	i32 probe_idx_to_update = 0;
	i32 next_atlas_index = 0;

	// Debug Data
	Mesh debug_sphere;
	sg_pipeline gi_debug_pipeline;

	// Static Parameters 
	static const int cubemap_capture_size = 256;
	static const int atlas_total_size = 512;
	static const int atlas_entry_size = 16;
	static const int probes_to_update_per_frame = 4;

};

#define GI_LOG_SCENE_INIT 0
#define GI_LOG_SCENE_UPDATE 0

void gi_scene_init(GI_Scene& out_gi_scene)
{
	out_gi_scene.cells.reset();
	out_gi_scene.probes.reset();

	out_gi_scene.cells.add_uninitialized(GI_CELL_COUNT);
	out_gi_scene.probes.add_uninitialized(GI_PROBE_COUNT);

	//FCS TODO: CHECK
	for (i32 cell_idx = 0; cell_idx < GI_CELL_COUNT; ++cell_idx)
	{
		GI_Cell& cell = out_gi_scene.cells[cell_idx];
		GI_Coords cell_coords = gi_cell_coords_from_index(cell_idx);

		if (GI_LOG_SCENE_INIT)
		{
			printf("Cell Coords: %i, %i, %i\n", cell_coords.x, cell_coords.y, cell_coords.z);
			HMM_Vec3 cell_center = gi_cell_center_from_coords(cell_coords);
			printf("Cell Center: %f, %f, %f\n", cell_center.X, cell_center.Y, cell_center.Z);
		}

		// Setup probe indices for this cell
		for (i32 z = 0; z < 2; ++z)
		{
			for (i32 y = 0; y < 2; ++y)
			{
				for (i32 x = 0; x < 2; ++x)
				{
					GI_Coords probe_coords = {
						.x = cell_coords.x + x,
						.y = cell_coords.y + y,
						.z = cell_coords.z + z,
					};

					if (GI_LOG_SCENE_INIT)
					{
						printf("\tProbe Coords: %i, %i, %i\n", probe_coords.x, probe_coords.y, probe_coords.z);
						HMM_Vec3 probe_position = gi_probe_position_from_coords(probe_coords);
						printf("\tProbe Position: %f, %f, %f\n", probe_position.X, probe_position.Y, probe_position.Z);
					}

					i32 probe_idx = 
						probe_coords.x + 
						probe_coords.y * GI_PROBE_DIMENSIONS + 
						probe_coords.z * GI_PROBE_DIMENSIONS * GI_PROBE_DIMENSIONS;

					i32 corner_idx = x + y * 2 + z * 4;
					cell.probe_indices[corner_idx] = probe_idx;
				}
			}
		}

		for (i32 i = 0; i < 8; ++i) 
		{
			i32 p_idx = cell.probe_indices[i];
			
			// 1. Ensure the index is valid
			assert(p_idx >= 0 && p_idx < GI_PROBE_COUNT);

			// 2. Decode the index back into coordinates to verify order
			GI_Coords test_coords = gi_probe_coords_from_index(p_idx);

			// We verify that the relative position of the probe matches the bitmask 'i'
			// This directly validates your shader's (i & 1), ((i >> 1) & 1), etc.
			assert((test_coords.x - cell_coords.x) == (i & 1));
			assert((test_coords.y - cell_coords.y) == ((i >> 1) & 1));
			assert((test_coords.z - cell_coords.z) == ((i >> 2) & 1));
		}
	}

	for (GI_Probe& probe : out_gi_scene.probes)
	{
		probe.atlas_idx = -1;
	}

	GpuBufferDesc<GI_Probe> probes_buffer_desc = {
		.size = sizeof(GI_Probe) * out_gi_scene.probes.length(),
		.usage = {
			.storage_buffer = true,
			.stream_update = true,
		},
		.label = "GI Probes Buffer",
	};
	out_gi_scene.probes_buffer = GpuBuffer<GI_Probe>(probes_buffer_desc);

	GpuBufferDesc<GI_Cell> cells_buffer_desc = {
		.size = sizeof(GI_Cell) * out_gi_scene.cells.length(),
		.usage = {
			.storage_buffer = true,
			.stream_update = true,
		},
		.label = "GI Cells Buffer",
	};
	out_gi_scene.cells_buffer = GpuBuffer<GI_Cell>(cells_buffer_desc);
	out_gi_scene.cells_buffer.update_gpu_buffer(
		(sg_range){
			.ptr = out_gi_scene.cells.data(), 
			.size = sizeof(GI_Cell) * out_gi_scene.cells.length(),
		}
	);
	
	const LightingCaptureDesc lighting_capture_desc = {
		// Cubemap Capture Setup
		.cubemap_render_size = GI_Scene::cubemap_capture_size,	
		// Octahedral Atlas Setup
		.octahedral_total_size = GI_Scene::atlas_total_size,
		.octahedral_entry_size = GI_Scene::atlas_entry_size,
	};
	out_gi_scene.lighting_capture.init(lighting_capture_desc);

	// Setup Debug Data
	const f32 debug_sphere_radius = GI_CELL_EXTENT * 0.1f; 
	out_gi_scene.debug_sphere = make_mesh(mesh_init_data_uv_sphere(debug_sphere_radius,24,24));
	out_gi_scene.gi_debug_pipeline = sg_make_pipeline(GIDebugPass::make_pipeline_desc(sglue_swapchain().depth_format));
}

void gi_scene_update(GI_Scene& in_gi_scene, State& in_state)
{
	assert(in_gi_scene.probes.is_valid_index(in_gi_scene.probe_idx_to_update));

	if (!in_state.gi_is_updating)
	{
		return;
	}

	bool needs_gpu_update = false;
	for (i32 i = 0; i < GI_Scene::probes_to_update_per_frame; ++i)
	{
		GI_Probe& probe_idx_to_update = in_gi_scene.probes[in_gi_scene.probe_idx_to_update];

		if (probe_idx_to_update.atlas_idx < 0)
		{
			probe_idx_to_update.atlas_idx = in_gi_scene.next_atlas_index;
			in_gi_scene.next_atlas_index += 1;
			needs_gpu_update = true;
		}
		
		const HMM_Vec3 lighting_capture_position = gi_probe_position_from_index(in_gi_scene.probe_idx_to_update);
		in_gi_scene.lighting_capture.render(
				in_state, 
				lighting_capture_position, 
				probe_idx_to_update.atlas_idx
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

		// Update probe_idx_to_update for next update call
		in_gi_scene.probe_idx_to_update = (in_gi_scene.probe_idx_to_update + 1) % in_gi_scene.probes.length();

		if (in_gi_scene.probe_idx_to_update == 0)
		{
			in_state.gi_is_updating = false;
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
	};
	// Apply Vertex Uniforms
	sg_apply_uniforms(0, SG_RANGE(vs_params));

	gi_debug_fs_params_t fs_params = {
		.atlas_total_size = in_gi_scene.lighting_capture.desc.octahedral_total_size,
		.atlas_entry_size = in_gi_scene.lighting_capture.desc.octahedral_entry_size,
		.probe_vis_mode = static_cast<i32>(state.probe_vis_mode),
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
		},
		.samplers = {
			[0] = state.linear_sampler,
			[1] = state.nearest_sampler,
		},
	};
	sg_apply_bindings(&bindings);
	sg_draw(0, in_gi_scene.debug_sphere.index_count, in_gi_scene.probes.length());

}

