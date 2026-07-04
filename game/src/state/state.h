#pragma once

#include <atomic>
#include <optional>

#include "ankerl/unordered_dense.h"
#include "core/stretchy_buffer.h"
#include "game_object/game_object.h"
#include "network/channel.h"
#include "network/socket_wrapper.h"
#include "render/gpu_image.h"
#include "render/render_pass.h"

#include "sokol/sokol_gfx.h"
#include "render/shader_files.h"

using ankerl::unordered_dense::map;
using std::atomic;
using std::optional;

enum class ERenderPass : int
{
	ShadowDepth,
	ShadowBlur,
	ShadowCascadeDebug,
	Geometry,
	SSAO,
	SSAO_Blur,
	ScreenSpaceShadows,
	Lighting,
	DOF_Combine,
	WireOverlay,
	TemporalAA,
	Tonemapping,
	FXAA,
	DebugText,
	CopyToSwapchain,
	COUNT,
};


enum class EProbeVisMode : i32
{
	Irradiance = 0,
	SH9Irradiance = 1,
	SG9Irradiance = 2,
	RadialDepth = 3,
	RadialDepthSquared = 4,
	EVRPPositiveMoment = 5,
	MAX,
};

const char* EProbeVisModeNames[(i32)EProbeVisMode::MAX] = {
	"Irradiance",
	"SH9 Irradiance",
	"SG9 Irradiance",
	"RadialDepth",
	"RadialDepthSquared",
	"EVRP Positive Moment",
};

enum class EProbeOcclusionMode : i32
{
	Chebyshev = 0,
	EVRP4 = 1,
	MAX,
};

const char* EProbeOcclusionModeNames[(i32)EProbeOcclusionMode::MAX] = {
	"Chebyshev",
	"EVRP4",
};

enum class EProbeRadianceMode : i32
{
	Octahedral = 0,
	SH9 = 1,
	SG9 = 2,
	MAX,
};

const char* EProbeRadianceModeNames[(i32)EProbeRadianceMode::MAX] = {
	"Octahedral",
	"SH9",
	"SG9",
};

static constexpr i32 MAX_SHADOW_CASCADES = 4;
static constexpr i32 DEFAULT_TARGET_RENDER_WIDTH = 1920;
static constexpr i32 DEFAULT_TARGET_RENDER_HEIGHT = 1080;
static constexpr f32 DEFAULT_TARGET_RENDER_ASPECT_RATIO = (f32)DEFAULT_TARGET_RENDER_WIDTH / (f32)DEFAULT_TARGET_RENDER_HEIGHT;

enum class EShadowCascadePlacementMode : i32
{
	Frustum = 0,
	CenteredSquares = 1,
	MAX,
};

const char* EShadowCascadePlacementModeNames[(i32)EShadowCascadePlacementMode::MAX] = {
	"Frustum",
	"Centered Squares",
};

enum class ETessellationMode : i32
{
	Fixed = 0,
	AdaptiveAngularPerMesh = 1,
	AdaptiveAngularPerTriangle = 2,
	MAX,
};

const char* ETessellationModeNames[(i32)ETessellationMode::MAX] = {
	"Fixed",
	"Adaptive Angular (Per Mesh)",
	"Adaptive Angular (Per Triangle)",
};

static constexpr i32 RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT = 3;
static constexpr i32 RENDER_OBJECT_SNAPSHOT_INITIAL_CAPACITY = 64;

struct State
{
	struct RuntimeState
	{
		optional<std::string> init_file;
		atomic<bool> game_running = true;
		atomic<bool> blender_data_loaded = false;
		bool is_simulating = true;
	} runtime;

	struct AnimationState
	{
		bool is_playing = true;
		f32 playback_rate = 1.0f;
		bool skinning_debug_view = false;
	} animation;

	struct WindowState
	{
		int width = 0;
		int height = 0;
		int target_render_width = DEFAULT_TARGET_RENDER_WIDTH;
		int target_render_height = DEFAULT_TARGET_RENDER_HEIGHT;
		bool maintain_target_aspect_ratio = true;
		f32 target_render_aspect_ratio = DEFAULT_TARGET_RENDER_ASPECT_RATIO;
		int render_width = DEFAULT_TARGET_RENDER_WIDTH;
		int render_height = DEFAULT_TARGET_RENDER_HEIGHT;
	} window;

	struct RenderPassState
	{
		RenderPassEntry passes[(int)ERenderPass::COUNT];
	} render_passes;

	struct RenderObjectSnapshotState
	{
		StretchyBuffer<geometry_ObjectData_t> items;
		GpuBuffer<geometry_ObjectData_t> buffers[RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT];
		i32 buffer_index = 0;
		i32 buffer_capacity = 0;
		bool valid = false;
	} render_objects;

	struct SceneState
	{
		map<i32, Object> objects;
		optional<i32> player_character_id;
		optional<i32> camera_control_id;
		optional<i32> primary_sun_id;

		struct IndexState
		{
			bool dirty = true;
			StretchyBuffer<i32> mesh_object_ids;
			StretchyBuffer<i32> light_object_ids;
			StretchyBuffer<i32> armature_object_ids;
			StretchyBuffer<i32> skinned_mesh_object_ids;
		} indexes;
	} scene;

	struct LiveLinkState
	{
		Channel<Object> updated_objects;
		Channel<i32> deleted_objects;
		Channel<bool> reset;
		std::thread thread;
		SOCKET blender_socket = socket_invalid();
		SOCKET connection_socket = socket_invalid();
	} live_link;

	struct GpuResourceState
	{
		sg_sampler linear_sampler;
		sg_sampler nearest_sampler;
		GpuImage default_image;
		GpuImage default_image_array;
		GpuImage white_image_cube;
		GpuBuffer<u8> default_buffer;
	} gpu;

	struct MaterialState
	{
		map<i32,i32> id_to_index;
		StretchyBuffer<geometry_Material_t> items;
		GpuBuffer<geometry_Material_t> buffer;
	} materials;

	struct ImageState
	{
		map<i32,i32> id_to_index;
		StretchyBuffer<GpuImage> items;
		bool enable_debug_fullscreen = false;
		i32 debug_index = 0;
	} images;

	struct LightingState
	{
		bool direct_enable = true;
		lighting_fs_params_t fs_params;
		bool needs_data_update = true;
		StretchyBuffer<lighting_PointLight_t> point_lights;
		GpuBuffer<lighting_PointLight_t> point_lights_buffer;
		StretchyBuffer<lighting_SpotLight_t> spot_lights;
		GpuBuffer<lighting_SpotLight_t> spot_lights_buffer;
		StretchyBuffer<lighting_SunLight_t> sun_lights;
		GpuBuffer<lighting_SunLight_t> sun_lights_buffer;
	} lighting;

	struct SsaoState
	{
		bool enable = true;
		GpuImage noise_texture;
		ssao_fs_params_t fs_params;
	} ssao;

	struct ShadowState
	{
		bool rendering_enable = true;
		bool blur_enable = true;
		bool depth_freeze = false;
		i32 num_cascades = 3;
		f32 frustum_cascade_distance_scale = 1.0f;
		f32 centered_square_cascade_distance_scale = 0.25f;
		EShadowCascadePlacementMode cascade_placement_mode = EShadowCascadePlacementMode::Frustum;
		HMM_Vec3 centered_square_center = HMM_V3(0.0f, 0.0f, 0.0f);
		f32 centered_square_lookahead_distance = 50.0f;
		bool force_recapture = false;
		i32 debug_cascade_index = 0;
		i32 debug_view_mode = 0;
		bool debug_show_cascade_selection = false;
		struct ScreenSpaceShadowState
		{
			bool enable = true;
			bool debug_show_mask = false;
			f32 ray_length = 1.0f;
			f32 thickness = 0.08f;
			f32 jitter_strength = 1.0f;
			i32 max_steps = 24;
			f32 intensity = 1.0f;
			i32 filter_radius = 2;
		} screen_space;
	} shadow;

	struct TessellationState
	{
		bool enabled = false;
		ETessellationMode mode = ETessellationMode::AdaptiveAngularPerTriangle;
		i32 fixed_factor = 4;
		i32 max_factor = 24;
		f32 target_pixels_per_segment = 20.0f;
		f32 phong_strength = 0.0f; //TODO: per-object control of this, or even per triangle.
		bool virtual_patches_enabled = true;
		i32 virtual_patch_max_depth = 2;
		i32 max_generated_patches = 256 * 1024;
		i32 max_generated_vertices = 4 * 1024 * 1024;
		i32 max_generated_indices = 12 * 1024 * 1024;
		f32 bounds_padding = 0.0f;

		i32 source_triangle_count = 0;
		i32 patch_count = 0;
		i32 generated_vertex_count = 0;
		i32 generated_index_count = 0;
		i32 mesh_count = 0;
		i32 overflowed_mesh_count = 0;
		i32 max_factor_seen = 1;
		bool readback_supported = false;
		i32 readback_age = 0;
	} tessellation;

	struct WireframeState
	{
		bool shaded_wireframe = false;
		f32 width = 0.5f;
		f32 softness = 1.0f;
		f32 opacity = 0.75f;
		HMM_Vec4 color = HMM_V4(0.01f, 0.01f, 0.01f, 1.0f);
		f32 visibility_tolerance = 0.02f;
	} wireframe;

	struct SkyState
	{
		bool rendering_enable = true;
	} sky;

	struct GiState
	{
		bool enable = true;
		bool probe_occlusion = true;
		i32 octree_depth = 4;
		bool layout_dirty = true;
		EProbeOcclusionMode probe_occlusion_mode = EProbeOcclusionMode::Chebyshev;
		EProbeRadianceMode probe_radiance_mode = EProbeRadianceMode::Octahedral;
		bool render_sky_to_probes = true;
		bool debug_constant_white_probes = false;
		float intensity = 1.0f;
		bool show_probes = false;
		EProbeVisMode probe_vis_mode = EProbeVisMode::Irradiance;
		bool probe_isolation_enable = false;
		i32 isolated_probe_index = -1;
		bool compute_irradiance = true;
		bool is_updating = true;
	} gi;

	struct DofState
	{
		bool enable = true;
		f32 focus_distance = 30.0f;
		f32 focus_range = 120.0f;
		f32 max_coc_radius = 8.0f;
		f32 foreground_blur_scale = 0.5f;
		f32 background_blur_scale = 1.0f;
		bool debug_show_coc = false;
	} dof;

	struct TemporalAAState
	{
		bool enable = true;
		bool enable_fxaa = true;
		bool history_valid = false;
		i32 history_index = 0;
		i32 jitter_phase = 0;
		f32 blend_alpha = 0.5f;
		f32 sharpen_strength = 0.08f;
		f32 rejection_threshold = 0.25f;
		i32 debug_mode = 0;
		HMM_Vec2 current_jitter_pixels = HMM_V2(0.0f, 0.0f);
		HMM_Mat4 previous_view_projection = {};
	} temporal_aa;

	struct TonemappingState
	{
		tonemapping_fs_params_t fs_params = {
			.exposure_bias = 1.5,
		};
	} tonemapping;

	struct DebugUiState
	{
		double stats_sample_elapsed = 0.0;
		i32 stats_sample_count = 0;
		f32 frame_time_ms = 0.0f;
		f32 fps = 0.0f;
		bool show_profiler = false;
		bool freeze_profiler = false;
		bool show_profiler_unaccounted = false;
		f32 profiler_zoom = 1.0f;
		f32 profiler_scroll_x = 0.0f;
		i32 num_profiler_frames = 3;
	} debug_ui;

	struct DataOrientedState
	{
		struct LiveLinkImportStats
		{
			u64 update_index = 0;
			u64 byte_count = 0;
			f64 generation_seconds = 0.0;
			i32 object_count = 0;
			i32 deleted_object_count = 0;
			i32 mesh_count = 0;
			i32 mesh_vertex_count = 0;
			i32 mesh_index_count = 0;
			i32 skinned_mesh_count = 0;
			i32 light_count = 0;
			i32 armature_count = 0;
			i32 animation_count = 0;
			i32 animation_matrix_count = 0;
			i32 material_count = 0;
			i32 image_count = 0;
			u64 image_byte_count = 0;
			i32 malformed_object_count = 0;
			bool reset = false;
		};

		struct FrameAccessStats
		{
			i32 scene_object_count = 0;
			i32 mesh_object_count = 0;
			i32 light_object_count = 0;
			i32 armature_object_count = 0;
			i32 skinned_mesh_object_count = 0;

			i32 live_link_updated_objects = 0;
			i32 live_link_deleted_objects = 0;
			i32 live_link_reset_count = 0;

			i32 animation_armature_candidates = 0;
			i32 animation_armatures_updated = 0;
			i32 animation_skinned_mesh_candidates = 0;
			i32 animation_skin_matrix_uploads = 0;

			i32 lighting_candidate_count = 0;
			i32 lighting_processed_count = 0;

			i32 object_update_scan_count = 0;
			i32 object_update_storage_updates = 0;
			i32 object_update_mesh_dirty_count = 0;

			i32 cull_calls = 0;
			i32 cull_candidate_count = 0;
			i32 cull_visible_count = 0;
			i32 cull_non_renderable_count = 0;
			i32 cull_visibility_count = 0;
			i32 cull_frustum_count = 0;
			i32 cull_skinned_visible_count = 0;

			i32 draw_calls = 0;
			i32 draw_mesh_count = 0;

			i32 gpu_skinning_candidate_count = 0;
			i32 gpu_skinning_updated_count = 0;

			i32 tessellation_candidate_count = 0;
			i32 tessellation_processed_count = 0;
		};

		u64 frame_index = 0;
		LiveLinkImportStats last_import;
		StretchyBuffer<LiveLinkImportStats> import_history;
		i32 selected_import_history_index = -1;
		FrameAccessStats frame;
		FrameAccessStats previous_frame;
	} data_oriented;

	struct DebugCameraState
	{
		bool active = true;
		Camera camera = {
			.location = HMM_V3(2.5f, -15.0f, 3.0f),
			.forward = HMM_NormV3(HMM_V3(0.0f, 1.0f, -0.5f)),
			.up = HMM_NormV3(HMM_V3(0.0f, 0.0f, 1.0f)),
		};
	} debug_camera;
} state;

void data_oriented_begin_frame(State& in_state)
{
	in_state.data_oriented.previous_frame = in_state.data_oriented.frame;
	in_state.data_oriented.frame = {};
	++in_state.data_oriented.frame_index;
}

void scene_record_index_counts(State& in_state)
{
	in_state.data_oriented.frame.scene_object_count = (i32)in_state.scene.objects.size();
	in_state.data_oriented.frame.mesh_object_count = (i32)in_state.scene.indexes.mesh_object_ids.length();
	in_state.data_oriented.frame.light_object_count = (i32)in_state.scene.indexes.light_object_ids.length();
	in_state.data_oriented.frame.armature_object_count = (i32)in_state.scene.indexes.armature_object_ids.length();
	in_state.data_oriented.frame.skinned_mesh_object_count = (i32)in_state.scene.indexes.skinned_mesh_object_ids.length();
}

void scene_mark_indexes_dirty(State& in_state)
{
	in_state.scene.indexes.dirty = true;
}

void scene_reset_indexes(State& in_state)
{
	in_state.scene.indexes.mesh_object_ids.reset();
	in_state.scene.indexes.light_object_ids.reset();
	in_state.scene.indexes.armature_object_ids.reset();
	in_state.scene.indexes.skinned_mesh_object_ids.reset();
	in_state.scene.indexes.dirty = true;
	scene_record_index_counts(in_state);
}

void scene_rebuild_indexes(State& in_state)
{
	in_state.scene.indexes.mesh_object_ids.reset();
	in_state.scene.indexes.light_object_ids.reset();
	in_state.scene.indexes.armature_object_ids.reset();
	in_state.scene.indexes.skinned_mesh_object_ids.reset();

	for (auto const& [unique_id, object] : in_state.scene.objects)
	{
		if (object.has_mesh)
		{
			in_state.scene.indexes.mesh_object_ids.add(unique_id);
			if (object.mesh.has_skinned_vertices)
			{
				in_state.scene.indexes.skinned_mesh_object_ids.add(unique_id);
			}
		}

		if (object.has_light)
		{
			in_state.scene.indexes.light_object_ids.add(unique_id);
		}

		if (object.has_armature)
		{
			in_state.scene.indexes.armature_object_ids.add(unique_id);
		}
	}

	in_state.scene.indexes.dirty = false;
	scene_record_index_counts(in_state);
}

void scene_ensure_indexes(State& in_state)
{
	if (in_state.scene.indexes.dirty)
	{
		scene_rebuild_indexes(in_state);
		return;
	}

	scene_record_index_counts(in_state);
}

void render_object_snapshot_ensure_capacity(State& in_state, i32 required_capacity)
{
	required_capacity = MAX(required_capacity, 1);
	if (in_state.render_objects.buffer_capacity >= required_capacity)
	{
		return;
	}

	i32 new_capacity = MAX(RENDER_OBJECT_SNAPSHOT_INITIAL_CAPACITY, in_state.render_objects.buffer_capacity);
	while (new_capacity < required_capacity)
	{
		new_capacity *= 2;
	}

	for (i32 buffer_idx = 0; buffer_idx < RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT; ++buffer_idx)
	{
		in_state.render_objects.buffers[buffer_idx].destroy_gpu_buffer();
		in_state.render_objects.buffers[buffer_idx] = GpuBuffer((GpuBufferDesc<geometry_ObjectData_t>){
			.data = nullptr,
			.size = sizeof(geometry_ObjectData_t) * new_capacity,
			.usage = {
				.storage_buffer = true,
				.stream_update = true,
			},
			.label = "render_object_snapshot",
		});
	}

	in_state.render_objects.buffer_capacity = new_capacity;
	in_state.render_objects.valid = false;
}

void build_render_object_snapshot(State& in_state)
{
	scene_ensure_indexes(in_state);

	for (auto& [unique_id, object] : in_state.scene.objects)
	{
		object.render_object_index = -1;
	}

	in_state.render_objects.items.clear();
	render_object_snapshot_ensure_capacity(in_state, (i32)in_state.scene.indexes.mesh_object_ids.length());

	for (const i32 unique_id : in_state.scene.indexes.mesh_object_ids)
	{
		if (!in_state.scene.objects.contains(unique_id))
		{
			continue;
		}

		Object& object = in_state.scene.objects[unique_id];
		if (!object.has_mesh)
		{
			continue;
		}

		object.render_object_index = (i32)in_state.render_objects.items.length();
		in_state.render_objects.items.add(object_make_render_data(object));
	}

	if (in_state.render_objects.items.length() <= 0)
	{
		in_state.render_objects.valid = false;
		return;
	}

	in_state.render_objects.buffer_index =
		(in_state.render_objects.buffer_index + 1) % RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT;

	GpuBuffer<geometry_ObjectData_t>& active_buffer =
		in_state.render_objects.buffers[in_state.render_objects.buffer_index];

	active_buffer.update_gpu_buffer(
		(sg_range){
			.ptr = in_state.render_objects.items.data(),
			.size = sizeof(geometry_ObjectData_t) * in_state.render_objects.items.length(),
		}
	);
	in_state.render_objects.valid = true;
}

GpuBuffer<geometry_ObjectData_t>& get_render_object_snapshot_buffer(State& in_state)
{
	assert(in_state.render_objects.valid);
	return in_state.render_objects.buffers[in_state.render_objects.buffer_index];
}

void state_init()
{
	// Create Default Image
	HMM_Vec4 default_image_data[1] = { HMM_V4(0,0,0,1) };
	GpuImageDesc default_image_desc = {
		.type = SG_IMAGETYPE_2D,
		.width = 1,
		.height = 1,
		.pixel_format = SG_PIXELFORMAT_RGBA32F,
		.data = (u8*) &default_image_data,
	};
	state.gpu.default_image = GpuImage(default_image_desc);

	HMM_Vec4 default_image_array_data[MAX_SHADOW_CASCADES] = {};
	for (i32 i = 0; i < MAX_SHADOW_CASCADES; ++i)
	{
		default_image_array_data[i] = HMM_V4(0,0,0,1);
	}
	GpuImageDesc default_image_array_desc = {
		.type = SG_IMAGETYPE_ARRAY,
		.width = 1,
		.height = 1,
		.num_slices = MAX_SHADOW_CASCADES,
		.pixel_format = SG_PIXELFORMAT_RGBA32F,
		.data = (u8*) &default_image_array_data,
	};
	state.gpu.default_image_array = GpuImage(default_image_array_desc);

	HMM_Vec4 white_image_cube_data[6] =
	{
		HMM_V4(1,1,1,1),
		HMM_V4(1,1,1,1),
		HMM_V4(1,1,1,1),
		HMM_V4(1,1,1,1),
		HMM_V4(1,1,1,1),
		HMM_V4(1,1,1,1),
	};
	GpuImageDesc white_image_cube_desc = {
		.type = SG_IMAGETYPE_CUBE,
		.width = 1,
		.height = 1,
		.num_slices = 6,
		.pixel_format = SG_PIXELFORMAT_RGBA32F,
		.data = (u8*) &white_image_cube_data,
	};
	state.gpu.white_image_cube = GpuImage(white_image_cube_desc);

	u8 default_buffer_data[32] = {};
	GpuBufferDesc<u8> default_buffer_desc = {
		.data = default_buffer_data,
		.size = sizeof(default_buffer_data),
		.usage = {
			.storage_buffer = true,
		},
		.label = "default_buffer",
	};
	state.gpu.default_buffer = GpuBuffer<u8>(default_buffer_desc);

	state.gpu.linear_sampler = sg_make_sampler((sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });

	state.gpu.nearest_sampler = sg_make_sampler((sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST,
        .mag_filter = SG_FILTER_NEAREST,
		.mipmap_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });

	state.temporal_aa.previous_view_projection = HMM_M4D(1.0f);
}

//FCS TODO: member methods on state once it's not a global
const i32 MAX_MATERIALS = 1024;
void init_materials_buffer()
{
	if (!state.materials.buffer.is_gpu_buffer_valid())
	{
		state.materials.buffer = GpuBuffer((GpuBufferDesc<geometry_Material_t>){
			.data = nullptr,
			.size = sizeof(geometry_Material_t) * MAX_MATERIALS,
			.usage = {
				.storage_buffer = true,
				.stream_update = true,
			},
			.label = "materials",
		});
	}
}

GpuBuffer<geometry_Material_t>& get_materials_buffer()
{
	init_materials_buffer();
	return state.materials.buffer;
}

void update_materials_buffer()
{
	// Write update to materials buffer
	get_materials_buffer().update_gpu_buffer(
		(sg_range){
			.ptr = state.materials.items.data(),
			.size = sizeof(geometry_Material_t) * state.materials.items.length(),
		}
	);
}

Camera& get_active_camera()
{
	if (state.scene.camera_control_id && !state.debug_camera.active)
	{
		Object& camera_control_target = state.scene.objects[*state.scene.camera_control_id];
		assert(camera_control_target.has_camera_control);
		return camera_control_target.camera_control.camera;
	}

	return state.debug_camera.camera;
}
