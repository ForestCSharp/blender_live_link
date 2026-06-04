#pragma once

#include "render/culling.h"
#include "render/gpu_image.h"
#include "render/render_pass.h"
#include "render/geometry_pass.h"
#include "render/lighting_pass.h"
#include "render/shader_files.h"
#include "state/state.h"

#include "render/sky_pass.h"

#include "ankerl/unordered_dense.h"
using ankerl::unordered_dense::map;

struct AtlasViewport
{
    int x, y, w, h;
};

AtlasViewport get_atlas_viewport(int atlas_size, int render_size, int idx)
{
    // Basic validation
    assert(atlas_size > 0 && render_size > 0);
    assert(atlas_size >= render_size);
    assert(atlas_size % render_size == 0);

    // Calculate how many tiles fit along one axis
    const int slots_per_dim = atlas_size / render_size;
    const int total_slots = slots_per_dim * slots_per_dim;

    assert(idx >= 0 && idx < total_slots);

    // Map the 1D index to 2D grid coordinates
    const int grid_x = idx % slots_per_dim;
    const int grid_y = idx / slots_per_dim;

    // Return the pixel coordinates for the viewport
    return {
        grid_x * render_size, // x
        grid_y * render_size, // y
        render_size,          // width
        render_size           // height
    };
}

struct LightingCaptureDesc
{
	i32 cubemap_render_size = 256;

	i32 octahedral_total_size = 1024;
	i32 octahedral_entry_size = 16;
};

struct LightingCapture
{
public:
	LightingCaptureDesc desc;

	RenderPass geometry_pass;
	RenderPass lighting_pass;
	RenderPass radial_depth_pass;
	RenderPass cube_to_oct_pass;
	sg_pipeline radiance_projection_pipeline;

	bool is_initialized = false;

	static const sg_pixel_format color_format = SG_PIXELFORMAT_RGBA32F;
	static const sg_pixel_format depth_format = SG_PIXELFORMAT_DEPTH;

public:
	void init(const LightingCaptureDesc& in_desc)
	{
		desc = in_desc;

		// Render geometry pass for 6 cube faces (separate textures, ERenderPassType::Multi)
		RenderPassDesc geometry_pass_desc = GeometryPass::make_render_pass_desc(depth_format);
		geometry_pass_desc.initial_width = in_desc.cubemap_render_size;
		geometry_pass_desc.initial_height = in_desc.cubemap_render_size;
		geometry_pass_desc.type = ERenderPassType::Multi;
		geometry_pass_desc.pass_count = NUM_CUBE_FACES;
		geometry_pass.init(geometry_pass_desc);

		// Render lighting into a cubemap (ERenderPassType::Cubemap)
		RenderPassDesc lighting_pass_desc = LightingPass::make_render_pass_desc(color_format);
		lighting_pass_desc.initial_width = in_desc.cubemap_render_size;
		lighting_pass_desc.initial_height = in_desc.cubemap_render_size;
		lighting_pass_desc.type = ERenderPassType::Cubemap;
		lighting_pass.init(lighting_pass_desc);

		// Render radial depth into a cubemap (ERenderPassType::Cubemap)
		sg_pixel_format radial_depth_format = SG_PIXELFORMAT_RGBA32F;
		RenderPassDesc radial_depth_pass_desc = {
			.initial_width = in_desc.cubemap_render_size,
			.initial_height = in_desc.cubemap_render_size,
			.pipeline_desc = (sg_pipeline_desc) {
				.shader = sg_make_shader(radial_depth_radial_depth_shader_desc(sg_query_backend())),
				.depth = {
					.pixel_format = SG_PIXELFORMAT_NONE,
				},
				.color_count = 1,
				.colors[0] = {
					.pixel_format = radial_depth_format,
				},
				.cull_mode = SG_CULLMODE_NONE,
				.label = "radial-depth-pipeline",
			},
			.num_outputs = 1,
			.outputs[0] = {
				.pixel_format = radial_depth_format,
				.load_action = SG_LOADACTION_CLEAR,
				.store_action = SG_STOREACTION_STORE,
				.clear_value = {1.0, 1.0, 1.0, 1.0},
			},
			.type = ERenderPassType::Cubemap,
		};
		radial_depth_pass.init(radial_depth_pass_desc);

		// Convert lighting information into octahedral irradiance
		RenderPassDesc cube_to_oct_pass_desc = {
			.initial_width = in_desc.octahedral_total_size,
			.initial_height = in_desc.octahedral_total_size,
			.pipeline_desc = (sg_pipeline_desc) {
				.shader = sg_make_shader(cubemap_to_octahedral_cubemap_to_octahedral_shader_desc(sg_query_backend())),
				.depth = {
					.pixel_format = SG_PIXELFORMAT_NONE,
				},
				.color_count = 2,
				.colors[0] = {
					.pixel_format = color_format,
				},
				.colors[1] = {
					.pixel_format = radial_depth_format,
				},
				.cull_mode = SG_CULLMODE_NONE,
				.label = "cubemap-to-octahedral-pipeline",
			},
			.num_outputs = 2,
			.outputs[0] = {
				.pixel_format = color_format,
				.load_action = SG_LOADACTION_LOAD,
				.store_action = SG_STOREACTION_STORE,
			},
			.outputs[1] = {
				.pixel_format = radial_depth_format,
				.load_action = SG_LOADACTION_LOAD,
				.store_action = SG_STOREACTION_STORE,
			},
			.type = ERenderPassType::Single,
		};
		cube_to_oct_pass.init(cube_to_oct_pass_desc);

		radiance_projection_pipeline = sg_make_pipeline((sg_pipeline_desc) {
			.compute = true,
			.shader = sg_make_shader(probe_radiance_projection_probe_radiance_projection_shader_desc(sg_query_backend())),
			.label = "probe-radiance-projection-pipeline",
		});

		is_initialized = true;
	}

	void render(
		State& in_state,
		const HMM_Vec3 in_location,
		const i32 in_atlas_idx,
		const bool should_render_geometry,
		const i32 in_probe_idx,
		const sg_view in_sh9_coefficients_view,
		const sg_view in_sg9_lobes_view)
	{
		assert_msgf(is_initialized, "Cubemap should be initialized with a LightingCaptureDesc passed to its constructor");

		// View + Projection matrix setup
		const f32 fov = HMM_AngleDeg(90.0f);
		const f32 aspect_ratio = 1.0f;
		HMM_Mat4 projection_matrix = mat4_perspective(fov, aspect_ratio);
		projection_matrix[1][1] *= -1.0f;


		HMM_Mat4 view_matrices[NUM_CUBE_FACES];
		HMM_Mat4 view_projection_matrices[NUM_CUBE_FACES];
		for (i32 face_idx = 0; face_idx < NUM_CUBE_FACES; ++face_idx)
		{
			HMM_Vec3 forward = Render::CUBE_FORWARD_AND_UP[face_idx][0];
			HMM_Vec3 up = Render::CUBE_FORWARD_AND_UP[face_idx][1];

			HMM_Vec3 target = in_location + forward * 10;
			view_matrices[face_idx] = HMM_LookAt_RH(in_location, target, up);
			view_projection_matrices[face_idx] = HMM_MulM4(projection_matrix, view_matrices[face_idx]);
		}

		geometry_pass.execute(
			[&](const i32 face_idx)
	   		{
				HMM_Mat4& view_matrix = view_matrices[face_idx];
				HMM_Mat4& view_projection_matrix = view_projection_matrices[face_idx];

				geometry_vs_params_t vs_params;
				vs_params.view = view_matrix;
				vs_params.projection = projection_matrix;

				// Apply Vertex Uniforms
				sg_apply_uniforms(0, SG_RANGE(vs_params));

				// Cull objects
				const f32 cull_bounds_padding = in_state.tessellation.enabled ? in_state.tessellation.bounds_padding : 0.0f;
				CullResult cull_result = cull_objects(in_state.scene.objects, view_projection_matrix, cull_bounds_padding);

				// Submit draw calls for objects after culling
				if (should_render_geometry)
				{
					for (auto& [unique_id, object_ptr] : cull_result.objects)
					{
						assert(object_ptr);
						Object& object = *object_ptr;

						if (object.has_mesh)
						{
							Mesh& mesh = object.mesh;
							MeshRenderView render_view = mesh_get_render_view(mesh);

							int mesh_material_idx = mesh.material_indices[0];
							assert(mesh_material_idx >= 0);
							const geometry_Material_t& material = in_state.materials.items[mesh_material_idx];

							GpuImage& base_color_image = material.base_color_image_index >= 0 ? in_state.images.items[material.base_color_image_index] : in_state.gpu.default_image;
							GpuImage& metallic_image = material.metallic_image_index >= 0 ? in_state.images.items[material.metallic_image_index] : in_state.gpu.default_image;
							GpuImage& roughness_image = material.roughness_image_index >= 0 ? in_state.images.items[material.roughness_image_index] : in_state.gpu.default_image;
							GpuImage& emission_color_image = material.emission_color_image_index >= 0 ? in_state.images.items[material.emission_color_image_index] : in_state.gpu.default_image;

							sg_bindings bindings = {
								.vertex_buffers[0] = render_view.vertex_buffer,
								.index_buffer = render_view.index_buffer,
								.views = {
									[0] = object.storage_buffer.get_storage_view(),
									[1] = get_materials_buffer().get_storage_view(),
									[2] = base_color_image.get_texture_view(0),
									[3] = metallic_image.get_texture_view(0),
									[4] = roughness_image.get_texture_view(0),
									[5] = emission_color_image.get_texture_view(0),
								},
								.samplers[0] = in_state.gpu.linear_sampler,
							};
							sg_apply_bindings(&bindings);
							sg_draw(0, render_view.index_count, 1);
						}
					}
				}

				if (in_state.gi.render_sky_to_probes)
				{
					SkyPass::render(view_projection_matrix, in_location, depth_format);
				}
			}
		);

		lighting_pass.execute(
			[&](const i32 face_idx)
	   		{
				lighting_fs_params_t fs_params = in_state.lighting.fs_params;
				fs_params.view_position = in_location;
				fs_params.view_forward = Render::CUBE_FORWARD_AND_UP[face_idx][0];
				fs_params.ssao_enable = false;
				fs_params.direct_lighting_enable = true;
				fs_params.gi_enable = false;
				fs_params.shadow_map_enable = false;
				sg_apply_uniforms(0, SG_RANGE(fs_params));

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
						[4] = in_state.gpu.default_image.get_texture_view(0),		//FCS TODO: Need SSAO
						[5] = in_state.lighting.point_lights_buffer.get_storage_view(),
						[6] = in_state.lighting.spot_lights_buffer.get_storage_view(),
						[7] = in_state.lighting.sun_lights_buffer.get_storage_view(),
						[8] = in_state.gpu.default_buffer.get_storage_view(),
						[9] = in_state.gpu.default_buffer.get_storage_view(),
						[10] = in_state.gpu.default_image.get_texture_view(0),
						[11] = in_state.gpu.default_image.get_texture_view(0),
						[12] = in_state.gpu.default_image_array.get_texture_array_view(),
						[13] = in_state.gpu.default_buffer.get_storage_view(),
						[14] = in_state.gpu.default_buffer.get_storage_view(),
					},
					.samplers = {
						[0] = in_state.gpu.linear_sampler,
						[1] = in_state.gpu.nearest_sampler,
					},
				};
				sg_apply_bindings(&bindings);

				sg_draw(0,6,1);
			}
		);

		radial_depth_pass.execute(
			[&](const i32 face_idx)
	   		{
				const HMM_Mat4& view_projection_matrix = view_projection_matrices[face_idx];

				radial_depth_fs_params_t fs_params = {
					.inverse_view_projection = HMM_InvGeneralM4(view_projection_matrix),
					.capture_location = in_location,
					.probe_occlusion_mode = static_cast<i32>(in_state.gi.probe_occlusion_mode),
					.force_fully_visible = in_state.gi.debug_constant_white_probes ? 1 : 0,
				};
				sg_apply_uniforms(0, SG_RANGE(fs_params));

				//GpuImage& depth_texture = geometry_pass.get_depth_output(face_idx);
				GpuImage& position_texture = geometry_pass.get_color_output(1, face_idx);
				sg_bindings bindings = {
					.views = {
						[0] = position_texture.get_texture_view(0),
					},
					.samplers[0] = in_state.gpu.nearest_sampler,
				};
				sg_apply_bindings(&bindings);

				sg_draw(0,6,1);
			}
		);

		cube_to_oct_pass.execute(
			[&](const i32 _)
	   		{
				// Determine current atlas location to render into
				{
					AtlasViewport viewport = get_atlas_viewport(
						desc.octahedral_total_size,
						desc.octahedral_entry_size,
						in_atlas_idx
					);

					sg_apply_viewport(
						viewport.x,
						viewport.y,
						viewport.w,
						viewport.h,
						true
					);
				}

				cubemap_to_octahedral_fs_params_t fs_params = {
					.cubemap_render_size = desc.cubemap_render_size,
					.atlas_entry_size = desc.octahedral_entry_size,
					.compute_irradiance = in_state.gi.compute_irradiance,
					.use_importance_sampling = true,
				};
				sg_apply_uniforms(0, SG_RANGE(fs_params));

				GpuImage& lighting_cubemap_texture = in_state.gi.debug_constant_white_probes
					? in_state.gpu.white_image_cube
					: lighting_pass.get_color_output(0);
				GpuImage& depth_cubemap_texture = radial_depth_pass.get_color_output(0);

					sg_bindings bindings = {
						.views = {
							[0] = lighting_cubemap_texture.get_texture_view(0),
							[1] = depth_cubemap_texture.get_texture_view(0),
						},
						.samplers = {
							[0] = in_state.gpu.linear_sampler,
							[1] = in_state.gpu.nearest_sampler,
						},
					};
					sg_apply_bindings(&bindings);

				sg_draw(0,6,1);
			}
		);

		const bool should_project_sh9 =
			in_state.gi.probe_radiance_mode == EProbeRadianceMode::SH9 ||
			in_state.gi.probe_vis_mode == EProbeVisMode::SH9Irradiance;
		const bool should_project_sg9 =
			in_state.gi.probe_radiance_mode == EProbeRadianceMode::SG9 ||
			in_state.gi.probe_vis_mode == EProbeVisMode::SG9Irradiance;

		auto project_probe_radiance = [&](const EProbeRadianceMode radiance_mode)
		{
			probe_radiance_projection_cs_params_t cs_params = {
				.probe_index = in_probe_idx,
				.radiance_mode = static_cast<i32>(radiance_mode),
				.sample_count = 1024,
			};

			GpuImage& lighting_cubemap_texture = in_state.gi.debug_constant_white_probes
				? in_state.gpu.white_image_cube
				: lighting_pass.get_color_output(0);

			sg_begin_pass((sg_pass) {
				.compute = true,
			});
			sg_apply_pipeline(radiance_projection_pipeline);
			sg_apply_uniforms(0, SG_RANGE(cs_params));
			sg_bindings bindings = {
				.views = {
					[0] = in_sh9_coefficients_view,
					[1] = in_sg9_lobes_view,
					[2] = lighting_cubemap_texture.get_texture_view(0),
				},
				.samplers[0] = in_state.gpu.linear_sampler,
			};
			sg_apply_bindings(&bindings);
			sg_dispatch(1, 1, 1);
			sg_end_pass();
		};

		if (should_project_sh9)
		{
			project_probe_radiance(EProbeRadianceMode::SH9);
		}

		if (should_project_sg9)
		{
			project_probe_radiance(EProbeRadianceMode::SG9);
		}
	}
};
