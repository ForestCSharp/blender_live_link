#pragma once

#include <optional>

#include "gpu_skinning.compiled.h"

#include "core/timings.h"
#include "render/sokol_helpers.h"
#include "state/state.h"

namespace GpuSkinning
{
	static constexpr u32 WORKGROUP_SIZE = 64;
	static constexpr u32 MAX_COMPUTE_GROUPS_PER_DISPATCH = 65535;

	struct SkinningParams
	{
		i32 vertex_count = 0;
		i32 base_vertex = 0;
		i32 padding0 = 0;
		i32 padding1 = 0;
	};

	static_assert(sizeof(SkinningParams) == 16, "SkinningParams must match gpu_skinning skinning_params shader layout.");

	std::optional<sg_shader> skin_vertices_shader;
	sg_pipeline skin_vertices_pipeline = {};
	bool initialized = false;

	void init()
	{
		if (initialized)
		{
			return;
		}

		skin_vertices_shader = sg_make_shader(gpu_skinning_skin_vertices_shader_desc(sg_query_backend()));
		skin_vertices_pipeline = sg_make_pipeline((sg_pipeline_desc) {
			.compute = true,
			.shader = skin_vertices_shader.value(),
			.label = "gpu-skinning-skin-vertices-pipeline",
		});

		initialized = true;
	}

	void ensure_cache(Mesh& mesh)
	{
		if (mesh.skinned_vertex_cache_capacity >= mesh.vertex_count &&
			mesh.skinned_vertex_cache_buffer.is_gpu_buffer_valid())
		{
			return;
		}

		mesh.skinned_vertex_cache_buffer.destroy_gpu_buffer();
		mesh.skinned_vertex_cache_capacity = MAX(mesh.vertex_count, 1u);
		mesh.skinned_vertex_cache_buffer = GpuBuffer((GpuBufferDesc<Vertex>) {
			.data = nullptr,
			.size = sizeof(Vertex) * mesh.skinned_vertex_cache_capacity,
			.usage = {
				.vertex_buffer = true,
				.storage_buffer = true,
			},
			.label = "Mesh::skinned_vertex_cache_buffer",
		});
		mesh.skinned_vertex_cache_buffer.get_storage_view();
	}

	void update_mesh(Mesh& mesh)
	{
		mesh.skinned_vertex_cache_valid = false;
		if (!mesh.has_skinned_vertices ||
			mesh.vertex_count == 0 ||
			mesh.skinned_vertices == nullptr ||
			mesh.skin_matrices == nullptr ||
			mesh.skin_matrix_count == 0)
		{
			return;
		}

		ensure_cache(mesh);

		const char* debug_label = "GPU Skin Vertices";
		for (u32 base_vertex = 0; base_vertex < mesh.vertex_count; base_vertex += MAX_COMPUTE_GROUPS_PER_DISPATCH * WORKGROUP_SIZE)
		{
			const u32 remaining_vertices = mesh.vertex_count - base_vertex;
			const u32 dispatch_vertex_count = MIN(remaining_vertices, MAX_COMPUTE_GROUPS_PER_DISPATCH * WORKGROUP_SIZE);
			const u32 group_count = (dispatch_vertex_count + WORKGROUP_SIZE - 1u) / WORKGROUP_SIZE;
			SkinningParams params = {
				.vertex_count = (i32) mesh.vertex_count,
				.base_vertex = (i32) base_vertex,
			};

			sg_bindings bindings = {
				.views = {
					[0] = mesh.vertex_buffer.get_storage_view(),
					[1] = mesh.skinned_vertex_buffer.get_storage_view(),
					[2] = mesh.skin_matrix_buffer.get_storage_view(),
					[3] = mesh.skinned_vertex_cache_buffer.get_storage_view(),
				},
			};

			{
				CPU_TIMING_BACKEND_SCOPE("sg_begin_pass", debug_label);
				sg_begin_pass((sg_pass) { .compute = true, .label = debug_label });
			}
			{
				GpuDebugScope debug_scope(debug_label);
				sg_apply_pipeline(skin_vertices_pipeline);
				sg_apply_uniforms(0, SG_RANGE(params));
				gpu_apply_bindings(&bindings);
				sg_dispatch((i32) group_count, 1, 1);
			}
			{
				CPU_TIMING_BACKEND_SCOPE("sg_end_pass", debug_label);
				sg_end_pass();
			}
		}

		mesh.skinned_vertex_cache_valid = true;
	}

	void update(State& state, const bool required)
	{
		if (!required)
		{
			for (auto& [unique_id, object] : state.scene.objects)
			{
				if (object.has_mesh && object.mesh.has_skinned_vertices)
				{
					object.mesh.skinned_vertex_cache_valid = false;
				}
			}
			return;
		}

		init();
		for (auto& [unique_id, object] : state.scene.objects)
		{
			if (!object.has_mesh || !object.mesh.has_skinned_vertices)
			{
				continue;
			}

			update_mesh(object.mesh);
		}
	}
}
