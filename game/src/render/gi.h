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

	// Probe Update State
	LightingCapture lighting_capture;
	i32 probe_idx_to_update = 0;
	i32 next_atlas_index = 0;

	// Debug Data
	Mesh debug_sphere;
	sg_pipeline gi_debug_pipeline;

};

void gi_scene_init(GI_Scene& out_gi_scene)
{
	out_gi_scene.cells.reset();
	out_gi_scene.probes.reset();

	out_gi_scene.cells.add_uninitialized(GI_CELL_COUNT);
	out_gi_scene.probes.add_uninitialized(GI_PROBE_COUNT);

	const bool log_scene_init = true;

	//FCS TODO: CHECK
	for (i32 cell_idx = 0; cell_idx < GI_CELL_COUNT; ++cell_idx)
	{
		GI_Cell& cell = out_gi_scene.cells[cell_idx];
		GI_Coords cell_coords = gi_cell_coords_from_index(cell_idx);

		if (log_scene_init)
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

					if (log_scene_init)
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
	
	const LightingCaptureDesc lighting_capture_desc = {
		.cubemap_render_size = 256,
		.octahedral_total_size = 4096,
		.octahedral_entry_size = 128,
	};
	out_gi_scene.lighting_capture.init(lighting_capture_desc);

	// Setup Debug Data
	out_gi_scene.debug_sphere = make_mesh(mesh_init_data_uv_sphere(0.1f,24,24));
	out_gi_scene.gi_debug_pipeline = sg_make_pipeline(GIDebugPass::make_pipeline_desc(sglue_swapchain().depth_format));
}

void gi_scene_update(GI_Scene& in_gi_scene, State& in_state)
{
	assert(in_gi_scene.probes.is_valid_index(in_gi_scene.probe_idx_to_update));

	GI_Probe& probe_idx_to_update = in_gi_scene.probes[in_gi_scene.probe_idx_to_update];

	if (probe_idx_to_update.atlas_idx < 0)
	{
		probe_idx_to_update.atlas_idx = in_gi_scene.next_atlas_index;
		in_gi_scene.next_atlas_index += 1;

		in_gi_scene.probes_buffer.update_gpu_buffer(
			(sg_range){
				.ptr = in_gi_scene.probes.data(), 
				.size = sizeof(GI_Probe) * in_gi_scene.probes.length(),
			}
		);
	}
	
	const HMM_Vec3 lighting_capture_position = gi_probe_position_from_index(in_gi_scene.probe_idx_to_update);
	in_gi_scene.lighting_capture.render(
			in_state, 
			lighting_capture_position, 
			probe_idx_to_update.atlas_idx
	);

	const bool log_update = false;
	if (log_update)
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
	};
	// Apply Fragment Uniforms
	sg_apply_uniforms(1, SG_RANGE(fs_params));


	GpuImage& octahedral_atlas = in_gi_scene.lighting_capture.cube_to_oct_pass.get_color_output(0);

	sg_bindings bindings = {
		.vertex_buffers[0] = in_gi_scene.debug_sphere.vertex_buffer.get_gpu_buffer(),
		.index_buffer = in_gi_scene.debug_sphere.index_buffer.get_gpu_buffer(),
		.views = {
			[0] = octahedral_atlas.get_texture_view(0),
			[1] = in_gi_scene.probes_buffer.get_storage_view(),
		},
		.samplers[0] = state.sampler,
	};
	sg_apply_bindings(&bindings);
	sg_draw(0, in_gi_scene.debug_sphere.index_count, in_gi_scene.probes.length());

}

