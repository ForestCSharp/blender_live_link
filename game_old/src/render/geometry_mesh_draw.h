#pragma once

#include "render/culling.h"
#include "render/geometry_pass.h"
#include "state/state.h"

static void draw_visible_geometry_meshes(
	State& state,
	const CullResult& cull_result,
	RenderPass& geometry_pass,
	const geometry_vs_params_t& vs_params,
	const geometry_fs_params_t& fs_params,
	sg_pixel_format skinned_depth_format)
{
	state.data_oriented.frame.draw_calls += 1;
	if (!state.render_objects.valid)
	{
		return;
	}

	sg_view render_object_data_view = get_render_object_snapshot_buffer(state).get_storage_view();

	for (const i32 unique_id : cull_result.object_ids)
	{
		if (!state.scene.objects.contains(unique_id))
		{
			continue;
		}

		Object& object = state.scene.objects[unique_id];

		if (!object.has_mesh)
		{
			continue;
		}
		if (object.render_object_index < 0)
		{
			continue;
		}

		Mesh& mesh = object.mesh;
		MeshRenderView render_view = mesh_get_render_view(mesh);

		int mesh_material_idx = mesh.material_indices[0];
		assert(mesh_material_idx >= 0);
		const geometry_Material_t& material = state.materials.items[mesh_material_idx];

		GpuImage& base_color_image = material.base_color_image_index >= 0 ? state.images.items[material.base_color_image_index] : state.gpu.default_image;
		GpuImage& metallic_image = material.metallic_image_index >= 0 ? state.images.items[material.metallic_image_index] : state.gpu.default_image;
		GpuImage& roughness_image = material.roughness_image_index >= 0 ? state.images.items[material.roughness_image_index] : state.gpu.default_image;
		GpuImage& emission_color_image = material.emission_color_image_index >= 0 ? state.images.items[material.emission_color_image_index] : state.gpu.default_image;

		sg_bindings bindings = {
			.views = {
				[0] = render_object_data_view,
				[1] = get_materials_buffer().get_storage_view(),
				[2] = base_color_image.get_texture_view(0),
				[3] = metallic_image.get_texture_view(0),
				[4] = roughness_image.get_texture_view(0),
				[5] = emission_color_image.get_texture_view(0),
			},
			.samplers[0] = state.gpu.linear_sampler,
		};
		const bool uses_skinning = mesh_render_view_uses_skinning(mesh, render_view);
		sg_apply_pipeline(uses_skinning
			? GeometryPass::get_skinned_pipeline(skinned_depth_format)
			: geometry_pass.pipeline);
		geometry_vs_params_t draw_vs_params = vs_params;
		draw_vs_params.object_index = object.render_object_index;
		sg_apply_uniforms(0, SG_RANGE(draw_vs_params));
		sg_apply_uniforms(1, SG_RANGE(fs_params));
		mesh_apply_render_bindings(bindings, mesh, render_view);
		gpu_apply_bindings(&bindings);
		sg_draw(0, render_view.index_count, 1);
		state.data_oriented.frame.draw_mesh_count += 1;
	}
}
