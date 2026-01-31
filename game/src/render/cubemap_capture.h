#pragma once

#include "render/culling.h"
#include "render/gpu_image.h"
#include "render/render_pass.h"
#include "render/geometry_pass.h"
#include "render/lighting_pass.h"
#include "state/state.h"

#include "ankerl/unordered_dense.h"
using ankerl::unordered_dense::map;

//FCS TODO: support cubemap in render_pass.h then we just need one here. Also solves duplicate pipeline issues

struct CubemapCaptureDesc
{	
	i32 size;
	HMM_Vec3 location;
};

struct CubemapCapture
{
public:
	CubemapCaptureDesc desc;

	RenderPass geometry_pass;
	RenderPass lighting_pass;

	bool is_initialized = false;

public:
	CubemapCapture() = default;
	CubemapCapture(const CubemapCaptureDesc& in_desc)
	: desc(in_desc)
	{		
		// Create geometry pass with correct render size
		RenderPassDesc geometry_pass_desc = GeometryPass::make_render_pass_desc(SG_PIXELFORMAT_DEPTH);
		geometry_pass_desc.initial_width = in_desc.size;
		geometry_pass_desc.initial_height = in_desc.size;
		geometry_pass_desc.type = ERenderPassType::Multi;
		geometry_pass_desc.pass_count = NUM_CUBE_FACES;
		geometry_pass.init(geometry_pass_desc);

		RenderPassDesc lighting_pass_desc = LightingPass::make_render_pass_desc(SG_PIXELFORMAT_RGBA32F);
		lighting_pass_desc.initial_width = in_desc.size;
		lighting_pass_desc.initial_height = in_desc.size;
		lighting_pass_desc.type = ERenderPassType::Cubemap;
		lighting_pass.init(lighting_pass_desc);

		is_initialized = true;
	}

	void render(State& in_state)
	{
		assert_msgf(is_initialized, "Cubemap should be initialized with a CubemapCaptureDesc passed to its constructor");

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
			HMM_V3(0,1,0),  // +Y
			HMM_V3(0,-1,0), // -Y
			HMM_V3(0,0,-1),	// -Z
			HMM_V3(0,0,1),  // +Z
		};

		const HMM_Vec3 up_vectors[NUM_CUBE_FACES] =
		{
			HMM_V3(0,1,0),	// +X
			HMM_V3(0,1,0),	// -X
			HMM_V3(0,0,1),	// +Y
			HMM_V3(0,0,-1),	// -Y
			HMM_V3(0,1,0),	// +Z
			HMM_V3(0,1,0),	// -Z
		};

		geometry_pass.execute(
			[&](const i32 face_idx)
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
							.views = {
								[0] = object.storage_buffer.get_storage_view(),
								[1] = get_materials_buffer().get_storage_view(), 
								[2] = base_color_image.get_texture_view(0), 
								[3] = metallic_image.get_texture_view(0), 
								[4] = roughness_image.get_texture_view(0),
								[5] = emission_color_image.get_texture_view(0),
							},
							.samplers[0] = in_state.sampler,
						};
						sg_apply_bindings(&bindings);
						sg_draw(0, mesh.index_count, 1);
					}
				}
			}
		);

		//FCS TODO: Is it possible to pass cubemaps across render-passes
		//FCS TODO: probably need to render intermediate passes using texture array

		lighting_pass.execute(
			[&](const i32 face_idx)
	   		{
				in_state.lighting_fs_params.view_position = desc.location;

				// FCS TODO: Get SSAO Working in Cubemap Captures...
				in_state.lighting_fs_params.ssao_enable = false;
				in_state.lighting_fs_params.gi_enable = false;

				// Apply Fragment Uniforms
				sg_apply_uniforms(0, SG_RANGE(in_state.lighting_fs_params));

				GpuImage& color_texture = geometry_pass.get_color_output(0, face_idx);
				GpuImage& position_texture = geometry_pass.get_color_output(1, face_idx);
				GpuImage& normal_texture = geometry_pass.get_color_output(2, face_idx);
				GpuImage& roughness_metallic_texture = geometry_pass.get_color_output(3, face_idx);

				//RenderPass& ssao_blur_pass = get_render_pass(ERenderPass::SSAO_Blur);		
				//sg_image blurred_ssao_texture = ssao_blur_pass.color_outputs[0];	
				// From below...
				//[4] = blurred_ssao_texture.get_texture_view(0),

				sg_bindings bindings = {
					.views = {
						[0] = color_texture.get_texture_view(0),
						[1] = position_texture.get_texture_view(0), 
						[2] = normal_texture.get_texture_view(0),
						[3] = roughness_metallic_texture.get_texture_view(0),
						[4] = in_state.default_image.get_texture_view(0),		//FCS TODO: Need SSAO
						[5] = in_state.point_lights_buffer.get_storage_view(),
						[6] = in_state.spot_lights_buffer.get_storage_view(), 
						[7] = in_state.sun_lights_buffer.get_storage_view(), 
						[8] = state.default_image_cube.get_texture_view(0),
					},
					.samplers[0] = in_state.sampler,
				};
				sg_apply_bindings(&bindings);

				sg_draw(0,6,1);
			}
		);
	}
};

