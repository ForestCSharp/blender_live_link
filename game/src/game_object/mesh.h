#pragma once

#include "core/stretchy_buffer.h"
#include "render/gpu_buffer.h"
#include "render/render_types.h"
#include "shaders/tessellation_common.h"

static_assert(sizeof(Vertex) == 48, "Vertex must match TessellationVertex shader layout.");
static_assert(sizeof(TessellationPatch) == 80, "TessellationPatch must match shader storage layout.");
static_assert(sizeof(TessellationWeldPair) == 16, "TessellationWeldPair must match shader storage layout.");
static_assert(sizeof(TessellationCounters) == 32, "TessellationCounters must match shader storage layout.");

struct MeshInitData
{	
	u32 num_indices = 0;
	u32* indices = nullptr;

	u32 num_vertices = 0;
	Vertex* vertices = nullptr;
	SkinnedVertex* skinned_vertices = nullptr;
	u32 skin_matrix_count = 0;
	i32 armature_id = -1;
	HMM_Mat4 mesh_to_armature = HMM_M4D(1.0f);
	HMM_Mat4 armature_to_mesh = HMM_M4D(1.0f);

	u32 num_material_indices = 0;
	i32* material_indices = nullptr;
};

struct TessellatedGeometry
{
	static constexpr u32 GPU_SLOT_COUNT = 2;

	struct GpuSlot
	{
		bool readback_requested = false;
		bool has_counts = false;
		u32 patch_capacity = 0;
		u32 vertex_capacity = 0;
		u32 index_capacity = 0;
		u32 wire_index_capacity = 0;
		TessellationCounters counters = {};

		GpuBuffer<TessellationCounters> counters_buffer;
		GpuBuffer<TessellationPatch> patch_buffer;
		GpuBuffer<Vertex> vertex_buffer;
		GpuBuffer<u32> index_buffer;
		GpuBuffer<u32> wire_index_buffer;
		sg_buffer_readback counters_readback = {};
	};

	bool active = false;
	bool overflowed = false;
	bool gpu_planned = false;
	bool readback_supported = false;
	u32 active_gpu_slot = GPU_SLOT_COUNT;
	u32 next_gpu_slot = 0;
	u32 readback_age = 0;

	u32 patch_count = 0;
	u32 vertex_count = 0;
	u32 index_count = 0;
	u32 wire_index_count = 0;
	u32 edge_weld_pair_count = 0;

	u32 patch_capacity = 0;
	u32 vertex_capacity = 0;
	u32 index_capacity = 0;
	u32 wire_index_capacity = 0;
	u32 edge_weld_pair_capacity = 0;

	StretchyBuffer<TessellationPatch> patches;
	StretchyBuffer<TessellationWeldPair> edge_weld_pairs;

	GpuBuffer<TessellationPatch> patch_buffer;
	GpuBuffer<TessellationWeldPair> edge_weld_pair_buffer;
	GpuBuffer<Vertex> vertex_buffer;
	GpuBuffer<u32> index_buffer;
	GpuBuffer<u32> wire_index_buffer;

	GpuSlot gpu_slots[GPU_SLOT_COUNT];
};

struct MeshRenderView
{
	sg_buffer vertex_buffer = {};
	sg_buffer index_buffer = {};
	sg_buffer wire_index_buffer = {};
	u32 index_count = 0;
	u32 wire_index_count = 0;
	bool is_tessellated = false;
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

		/*
			* We add (latitudes + 1) vertices per longitude because of equator,
			* the North pole and South pole are not counted here, as they overlap.
			* The first and last vertices have same position and normal, but
			* different tex coords.
		*/
		for (i32 j = 0; j <= longitudes; ++j)
		{
			f32 longitudeAngle = j * deltaLongitude;

			Vertex& vertex = vertices[current_vertex_idx++];

			HMM_Vec3 local_position = HMM_V3(0,0,0);		
			local_position.X += xz * cosf(longitudeAngle);	
			local_position.Y += y;
			local_position.Z += xz * sinf(longitudeAngle);		/* z = r * sin(phi) */
			vertex.position = HMM_V4V(local_position, 1.0);

			HMM_Vec3 world_normal = local_position *  1.0f / radius;
			vertex.normal = HMM_V4V(world_normal, 0.0f);

			//FCS TODO:
			//vertex.uv.x = u;
			//vertex.uv.y = v;
		}
	}

	/*
	*  Indices
	*  k1--k1+1
	*  |  / |
	*  | /  |
	*  k2--k2+1
	*/
	u32 current_index_index = 0;
	for(int i = 0; i < latitudes; ++i)
	{
		u32 k1 = (i * (longitudes + 1));
		u32 k2 = (k1 + longitudes + 1);
		// 2 Triangles per latitude block excluding the first and last longitudes blocks
		for(int j = 0; j < longitudes; ++j, ++k1, ++k2)
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

		// Unused data
		.skinned_vertices = nullptr,
		.skin_matrix_count = 0,
		.armature_id = -1,
		.mesh_to_armature = HMM_M4D(1.0f),
		.armature_to_mesh = HMM_M4D(1.0f),
		.num_material_indices = 0,
		.material_indices = nullptr,
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

	bool has_skinned_vertices;
	SkinnedVertex* skinned_vertices;
	GpuBuffer<SkinnedVertex> skinned_vertex_buffer;
	u32 skin_matrix_count;
	HMM_Mat4* skin_matrices;
	GpuBuffer<HMM_Mat4> skin_matrix_buffer;
	i32 armature_id;
	HMM_Mat4 mesh_to_armature;
	HMM_Mat4 armature_to_mesh;

	u32 material_indices_count;
	i32* material_indices;

	BoundingBox bounding_box;

	TessellatedGeometry tessellated_geometry;
};

// Takes ownership of vertices and indices
Mesh make_mesh(const MeshInitData& in_init_data)
{
	BoundingBox bounding_box = bounding_box_init();

	for (i32 vtx_idx = 0; vtx_idx < in_init_data.num_vertices; ++vtx_idx)
	{
		Vertex v = in_init_data.vertices[vtx_idx];
		HMM_Vec3 vtx_pos = v.position.XYZ;
		bounding_box_expand(bounding_box, vtx_pos);
	}

	u64 indices_size = sizeof(u32) * in_init_data.num_indices;
	u64 vertices_size = sizeof(Vertex) * in_init_data.num_vertices;
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

	Mesh out_mesh {
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
		.has_skinned_vertices = false,
		.skinned_vertices = in_init_data.skinned_vertices,
		.skin_matrix_count = 0,
		.skin_matrices = nullptr,
		.armature_id = in_init_data.armature_id,
		.mesh_to_armature = in_init_data.mesh_to_armature,
		.armature_to_mesh = in_init_data.armature_to_mesh,
		.material_indices_count = in_init_data.num_material_indices,
		.material_indices = in_init_data.material_indices,
		.bounding_box = bounding_box,
	};

	if (in_init_data.skinned_vertices != nullptr)
	{
		u64 skinned_vertices_size = sizeof(SkinnedVertex) * in_init_data.num_vertices;
		out_mesh.skin_matrix_count = MAX(in_init_data.skin_matrix_count, 1);
		out_mesh.skin_matrices = (HMM_Mat4*) malloc(sizeof(HMM_Mat4) * out_mesh.skin_matrix_count);
		for (u32 matrix_idx = 0; matrix_idx < out_mesh.skin_matrix_count; ++matrix_idx)
		{
			out_mesh.skin_matrices[matrix_idx] = HMM_M4D(1.0f);
		}

		out_mesh.has_skinned_vertices = true;
		out_mesh.skinned_vertex_buffer = GpuBuffer((GpuBufferDesc<SkinnedVertex>){
			.data = in_init_data.skinned_vertices,
			.size = skinned_vertices_size,
			.usage = {
				.vertex_buffer = true,
				.storage_buffer = true,
			},
			.label = "Mesh::skinned_vertex_buffer",
		});
		out_mesh.skin_matrix_buffer = GpuBuffer((GpuBufferDesc<HMM_Mat4>){
			.data = nullptr,
			.size = sizeof(HMM_Mat4) * out_mesh.skin_matrix_count,
			.usage = {
				.storage_buffer = true,
				.stream_update = true,
			},
			.label = "Mesh::skin_matrix_buffer",
		});
	}

	return out_mesh;
}

MeshRenderView mesh_get_render_view(Mesh& in_mesh)
{
	if (in_mesh.tessellated_geometry.active && in_mesh.tessellated_geometry.index_count > 0)
	{
		if (in_mesh.tessellated_geometry.gpu_planned &&
			in_mesh.tessellated_geometry.active_gpu_slot < TessellatedGeometry::GPU_SLOT_COUNT)
		{
			TessellatedGeometry::GpuSlot& slot = in_mesh.tessellated_geometry.gpu_slots[in_mesh.tessellated_geometry.active_gpu_slot];
			return (MeshRenderView) {
				.vertex_buffer = slot.vertex_buffer.get_gpu_buffer(),
				.index_buffer = slot.index_buffer.get_gpu_buffer(),
				.wire_index_buffer = slot.wire_index_buffer.get_gpu_buffer(),
				.index_count = in_mesh.tessellated_geometry.index_count,
				.wire_index_count = in_mesh.tessellated_geometry.wire_index_count,
				.is_tessellated = true,
			};
		}

		return (MeshRenderView) {
			.vertex_buffer = in_mesh.tessellated_geometry.vertex_buffer.get_gpu_buffer(),
			.index_buffer = in_mesh.tessellated_geometry.index_buffer.get_gpu_buffer(),
			.wire_index_buffer = in_mesh.tessellated_geometry.wire_index_buffer.get_gpu_buffer(),
			.index_count = in_mesh.tessellated_geometry.index_count,
			.wire_index_count = in_mesh.tessellated_geometry.wire_index_count,
			.is_tessellated = true,
		};
	}

	return (MeshRenderView) {
		.vertex_buffer = in_mesh.vertex_buffer.get_gpu_buffer(),
		.index_buffer = in_mesh.index_buffer.get_gpu_buffer(),
		.wire_index_buffer = in_mesh.wire_index_buffer.get_gpu_buffer(),
		.index_count = in_mesh.index_count,
		.wire_index_count = in_mesh.wire_index_count,
		.is_tessellated = false,
	};
}

bool mesh_render_view_uses_skinning(const Mesh& in_mesh, const MeshRenderView& in_render_view)
{
	return in_mesh.has_skinned_vertices && !in_render_view.is_tessellated;
}

void mesh_apply_render_bindings(sg_bindings& out_bindings, Mesh& in_mesh, const MeshRenderView& in_render_view)
{
	out_bindings.vertex_buffers[0] = in_render_view.vertex_buffer;
	out_bindings.index_buffer = in_render_view.index_buffer;

	if (mesh_render_view_uses_skinning(in_mesh, in_render_view))
	{
		out_bindings.vertex_buffers[1] = in_mesh.skinned_vertex_buffer.get_gpu_buffer();
		out_bindings.views[6] = in_mesh.skin_matrix_buffer.get_storage_view();
	}
}

void mesh_cleanup_tessellated_geometry(Mesh& in_mesh)
{
	TessellatedGeometry& tessellated = in_mesh.tessellated_geometry;
	tessellated.patch_buffer.destroy_gpu_buffer();
	tessellated.edge_weld_pair_buffer.destroy_gpu_buffer();
	tessellated.vertex_buffer.destroy_gpu_buffer();
	tessellated.index_buffer.destroy_gpu_buffer();
	tessellated.wire_index_buffer.destroy_gpu_buffer();
	for (u32 slot_index = 0; slot_index < TessellatedGeometry::GPU_SLOT_COUNT; ++slot_index)
	{
		TessellatedGeometry::GpuSlot& slot = tessellated.gpu_slots[slot_index];
		slot.counters_buffer.destroy_gpu_buffer();
		slot.patch_buffer.destroy_gpu_buffer();
		slot.vertex_buffer.destroy_gpu_buffer();
		slot.index_buffer.destroy_gpu_buffer();
		slot.wire_index_buffer.destroy_gpu_buffer();
		if (slot.counters_readback.impl)
		{
			sg_destroy_buffer_readback(slot.counters_readback);
		}
	}
	tessellated.patches.reset();
	tessellated.edge_weld_pairs.reset();
	tessellated = {};
}
