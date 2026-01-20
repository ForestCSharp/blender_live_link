#pragma once

#include "render/culling.h"
#include "render/gpu_image.h"
#include "render/render_pass.h"
#include "render/geometry_pass.h"
#include "state/state.h"

#include "ankerl/unordered_dense.h"
using ankerl::unordered_dense::map;

//FCS TODO: support cubemap in render_pass.h then we just need one here. Also solves duplicate pipeline issues

struct CubemapDesc
{	
	i32 size;
	HMM_Vec3 location;
};

struct Cubemap
{
public:
	CubemapDesc desc;

	//FCS TODO: REMOVE
	RenderPass geometry_passes[NUM_CUBE_FACES];
	//FCS TODO: REMOVE

	RenderPass geometry_pass_cube;
	bool is_initialized = false;

public:
	Cubemap() = default;
	Cubemap(const CubemapDesc& in_desc)
	: desc(in_desc)
	{		
		// Create geometry pass with correct render size
		RenderPassDesc geometry_pass_desc = GeometryPass::make_render_pass_desc(SG_PIXELFORMAT_DEPTH);
		geometry_pass_desc.initial_width = in_desc.size;
		geometry_pass_desc.initial_height = in_desc.size;

		//FCS TODO: REMOVE
		for (i32 face_idx = 0; face_idx < NUM_CUBE_FACES; ++face_idx)
		{
			geometry_passes[face_idx].init(geometry_pass_desc);
		}
		//FCS TODO: REMOVE

		geometry_pass_desc.type = ERenderPassType::Cubemap;
		geometry_pass_cube.init(geometry_pass_desc);

		is_initialized = true;
	}

	void render(State& in_state)
	{
		assert_msgf(is_initialized, "Cubemap should be initialized with a CubemapDesc passed to its constructor");

		// View + Projection matrix setup
		const f32 fov = HMM_AngleDeg(90.0f);
		const f32 aspect_ratio = 1.0f;
		const f32 projection_near = 0.01f;
		const f32 projection_far = 10000.0f;
		HMM_Mat4 projection_matrix = HMM_Perspective_RH_NO(fov, aspect_ratio, projection_near, projection_far);

		const HMM_Vec3 direction_vectors[NUM_CUBE_FACES] =
		{
			HMM_V3(1,0,0),	// +X
			HMM_V3(-1,0,0),	// -X
			HMM_V3(0,0,1),  // +Z
			HMM_V3(0,0,-1),	// -Z
			HMM_V3(0,1,0),  // +Y
			HMM_V3(0,-1,0), // -Y
		};

		const HMM_Vec3 up_vectors[NUM_CUBE_FACES] =
		{
			HMM_V3(0,1,0),	// +X
			HMM_V3(0,1,0),	// -X
			HMM_V3(0,1,0),	// +Z
			HMM_V3(0,1,0),	// -Z
			HMM_V3(0,0,1),	// +Y
			HMM_V3(0,0,1),	// -Y
		};


		i32 face_idx = 0;
		geometry_pass_cube.execute(
			[&]()
	   		{	
				HMM_Vec3 target = desc.location + direction_vectors[face_idx] * 10;
				HMM_Mat4 view_matrix = HMM_LookAt_RH(desc.location, target, up_vectors[face_idx]);
				HMM_Mat4 view_projection_matrix = HMM_MulM4(projection_matrix, view_matrix);

				geometry_vs_params_t vs_params;
				vs_params.view = view_matrix;
				vs_params.projection = projection_matrix;

				// Apply Vertex Uniforms
				sg_apply_uniforms(0, SG_RANGE(vs_params));

				// Cull objects
				CullResult cull_result = cull_objects(in_state.objects, view_projection_matrix);

				// Submit draw calls for culled objects
				for (auto& [unique_id, object_ptr] : cull_result.objects)
				{
					assert(object_ptr);
					Object& object = *object_ptr;

					if (object.has_mesh)
					{
						Mesh& mesh = object.mesh;

						int mesh_material_idx = mesh.material_indices[0];
						assert(mesh_material_idx >= 0);
						const geometry_Material_t& material = in_state.materials[mesh_material_idx]; 

						GpuImage& base_color_image = material.base_color_image_index >= 0 ? in_state.images[material.base_color_image_index] : in_state.default_image;
						GpuImage& metallic_image = material.metallic_image_index >= 0 ? in_state.images[material.metallic_image_index] : in_state.default_image;
						GpuImage& roughness_image = material.roughness_image_index >= 0 ? in_state.images[material.roughness_image_index] : in_state.default_image;
						GpuImage& emission_color_image = material.emission_color_image_index >= 0 ? in_state.images[material.emission_color_image_index] : in_state.default_image;

						sg_bindings bindings = {
							.vertex_buffers[0] = mesh.vertex_buffer.get_gpu_buffer(),
							.index_buffer = mesh.index_buffer.get_gpu_buffer(),
							.images = {
								[0] = base_color_image.get_gpu_image(),
								[1] = metallic_image.get_gpu_image(),
								[2] = roughness_image.get_gpu_image(),
								[3] = emission_color_image.get_gpu_image(),
							},
							.samplers[0] = in_state.sampler,
							.storage_buffers = {
								[0] = object.storage_buffer.get_gpu_buffer(),
								[1] = get_materials_buffer().get_gpu_buffer(),
							},
						};
						sg_apply_bindings(&bindings);
						sg_draw(0, mesh.index_count, 1);
					}
				}
				face_idx += 1;
			}
		);

		//FCS TODO: REMOVE
		for (i32 face_idx = 0; face_idx < NUM_CUBE_FACES; ++face_idx)
		{
			HMM_Vec3 target = desc.location + direction_vectors[face_idx] * 10;
			HMM_Mat4 view_matrix = HMM_LookAt_RH(desc.location, target, up_vectors[face_idx]);
			HMM_Mat4 view_projection_matrix = HMM_MulM4(projection_matrix, view_matrix);
			
			geometry_passes[face_idx].execute(		
				[&]()
				{	
				    geometry_vs_params_t vs_params;
					vs_params.view = view_matrix;
					vs_params.projection = projection_matrix;

					// Apply Vertex Uniforms
					sg_apply_uniforms(0, SG_RANGE(vs_params));

					// Cull objects
					CullResult cull_result = cull_objects(in_state.objects, view_projection_matrix);

					// Submit draw calls for culled objects
					for (auto& [unique_id, object_ptr] : cull_result.objects)
					{
						assert(object_ptr);
						Object& object = *object_ptr;

						if (object.has_mesh)
						{
							Mesh& mesh = object.mesh;

							int mesh_material_idx = mesh.material_indices[0];
							assert(mesh_material_idx >= 0);
							const geometry_Material_t& material = in_state.materials[mesh_material_idx]; 

							GpuImage& base_color_image = material.base_color_image_index >= 0 ? in_state.images[material.base_color_image_index] : in_state.default_image;
							GpuImage& metallic_image = material.metallic_image_index >= 0 ? in_state.images[material.metallic_image_index] : in_state.default_image;
							GpuImage& roughness_image = material.roughness_image_index >= 0 ? in_state.images[material.roughness_image_index] : in_state.default_image;
							GpuImage& emission_color_image = material.emission_color_image_index >= 0 ? in_state.images[material.emission_color_image_index] : in_state.default_image;

							sg_bindings bindings = {
								.vertex_buffers[0] = mesh.vertex_buffer.get_gpu_buffer(),
								.index_buffer = mesh.index_buffer.get_gpu_buffer(),
								.images = {
									[0] = base_color_image.get_gpu_image(),
									[1] = metallic_image.get_gpu_image(),
									[2] = roughness_image.get_gpu_image(),
									[3] = emission_color_image.get_gpu_image(),
								},
								.samplers[0] = in_state.sampler,
								.storage_buffers = {
									[0] = object.storage_buffer.get_gpu_buffer(),
									[1] = get_materials_buffer().get_gpu_buffer(),
								},
							};
							sg_apply_bindings(&bindings);
							sg_draw(0, mesh.index_count, 1);
						}
					}
				}
			);
		}
		//FCS TODO: REMOVE
	}
};

