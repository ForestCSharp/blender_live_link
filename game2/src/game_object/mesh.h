#pragma once

#include "render/gpu_buffer.h"
#include "render/render_types.h"

// Stripped-down port of game/'s mesh.h: no tessellation or skinning yet —
// those return with their render passes.

struct MeshInitData
{
	u32 num_indices = 0;
	u32* indices = nullptr;

	u32 num_vertices = 0;
	Vertex* vertices = nullptr;

	// Material ids from the flatbuffer (raw ids on the live-link thread;
	// resolved to indices at drain — see resolve_mesh_material_indices)
	u32 num_material_indices = 0;
	i32* material_indices = nullptr;

	// Skinning (armature_id > 0 gates the whole block)
	SkinnedVertex* skinned_vertices = nullptr;
	u32 skin_matrix_count = 0;
	i32 armature_id = -1;
	HMM_Mat4 mesh_to_armature = HMM_M4D(1.0f);
	HMM_Mat4 armature_to_mesh = HMM_M4D(1.0f);
};

MeshInitData mesh_init_data_uv_sphere(f32 radius, i32 latitudes, i32 longitudes)
{
	latitudes	= MAX(2, latitudes);
	longitudes	= MAX(3, longitudes);

	u32 num_vertices = (latitudes + 1) * (longitudes + 1);
	Vertex* vertices = (Vertex*) calloc(num_vertices, sizeof(Vertex));

	u32 num_indices = latitudes * longitudes * 6;
	u32* indices = (u32*) calloc(num_indices, sizeof(u32));

	const f64 PI = Constants::Pi;

	f32 deltaLatitude = PI / (f32) latitudes;
	f32 deltaLongitude = 2.0f * PI / (f32) longitudes;

	u32 current_vertex_idx = 0;
	for (i32 i = 0; i <= latitudes; ++i)
	{
		f32 latitudeAngle = PI / 2.0f - i * deltaLatitude;	/* Starting -pi/2 to pi/2 */
		f32 xz = radius * cosf(latitudeAngle);				/* r * cos(phi) */
		f32 y = radius * sinf(latitudeAngle);				/* r * sin(phi )*/

		for (i32 j = 0; j <= longitudes; ++j)
		{
			f32 longitudeAngle = j * deltaLongitude;

			Vertex& vertex = vertices[current_vertex_idx++];

			HMM_Vec3 local_position = HMM_V3(0,0,0);
			local_position.X += xz * cosf(longitudeAngle);
			local_position.Y += y;
			local_position.Z += xz * sinf(longitudeAngle);
			vertex.position = HMM_V4V(local_position, 1.0);

			HMM_Vec3 world_normal = local_position * 1.0f / radius;
			vertex.normal = HMM_V4V(world_normal, 0.0f);
		}
	}

	u32 current_index_index = 0;
	for (int i = 0; i < latitudes; ++i)
	{
		u32 k1 = (i * (longitudes + 1));
		u32 k2 = (k1 + longitudes + 1);
		for (int j = 0; j < longitudes; ++j, ++k1, ++k2)
		{
			if (i != 0)
			{
				indices[current_index_index++] = k1;
				indices[current_index_index++] = k2;
				indices[current_index_index++] = k1 + 1;
			}

			if (i != (latitudes - 1))
			{
				indices[current_index_index++] = k1 + 1;
				indices[current_index_index++] = k2;
				indices[current_index_index++] = k2 + 1;
			}
		}
	}

	return (MeshInitData) {
		.num_indices = num_indices,
		.indices = indices,
		.num_vertices = num_vertices,
		.vertices = vertices,
	};
}

struct Mesh
{
	u32 index_count;
	u32* indices;
	GpuBuffer<u32> index_buffer;

	u32 wire_index_count;
	u32* wire_indices;
	GpuBuffer<u32> wire_index_buffer;

	u32 vertex_count;
	Vertex* vertices;
	GpuBuffer<Vertex> vertex_buffer;

	// Raw material IDS until resolve_mesh_material_indices runs at drain;
	// after that, indices into state.materials.items (-1 = none)
	u32 material_indices_count;
	i32* material_indices;

	// Skinning. skin_matrices is the CPU array update_skinned_animations
	// fills each frame and appends into the shared arena SSBO; there is no
	// per-mesh GPU skin matrix buffer (deviation from game/ — the arena ring
	// makes per-frame writes safe vs frames in flight). The Phase 3 GPU
	// skinning compute pass reads the same arena.
	bool has_skinned_vertices;
	SkinnedVertex* skinned_vertices;
	GpuBuffer<SkinnedVertex> skinned_vertex_buffer;
	u32 skin_matrix_count;
	HMM_Mat4* skin_matrices;
	i32 skin_matrix_arena_offset;	// -1 when not written this frame
	i32 armature_id;
	HMM_Mat4 mesh_to_armature;
	HMM_Mat4 armature_to_mesh;

	BoundingBox bounding_box;
};

// Takes ownership of vertices and indices. Only constructs GpuBuffer
// descriptors — actual GPU buffers are created lazily on the main thread.
Mesh make_mesh(const MeshInitData& in_init_data)
{
	BoundingBox bounding_box = bounding_box_init();

	for (u32 vtx_idx = 0; vtx_idx < in_init_data.num_vertices; ++vtx_idx)
	{
		Vertex v = in_init_data.vertices[vtx_idx];
		HMM_Vec3 vtx_pos = v.position.XYZ;
		bounding_box_expand(bounding_box, vtx_pos);
	}

	u64 indices_size = sizeof(u32) * in_init_data.num_indices;
	u64 vertices_size = sizeof(Vertex) * in_init_data.num_vertices;

	// Wire indices: each triangle becomes 3 lines (for the wire overlay pass)
	u32 source_triangle_count = in_init_data.num_indices / 3;
	u32 source_wire_index_count = source_triangle_count * 6;
	u32* source_wire_indices = (u32*) malloc(sizeof(u32) * source_wire_index_count);
	for (u32 index_idx = 0, wire_idx = 0; index_idx + 2 < in_init_data.num_indices; index_idx += 3)
	{
		const u32 i0 = in_init_data.indices[index_idx + 0];
		const u32 i1 = in_init_data.indices[index_idx + 1];
		const u32 i2 = in_init_data.indices[index_idx + 2];
		source_wire_indices[wire_idx++] = i0;
		source_wire_indices[wire_idx++] = i1;
		source_wire_indices[wire_idx++] = i1;
		source_wire_indices[wire_idx++] = i2;
		source_wire_indices[wire_idx++] = i2;
		source_wire_indices[wire_idx++] = i0;
	}
	u64 wire_indices_size = sizeof(u32) * source_wire_index_count;

	Mesh out_mesh = {
		.index_count = in_init_data.num_indices,
		.indices = in_init_data.indices,
		.index_buffer = GpuBuffer((GpuBufferDesc<u32>){
			.data = in_init_data.indices,
			.size = indices_size,
			.usage = {
				.index_buffer = true,
				.storage_buffer = true,
			},
			.label = "Mesh::index_buffer",
		}),
		.wire_index_count = source_wire_index_count,
		.wire_indices = source_wire_indices,
		.wire_index_buffer = GpuBuffer((GpuBufferDesc<u32>){
			.data = source_wire_indices,
			.size = wire_indices_size,
			.usage = {
				.index_buffer = true,
			},
			.label = "Mesh::wire_index_buffer",
		}),
		.vertex_count = in_init_data.num_vertices,
		.vertices = in_init_data.vertices,
		.vertex_buffer = GpuBuffer((GpuBufferDesc<Vertex>){
			.data = in_init_data.vertices,
			.size = vertices_size,
			.usage = {
				.vertex_buffer = true,
				.storage_buffer = true,
			},
			.label = "Mesh::vertex_buffer",
		}),
		.material_indices_count = in_init_data.num_material_indices,
		.material_indices = in_init_data.material_indices,
		.has_skinned_vertices = false,
		.skinned_vertices = in_init_data.skinned_vertices,
		.skin_matrix_count = 0,
		.skin_matrices = nullptr,
		.skin_matrix_arena_offset = -1,
		.armature_id = in_init_data.armature_id,
		.mesh_to_armature = in_init_data.mesh_to_armature,
		.armature_to_mesh = in_init_data.armature_to_mesh,
		.bounding_box = bounding_box,
	};

	if (in_init_data.skinned_vertices != nullptr)
	{
		out_mesh.has_skinned_vertices = true;
		out_mesh.skin_matrix_count = MAX(in_init_data.skin_matrix_count, 1);

		out_mesh.skinned_vertex_buffer = GpuBuffer((GpuBufferDesc<SkinnedVertex>){
			.data = in_init_data.skinned_vertices,
			.size = sizeof(SkinnedVertex) * in_init_data.num_vertices,
			.usage = {
				.vertex_buffer = true,
				.storage_buffer = true,
			},
			.label = "Mesh::skinned_vertex_buffer",
		});

		out_mesh.skin_matrices = (HMM_Mat4*) malloc(sizeof(HMM_Mat4) * out_mesh.skin_matrix_count);
		for (u32 matrix_idx = 0; matrix_idx < out_mesh.skin_matrix_count; ++matrix_idx)
		{
			out_mesh.skin_matrices[matrix_idx] = HMM_M4D(1.0f);
		}
	}

	return out_mesh;
}
