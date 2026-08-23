#pragma once

#include "core/types.h"
#include "core/timings.h"
#include "render/frame_render_graph.h"
#include "render/fullscreen_pipeline.h"
#include "render/vulkan_context.h"
#include "state/state.h"

// GPU skinning cache: a compute pass bakes
// each skinned mesh's posed vertices into a per-mesh cache buffer so
// tessellation and the wire overlay can consume skinned meshes as static
// geometry. It runs only for tessellation or shaded-wireframe consumers; the
// normal draw path keeps in-shader skinning. Skin matrices come from the
// shared per-frame arena ring, so push constants carry each mesh's offset.
// Each cache remains valid until the source pose changes.

namespace GpuSkinning
{
	constexpr u32 WORKGROUP_SIZE = 64;
	constexpr u32 MAX_COMPUTE_GROUPS_PER_DISPATCH = 65535;
	constexpr u32 MAX_SKINNED_DISPATCH_SETS_PER_FRAME = 256;

	struct SkinningParams
	{
		i32 vertex_count = 0;
		i32 base_vertex = 0;
		i32 skin_matrix_offset = 0;
		i32 _padding0 = 0;
	};
	static_assert(sizeof(SkinningParams) == 16, "Must match gpu_skinning.comp's push constant block");

	inline TypedComputeEffect<SkinningParams> effect;

	inline void init(VulkanContext* ctx)
	{
		DescriptorBindingSpec bindings[4] = {};
		for (u32 binding_idx = 0; binding_idx < 4; ++binding_idx)
		{
			bindings[binding_idx] = {
				.binding = binding_idx,
				.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.stages = VK_SHADER_STAGE_COMPUTE_BIT,
			};
		}
		effect.init(ctx, {
			.shader_path = "bin/shaders/gpu_skinning.comp.spv",
			.bindings = bindings,
			.binding_count = 4,
		});
	}

	inline void ensure_cache(Mesh& in_mesh)
	{
		if (in_mesh.skinned_vertex_cache_capacity >= in_mesh.vertex_count
			&& in_mesh.skinned_vertex_cache_buffer.is_gpu_buffer_valid())
		{
			return;
		}

		in_mesh.skinned_vertex_cache_buffer.destroy_gpu_buffer();
		in_mesh.skinned_vertex_cache_capacity = MAX(in_mesh.vertex_count, 1u);
		in_mesh.skinned_vertex_cache_buffer = GpuBuffer((GpuBufferDesc<Vertex>) {
			.data = nullptr,
			.size = sizeof(Vertex) * in_mesh.skinned_vertex_cache_capacity,
			.usage = {
				.vertex_buffer = true,
				.storage_buffer = true,
			},
			.label = "Mesh::skinned_vertex_cache_buffer",
		});
	}

	// Records the cache dispatch for one mesh into the frame's command buffer
	inline void update_mesh(
		FrameRenderGraph& graph,
		VulkanContext* ctx,
		State& in_state,
		Mesh& in_mesh)
	{
		in_mesh.skinned_vertex_cache_valid = false;
		if (!in_mesh.has_skinned_vertices
			|| in_mesh.vertex_count == 0
			|| in_mesh.skinned_vertices == nullptr
			|| in_mesh.skin_matrices == nullptr
			|| in_mesh.skin_matrix_count == 0
			|| in_mesh.skin_matrix_arena_offset < 0)
		{
			return;
		}

		ensure_cache(in_mesh);

		VkBuffer buffers[] = {
			in_mesh.vertex_buffer.get_gpu_buffer(),
			in_mesh.skinned_vertex_buffer.get_gpu_buffer(),
			get_skin_matrix_arena_buffer(in_state).get_gpu_buffer(),
			in_mesh.skinned_vertex_cache_buffer.get_gpu_buffer(),
		};
		DescriptorWriter writer = effect.writer(ctx);
		for (u32 binding_idx = 0; binding_idx < 4; ++binding_idx)
		{
			writer.buffer(binding_idx, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				buffers[binding_idx]);
		}
		writer.commit();
		graph.storage_read(frame_graph_buffer(buffers[0]));
		graph.storage_read(frame_graph_buffer(buffers[1]));
		graph.storage_read(frame_graph_buffer(buffers[2]));
		graph.storage_write(frame_graph_buffer(buffers[3]));
		graph.compute([&]() {
			effect.bind(ctx, writer.set);
			for (u32 base_vertex = 0; base_vertex < in_mesh.vertex_count;
				base_vertex += MAX_COMPUTE_GROUPS_PER_DISPATCH * WORKGROUP_SIZE)
			{
				const u32 remaining_vertices = in_mesh.vertex_count - base_vertex;
				const u32 dispatch_vertex_count = MIN(remaining_vertices,
					MAX_COMPUTE_GROUPS_PER_DISPATCH * WORKGROUP_SIZE);
				const u32 group_count =
					(dispatch_vertex_count + WORKGROUP_SIZE - 1u) / WORKGROUP_SIZE;
				SkinningParams params = {
					.vertex_count = (i32) in_mesh.vertex_count,
					.base_vertex = (i32) base_vertex,
					.skin_matrix_offset = in_mesh.skin_matrix_arena_offset,
				};
				effect.dispatch(ctx, params, group_count, 1, 1);
			}
		});
		graph.storage_read(frame_graph_buffer(buffers[3]),
			VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
		graph.vertex(frame_graph_buffer(buffers[3]));

		in_mesh.skinned_vertex_cache_valid = true;
	}

	// Records all cache dispatches + the barrier making the caches visible to
	// vertex input/shaders. Call after begin_frame, before any pass executes.
	inline void update(
		FrameRenderGraph& graph,
		VulkanContext* ctx,
		State& in_state,
		const bool in_required)
	{
		scene_ensure_indexes(in_state);
		in_state.data_oriented.frame.gpu_skinning_candidate_count += (i32) in_state.scene.indexes.skinned_mesh_object_ids.length();

		if (!in_required)
		{
			for (const i32 unique_id : in_state.scene.indexes.skinned_mesh_object_ids)
			{
				auto found = in_state.scene.objects.find(unique_id);
				if (found == in_state.scene.objects.end())
				{
					continue;
				}
				found->second.mesh.skinned_vertex_cache_valid = false;
			}
			return;
		}

		CPU_TIMING_SCOPE("GPU Skinning Cache");

		u32 dispatch_count = 0;
		for (const i32 unique_id : in_state.scene.indexes.skinned_mesh_object_ids)
		{
			auto found = in_state.scene.objects.find(unique_id);
			if (found == in_state.scene.objects.end())
			{
				continue;
			}
			if (dispatch_count >= MAX_SKINNED_DISPATCH_SETS_PER_FRAME)
			{
				break;
			}

			Mesh& mesh = found->second.mesh;
			update_mesh(graph, ctx, in_state, mesh);
			if (mesh.skinned_vertex_cache_valid)
			{
				in_state.data_oriented.frame.gpu_skinning_updated_count += 1;
			}
			dispatch_count += 1;
		}
	}

	inline void shutdown(VulkanContext* ctx)
	{
		effect.shutdown(ctx);
	}
}
