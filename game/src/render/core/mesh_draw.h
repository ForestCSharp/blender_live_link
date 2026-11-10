#pragma once

#include "game_object/mesh.h"

// Start with empty bindings for each pass, cascade, or probe face.
struct MeshDrawBindings
{
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkBuffer vertex_buffer = VK_NULL_HANDLE;
	VkBuffer skinned_vertex_buffer = VK_NULL_HANDLE;
	VkBuffer index_buffer = VK_NULL_HANDLE;
};

inline bool draw_indexed_mesh(
	VulkanContext* ctx, Mesh& in_mesh, i32 in_object_index,
	VkPipeline in_pipeline, VkPipeline in_skinned_pipeline, MeshDrawBindings& bindings)
{
	VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);

	// Query the arena first: mesh_get_render_view lazily allocates per-mesh buffers.
	MeshArenaSlice arena_slice;
	const bool from_arena = mesh_get_arena_slice(in_mesh, arena_slice);

	MeshRenderView render_view = {};
	bool skinned = false;
	if (!from_arena)
	{
		render_view = mesh_get_render_view(in_mesh);
		skinned = in_mesh.has_skinned_vertices && !render_view.is_tessellated;
		if (skinned && in_mesh.skin_matrix_arena_offset < 0)
		{
			return false;
		}
	}

	VkPipeline wanted_pipeline = skinned ? in_skinned_pipeline : in_pipeline;
	if (bindings.pipeline != wanted_pipeline)
	{
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted_pipeline);
		bindings.pipeline = wanted_pipeline;
	}

	VkBuffer vertex_buffer = from_arena
		? g_geometry_arena.vertex_buffer.get_gpu_buffer()
		: render_view.vertex_buffer;
	VkBuffer index_buffer = from_arena
		? g_geometry_arena.index_buffer.get_gpu_buffer()
		: render_view.index_buffer;

	if (bindings.vertex_buffer != vertex_buffer)
	{
		VkDeviceSize vertex_buffer_offset = 0;
		vulkan_cmd_bind_vertex_buffers(ctx, 0, 1, &vertex_buffer, &vertex_buffer_offset);
		bindings.vertex_buffer = vertex_buffer;
	}

	if (skinned)
	{
		VkBuffer skinned_vertex_buffer = in_mesh.skinned_vertex_buffer.get_gpu_buffer();
		if (bindings.skinned_vertex_buffer != skinned_vertex_buffer)
		{
			VkDeviceSize skinned_offset = 0;
			vulkan_cmd_bind_vertex_buffers(ctx, 1, 1, &skinned_vertex_buffer, &skinned_offset);
			bindings.skinned_vertex_buffer = skinned_vertex_buffer;
		}
	}

	if (bindings.index_buffer != index_buffer)
	{
		vulkan_cmd_bind_index_buffer(ctx, index_buffer, 0, VK_INDEX_TYPE_UINT32);
		bindings.index_buffer = index_buffer;
	}

	// firstInstance carries the object index; the shader reads gl_InstanceIndex.
	if (from_arena)
	{
		vulkan_cmd_draw_indexed(ctx, arena_slice.index_count, 1,
			arena_slice.first_index, (i32) arena_slice.vertex_offset, (u32) in_object_index);
	}
	else if (render_view.is_tessellated)
	{
		// The index count lives in a GPU-written command; the CPU only knows the
		// capacity, which would draw past what was emitted.
		vulkan_cmd_draw_indexed_indirect(ctx, render_view.draw_command_buffer, 0, 1,
			sizeof(VkDrawIndexedIndirectCommand));
	}
	else
	{
		vulkan_cmd_draw_indexed(ctx, render_view.index_count, 1, 0, 0, (u32) in_object_index);
	}
	return true;
}
