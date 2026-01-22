#pragma once

#include "render/render_types.h"

struct MeshInitData
{	
	u32 num_indices = 0;
	u32* indices = nullptr;

	u32 num_vertices = 0;
	Vertex* vertices = nullptr;
	SkinnedVertex* skinned_vertices = nullptr;

	u32 num_material_indices = 0;
	i32* material_indices = nullptr;
};

// currently we pass this data to Mesh so shouldn't destroy
void mesh_init_data_free(MeshInitData& in_init_data)
{
	free(in_init_data.indices);
	free(in_init_data.vertices);
	free(in_init_data.skinned_vertices);
	free(in_init_data.material_indices);
	memset(&in_init_data, 0, sizeof(MeshInitData));
}

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
		.num_material_indices = 0,
		.material_indices = nullptr,
	};
}

struct Mesh 
{
	u32 index_count;
	u32* indices;
	GpuBuffer<u32> index_buffer; 

	u32 vertex_count;
	Vertex* vertices; 
	GpuBuffer<Vertex> vertex_buffer;

	bool has_skinned_vertices;
	GpuBuffer<SkinnedVertex> skinned_vertex_buffer;

	u32 material_indices_count;
	i32* material_indices;

	BoundingBox bounding_box;
};

// Takes ownership of vertices and indices
Mesh make_mesh(const MeshInitData& in_init_data)
{
	BoundingBox bounding_box = bounding_box_init();

	for (i32 vtx_idx = 0; vtx_idx < in_init_data.num_vertices; ++vtx_idx)
	{
		Vertex v = in_init_data.vertices[vtx_idx];
		HMM_Vec3 vtx_pos = v.position.XYZ;
		bounding_box.min = HMM_MinV3(bounding_box.min, vtx_pos);
		bounding_box.max = HMM_MaxV3(bounding_box.max, vtx_pos);
	}

	u64 indices_size = sizeof(u32) * in_init_data.num_indices;
	u64 vertices_size = sizeof(Vertex) * in_init_data.num_vertices;

	Mesh out_mesh {
		.index_count = in_init_data.num_indices,
		.indices = in_init_data.indices,
		.index_buffer = GpuBuffer((GpuBufferDesc<u32>){
			.data = in_init_data.indices,
			.size = indices_size,
			.usage = {
				.index_buffer = true,
			},
			.label = "Mesh::index_buffer",
		}),
		.vertex_count = in_init_data.num_vertices,
		.vertices = in_init_data.vertices,
		.vertex_buffer = GpuBuffer((GpuBufferDesc<Vertex>){
			.data = in_init_data.vertices,
			.size = vertices_size,
			.usage = {
				.vertex_buffer = true,
			},
			.label = "Mesh::vertex_buffer",
		}),	
		.material_indices_count = in_init_data.num_material_indices,
		.material_indices = in_init_data.material_indices,
		.bounding_box = bounding_box,
	};

	if (in_init_data.skinned_vertices != nullptr)
	{
		u64 skinned_vertices_size = sizeof(SkinnedVertex) * in_init_data.num_vertices;
		out_mesh.has_skinned_vertices = true;
		out_mesh.skinned_vertex_buffer = GpuBuffer((GpuBufferDesc<SkinnedVertex>){
			.data = in_init_data.skinned_vertices,
			.size = skinned_vertices_size,
			.usage = {
				.vertex_buffer = true,
			},
			.label = "Mesh::skinned_vertex_buffer",
		});
	}

	return out_mesh;
}
