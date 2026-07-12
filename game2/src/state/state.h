#pragma once

#include <optional>
#include <string>
#include <thread>

#include "ankerl/unordered_dense.h"
#include "core/types.h"
#include "network/channel.h"
#include "network/socket_wrapper.h"
#include "game_object/camera.h"
#include "game_object/game_object.h"
#include "render/vulkan_context.h"
#include "render/gpu_buffer.h"
#include "render/render_pass.h"

// ObjectData (shared with shaders)
#include "shader_common.h"

static constexpr i32 RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT = 3;
static constexpr i32 RENDER_OBJECT_SNAPSHOT_INITIAL_CAPACITY = 64;
static constexpr i32 MAX_LIGHTS_PER_TYPE = 1024;

static constexpr i32 MIN_RENDER_RESOLUTION_PERCENTAGE = 5;
static constexpr i32 MAX_RENDER_RESOLUTION_PERCENTAGE = 100;
static constexpr i32 DEFAULT_RENDER_RESOLUTION_PERCENTAGE = 50;

// Frame-order pass registry (game/'s lives in state.h too; game/'s full list
// slots back in here as passes are ported in Phase 3)
static constexpr i32 MAX_SHADOW_CASCADES = 4;

enum class EShadowCascadePlacementMode : i32
{
	Frustum = 0,
	CenteredSquares = 1,
	MAX,
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

enum class EProbeOcclusionMode : i32
{
	Chebyshev = 0,
	EVRP4 = 1,
	MAX,
};

enum class EProbeRadianceMode : i32
{
	Octahedral = 0,
	SH9 = 1,
	SG9 = 2,
	MAX,
};

enum class ETessellationMode : i32
{
	Fixed = 0,
	AdaptiveAngularPerMesh = 1,
	AdaptiveAngularPerTriangle = 2,
	MAX,
};

inline const char* ETessellationModeNames[(i32) ETessellationMode::MAX] = {
	"Fixed",
	"Adaptive Angular (Per Mesh)",
	"Adaptive Angular (Per Triangle)",
};

enum class ERenderPass : i32
{
	ShadowDepth,
	ShadowBlur,
	ShadowCascadeDebug,
	Geometry,
	SSAO,
	SSAO_Blur,
	ScreenSpaceShadows,
	Lighting,
	Fog,
	DofCombine,
	WireOverlay,
	TemporalAA,
	Tonemapping,
	FXAA,
	CopyToSwapchain,

	COUNT,
};

// ---- Live link messages ----
// game/ registers images/materials on the live-link thread; game2 must not
// (GPU image creation submits to the graphics queue, and the id->index maps
// are read by the main thread). Instead the parse thread packages one
// SceneUpdate per flatbuffer Update and ALL registration happens at drain
// on the main thread, preserving game/'s ordering:
// images -> materials -> objects -> deleted -> reset.

struct PendingImage
{
	i32 unique_id = 0;
	i32 width = 0;
	i32 height = 0;
	u8* pixels = nullptr;	// malloc'd RGBA8, width*height*4, validated at parse
};

struct PendingMaterial
{
	i32 unique_id = 0;
	HMM_Vec4 base_color = HMM_V4(1.0f, 1.0f, 1.0f, 1.0f);
	HMM_Vec4 emission_color = HMM_V4(0.0f, 0.0f, 0.0f, 1.0f);
	f32 metallic = 0.0f;
	f32 roughness = 0.5f;
	f32 emission_strength = 0.0f;

	// Raw flatbuffer image ids (0 = none); resolved to indices at drain
	i32 base_color_image_id = 0;
	i32 emission_color_image_id = 0;
	i32 metallic_image_id = 0;
	i32 roughness_image_id = 0;
};

struct SceneUpdate
{
	struct ImportStats
	{
		u64 byte_count = 0;
		f64 generation_seconds = 0.0;
		i32 object_count = 0;
		i32 deleted_object_count = 0;
		i32 material_count = 0;
		i32 image_count = 0;
		u64 image_byte_count = 0;
		i32 mesh_count = 0;
		i32 mesh_vertex_count = 0;
		i32 mesh_index_count = 0;
		i32 skinned_mesh_count = 0;
		i32 light_count = 0;
		i32 armature_count = 0;
		i32 animation_count = 0;
		i32 animation_matrix_count = 0;
		i32 malformed_object_count = 0;
		bool reset = false;
	} stats;
	StretchyBuffer<PendingImage> images;
	StretchyBuffer<PendingMaterial> materials;

	// Note: each Object's mesh.material_indices still holds raw material IDS
	// here; resolve_mesh_material_indices converts them at drain
	StretchyBuffer<Object> objects;

	StretchyBuffer<i32> deleted_object_uids;
	bool reset = false;
};

// Global game state, in the shape of game/'s State struct
struct State
{
	struct RuntimeState
	{
		bool game_running = true;
		bool blender_data_loaded = false;
		bool is_simulating = true;
		std::optional<std::string> init_file;
	} runtime;

	struct DebugUiState
	{
		bool visible = true;
		f64 stats_sample_elapsed = 0.0;
		i32 stats_sample_count = 0;
		f64 cpu_time_sample_sum_ms = 0.0;
		i32 cpu_time_sample_count = 0;
		f64 gpu_time_sample_sum_ms = 0.0;
		i32 gpu_time_sample_count = 0;
		f32 immediate_frame_time_ms = 0.0f;
		f32 immediate_fps = 0.0f;
		f32 immediate_cpu_time_ms = 0.0f;
		bool immediate_cpu_time_valid = false;
		f32 immediate_gpu_time_ms = 0.0f;
		bool immediate_gpu_time_valid = false;
		bool immediate_gpu_time_pending = false;
		bool show_profiler = false;
		bool freeze_profiler = false;
		bool show_profiler_unaccounted = false;
		f32 profiler_zoom = 1.0f;
		f32 profiler_scroll_x = 0.0f;
		i32 num_profiler_frames = 3;
		bool show_texture_viewer = false;
		f32 frame_time_ms = 0.0f;
		f32 fps = 0.0f;
		f32 cpu_time_ms = 0.0f;
		bool cpu_time_valid = false;
		f32 gpu_time_ms = 0.0f;
		bool gpu_time_valid = false;
		bool gpu_time_pending = false;
		bool show_immediate_timings = false;
	} debug_ui;

	struct AnimationState
	{
		bool is_playing = true;
		f32 playback_rate = 1.0f;
		bool skinning_debug_view = false;
	} animation;

	struct WindowState
	{
		GLFWwindow* handle = nullptr;

		// Framebuffer pixels (authoritative source: the swapchain extent).
		// Initial values are the window-creation size in screen coordinates;
		// handle_resize overwrites them with pixels after Vulkan init.
		i32 width = 1920;
		i32 height = 1080;

		// Internal render size = window size * resolution_percentage / 100
		i32 resolution_percentage = DEFAULT_RENDER_RESOLUTION_PERCENTAGE;
		i32 render_width = 1920;
		i32 render_height = 1080;
	} window;

	struct RenderPassState
	{
		RenderPassEntry passes[(i32) ERenderPass::COUNT];
	} render_passes;

	struct InputState
	{
		bool keycodes[GLFW_KEY_LAST + 1] = {};
		HMM_Vec2 mouse_position = HMM_V2(0.0f, 0.0f);
		HMM_Vec2 mouse_delta = HMM_V2(0.0f, 0.0f);
		bool is_mouse_locked = false;
	} input;

	struct SceneState
	{
		ankerl::unordered_dense::map<i32, Object> objects;
		std::optional<i32> camera_control_id;
		std::optional<i32> primary_sun_id;
		std::optional<i32> player_character_id;

		// Per-kind object id lists rebuilt lazily when dirty (port of game/'s
		// scene indexes)
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
		std::string port = "65432";
		std::thread thread;

		SOCKET blender_socket = socket_invalid();
		SOCKET connection_socket = socket_invalid();

		Channel<SceneUpdate> scene_updates;
	} live_link;

	// Batched per-object GPU data, rebuilt each frame and triple-buffered so
	// the CPU never writes a buffer a frame in flight still reads
	// (port of game/src/state/state.h:156-163)
	struct RenderObjectSnapshotState
	{
		StretchyBuffer<ObjectData> items;
		GpuBuffer<ObjectData> buffers[RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT];
		i32 buffer_index = 0;
		i32 buffer_capacity = 0;
		bool valid = false;
	} render_objects;

	// Registered materials (port of game/src/state/state.h:203-208). The GPU
	// buffer is a fixed MAX_MATERIALS-slot stream buffer created at init and
	// kept alive across resets — descriptor set 0 binding 2 is written every
	// frame and must always have a buffer (deviation from game/'s
	// destroy-on-reset).
	struct MaterialState
	{
		ankerl::unordered_dense::map<i32, i32> id_to_index;
		StretchyBuffer<Material> items;
		GpuBuffer<Material> buffer;
	} materials;

	// Registered images backing the bindless texture array
	// (port of game/src/state/state.h:210-216 minus debug fields)
	struct ImageState
	{
		ankerl::unordered_dense::map<i32, i32> id_to_index;
		StretchyBuffer<GpuImage> items;
		bool enable_debug_fullscreen = false;
		i32 debug_index = 0;
	} images;

	// Per-frame skin matrix arena: every skinned mesh's matrices are packed
	// into one SSBO each frame (mesh.skin_matrix_arena_offset indexes it).
	// Triple-buffered ring like the render-object snapshot. Deviation from
	// game/'s per-mesh stream buffers: game2's mapped stream buffers would
	// race the frame in flight; the ring is the same fix the snapshot uses.
	struct SkinMatrixArenaState
	{
		StretchyBuffer<HMM_Mat4> items;
		GpuBuffer<HMM_Mat4> buffers[RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT];
		i32 buffer_index = 0;
		i32 buffer_capacity = 0;
		bool valid = false;
	} skin_matrices;

	// Packed light data for the lighting pass. CPU arrays rebuilt when
	// needs_data_update; GPU side is a 3-ring per type uploaded every frame
	// (same hazard fix as the snapshot — game2's mapped stream buffers
	// would otherwise race frames in flight)
	struct LightingState
	{
		bool direct_enable = true;
		bool needs_data_update = true;

		StretchyBuffer<PointLightData> point_lights;
		StretchyBuffer<SpotLightData> spot_lights;
		StretchyBuffer<SunLightData> sun_lights;

		GpuBuffer<PointLightData> point_buffers[RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT];
		GpuBuffer<SpotLightData> spot_buffers[RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT];
		GpuBuffer<SunLightData> sun_buffers[RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT];
		i32 buffer_index = 0;
	} lighting;

	struct TonemappingState
	{
		f32 exposure_bias = 1.5f;	// game/ parity (state.h:361)
	} tonemapping;

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
		f32 intensity = 1.0f;
		bool show_probes = false;
		EProbeVisMode probe_vis_mode = EProbeVisMode::Irradiance;
		bool probe_isolation_enable = false;
		i32 isolated_probe_index = -1;
		bool compute_irradiance = true;
		bool is_updating = true;
	} gi;

	struct TessellationState
	{
		bool enabled = false;
		ETessellationMode mode = ETessellationMode::AdaptiveAngularPerTriangle;
		i32 fixed_factor = 4;
		i32 max_factor = 24;
		f32 target_pixels_per_segment = 20.0f;
		f32 phong_strength = 0.0f;
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
		bool readback_supported = true;
		i32 readback_age = 0;
	} tessellation;

	struct SkyState
	{
		bool rendering_enable = true;
	} sky;

	// Cascaded shadow settings (minimal port of game/'s ShadowState;
	// CenteredSquares mode + blur + debug fields arrive in 3b)
	struct ShadowState
	{
		bool rendering_enable = true;
		bool blur_enable = true;
		// Debug: freeze the shadow map (skip re-render/re-blur; lighting keeps
		// sampling the stale map with its frozen matrices)
		bool depth_freeze = false;
		bool force_recapture = false;
		i32 num_cascades = 3;
		f32 frustum_cascade_distance_scale = 1.0f;
		f32 centered_square_cascade_distance_scale = 0.25f;
		EShadowCascadePlacementMode cascade_placement_mode = EShadowCascadePlacementMode::Frustum;
		HMM_Vec3 centered_square_center = HMM_V3(0.0f, 0.0f, 0.0f);
		f32 centered_square_lookahead_distance = 50.0f;
		i32 debug_cascade_index = 0;
		i32 debug_view_mode = 0;
		bool debug_show_cascade_selection = false;
		f32 shadow_bias = 0.001f;

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

	struct SsaoState
	{
		bool enable = true;
	} ssao;

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

	struct WireframeState
	{
		bool shaded_wireframe = false;
		f32 width = 0.5f;
		f32 softness = 1.0f;
		f32 opacity = 0.75f;
		HMM_Vec4 color = HMM_V4(0.01f, 0.01f, 0.01f, 1.0f);
		f32 visibility_tolerance = 0.02f;
	} wireframe;

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

	// Fog controller selection (data only — fs_params + the fog render pass
	// arrive in Phase 3). Deviation from game/: active_fog_controller_id
	// lives here rather than in SceneState.
	struct FogState
	{
		bool debug_active = true;
		bool active = false;
		std::optional<i32> active_fog_controller_id;
	} fog;

	struct DebugCameraState
	{
		bool active = true;
		Camera camera = {
			.location = HMM_V3(2.5f, -15.0f, 3.0f),
			.forward = HMM_NormV3(HMM_V3(0.0f, 1.0f, -0.5f)),
			.up = HMM_NormV3(HMM_V3(0.0f, 0.0f, 1.0f)),
		};
	} debug_camera;

	struct DataOrientedState
	{
		struct LiveLinkImportStats : SceneUpdate::ImportStats
		{
			u64 update_index = 0;
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

	VulkanContext vk;
} state;

void data_oriented_begin_frame(State& in_state)
{
	in_state.data_oriented.previous_frame = in_state.data_oriented.frame;
	in_state.data_oriented.frame = {};
	++in_state.data_oriented.frame_index;
}

void scene_record_index_counts(State& in_state)
{
	in_state.data_oriented.frame.scene_object_count = (i32) in_state.scene.objects.size();
	in_state.data_oriented.frame.mesh_object_count = (i32) in_state.scene.indexes.mesh_object_ids.length();
	in_state.data_oriented.frame.light_object_count = (i32) in_state.scene.indexes.light_object_ids.length();
	in_state.data_oriented.frame.armature_object_count = (i32) in_state.scene.indexes.armature_object_ids.length();
	in_state.data_oriented.frame.skinned_mesh_object_count = (i32) in_state.scene.indexes.skinned_mesh_object_ids.length();
}

RenderPassEntry& get_render_pass_entry(ERenderPass in_pass)
{
	return state.render_passes.passes[(i32) in_pass];
}

RenderPass& get_render_pass(ERenderPass in_pass)
{
	return get_render_pass_entry(in_pass).final_pass();
}

void scene_mark_indexes_dirty(State& in_state)
{
	in_state.scene.indexes.dirty = true;
}

void scene_reset_indexes(State& in_state)
{
	in_state.scene.indexes.mesh_object_ids.clear();
	in_state.scene.indexes.light_object_ids.clear();
	in_state.scene.indexes.armature_object_ids.clear();
	in_state.scene.indexes.skinned_mesh_object_ids.clear();
	in_state.scene.indexes.dirty = true;
}

void scene_rebuild_indexes(State& in_state)
{
	State::SceneState::IndexState& indexes = in_state.scene.indexes;
	indexes.mesh_object_ids.clear();
	indexes.light_object_ids.clear();
	indexes.armature_object_ids.clear();
	indexes.skinned_mesh_object_ids.clear();

	for (auto& [unique_id, object] : in_state.scene.objects)
	{
		if (object.has_mesh)
		{
			indexes.mesh_object_ids.add(unique_id);

			if (object.mesh.has_skinned_vertices)
			{
				indexes.skinned_mesh_object_ids.add(unique_id);
			}
		}
		if (object.has_light)
		{
			indexes.light_object_ids.add(unique_id);
		}
		if (object.has_armature)
		{
			indexes.armature_object_ids.add(unique_id);
		}
	}

	indexes.dirty = false;
}

void scene_ensure_indexes(State& in_state)
{
	if (in_state.scene.indexes.dirty)
	{
		scene_rebuild_indexes(in_state);
	}
	scene_record_index_counts(in_state);
}

// Grows all snapshot buffers (doubling) when the mesh count exceeds capacity
// (port of game/src/state/state.h:557-587). Old buffers go through the
// deletion queue, so this is safe while frames are in flight.
void render_object_snapshot_ensure_capacity(State& in_state, i32 in_required_capacity)
{
	State::RenderObjectSnapshotState& render_objects = in_state.render_objects;
	if (in_required_capacity <= render_objects.buffer_capacity)
	{
		return;
	}

	i32 new_capacity = MAX(RENDER_OBJECT_SNAPSHOT_INITIAL_CAPACITY, render_objects.buffer_capacity);
	while (new_capacity < in_required_capacity)
	{
		new_capacity *= 2;
	}

	for (i32 buffer_idx = 0; buffer_idx < RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT; ++buffer_idx)
	{
		render_objects.buffers[buffer_idx].destroy_gpu_buffer();
		render_objects.buffers[buffer_idx] = GpuBuffer((GpuBufferDesc<ObjectData>){
			.data = nullptr,
			.size = sizeof(ObjectData) * (u64) new_capacity,
			.usage = {
				.storage_buffer = true,
				.stream_update = true,
			},
			.label = "State::render_objects",
		});
	}

	render_objects.buffer_capacity = new_capacity;
	render_objects.valid = false;
}

// Rebuilds the per-object GPU snapshot for this frame and advances the buffer
// ring (port of game/src/state/state.h:589-637). Runs before begin_frame:
// with a 3-buffer ring and 2 frames in flight, the buffer written here was
// last consumed 3 frames ago.
void build_render_object_snapshot(State& in_state)
{
	scene_ensure_indexes(in_state);

	State::RenderObjectSnapshotState& render_objects = in_state.render_objects;

	for (auto& [unique_id, object] : in_state.scene.objects)
	{
		in_state.data_oriented.frame.object_update_scan_count += 1;
		object.render_object_index = -1;
	}

	render_objects.items.clear();
	render_object_snapshot_ensure_capacity(in_state, (i32) in_state.scene.indexes.mesh_object_ids.length());

	for (i32 mesh_object_id : in_state.scene.indexes.mesh_object_ids)
	{
		auto found = in_state.scene.objects.find(mesh_object_id);
		if (found == in_state.scene.objects.end())
		{
			continue;
		}

		Object& object = found->second;
		in_state.data_oriented.frame.object_update_storage_updates += 1;
		if (object.has_mesh) in_state.data_oriented.frame.object_update_mesh_dirty_count += 1;
		object.render_object_index = (i32) render_objects.items.length();
		render_objects.items.add(object_make_render_data(object));
	}

	if (render_objects.items.length() == 0)
	{
		render_objects.valid = false;
		return;
	}

	render_objects.buffer_index = (render_objects.buffer_index + 1) % RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT;
	render_objects.buffers[render_objects.buffer_index].update_gpu_buffer(
		render_objects.items.data(),
		sizeof(ObjectData) * render_objects.items.length()
	);
	render_objects.valid = true;
}

// The buffer descriptor writes bind every frame (always valid — capacity is
// pre-created at init so empty scenes still have a buffer to bind)
GpuBuffer<ObjectData>& get_render_object_snapshot_buffer(State& in_state)
{
	return in_state.render_objects.buffers[in_state.render_objects.buffer_index];
}

// Fixed-size materials SSBO, created once (port of game/'s
// init_materials_buffer, state.h:722-743)
void init_materials_buffer(State& in_state)
{
	in_state.materials.buffer = GpuBuffer((GpuBufferDesc<Material>){
		.data = nullptr,
		.size = sizeof(Material) * MAX_MATERIALS,
		.usage = {
			.storage_buffer = true,
			.stream_update = true,
		},
		.label = "State::materials",
	});
}

// Uploads the whole registered-material array. Append-only updates are safe
// vs frames in flight: existing prefix bytes are rewritten with identical
// values (registered materials never change — game/ parity).
void update_materials_buffer(State& in_state)
{
	if (in_state.materials.items.length() == 0)
	{
		return;
	}

	in_state.materials.buffer.update_gpu_buffer(
		in_state.materials.items.data(),
		sizeof(Material) * in_state.materials.items.length()
	);
}

// Grows the skin matrix arena ring (clone of the snapshot grower)
void skin_matrix_arena_ensure_capacity(State& in_state, i32 in_required_capacity)
{
	State::SkinMatrixArenaState& arena = in_state.skin_matrices;
	if (in_required_capacity <= arena.buffer_capacity)
	{
		return;
	}

	i32 new_capacity = MAX(RENDER_OBJECT_SNAPSHOT_INITIAL_CAPACITY, arena.buffer_capacity);
	while (new_capacity < in_required_capacity)
	{
		new_capacity *= 2;
	}

	for (i32 buffer_idx = 0; buffer_idx < RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT; ++buffer_idx)
	{
		arena.buffers[buffer_idx].destroy_gpu_buffer();
		arena.buffers[buffer_idx] = GpuBuffer((GpuBufferDesc<HMM_Mat4>){
			.data = nullptr,
			.size = sizeof(HMM_Mat4) * (u64) new_capacity,
			.usage = {
				.storage_buffer = true,
				.stream_update = true,
			},
			.label = "State::skin_matrices",
		});
	}

	arena.buffer_capacity = new_capacity;
	arena.valid = false;
}

// Advances the arena ring and uploads this frame's packed matrices. Runs
// before begin_frame (same ring-depth safety argument as the snapshot).
void skin_matrix_arena_upload(State& in_state)
{
	State::SkinMatrixArenaState& arena = in_state.skin_matrices;
	if (arena.items.length() == 0)
	{
		arena.valid = false;
		return;
	}

	skin_matrix_arena_ensure_capacity(in_state, (i32) arena.items.length());

	arena.buffer_index = (arena.buffer_index + 1) % RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT;
	arena.buffers[arena.buffer_index].update_gpu_buffer(
		arena.items.data(),
		sizeof(HMM_Mat4) * arena.items.length()
	);
	arena.valid = true;
}

GpuBuffer<HMM_Mat4>& get_skin_matrix_arena_buffer(State& in_state)
{
	return in_state.skin_matrices.buffers[in_state.skin_matrices.buffer_index];
}

void mark_lighting_dirty(State& in_state)
{
	in_state.lighting.needs_data_update = true;
}

// Fixed-size light SSBO rings, created once at init (bindings must always
// be valid, even with zero lights)
void init_lighting_buffers(State& in_state)
{
	for (i32 buffer_idx = 0; buffer_idx < RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT; ++buffer_idx)
	{
		in_state.lighting.point_buffers[buffer_idx] = GpuBuffer((GpuBufferDesc<PointLightData>){
			.data = nullptr,
			.size = sizeof(PointLightData) * MAX_LIGHTS_PER_TYPE,
			.usage = { .storage_buffer = true, .stream_update = true },
			.label = "State::point_lights",
		});
		in_state.lighting.spot_buffers[buffer_idx] = GpuBuffer((GpuBufferDesc<SpotLightData>){
			.data = nullptr,
			.size = sizeof(SpotLightData) * MAX_LIGHTS_PER_TYPE,
			.usage = { .storage_buffer = true, .stream_update = true },
			.label = "State::spot_lights",
		});
		in_state.lighting.sun_buffers[buffer_idx] = GpuBuffer((GpuBufferDesc<SunLightData>){
			.data = nullptr,
			.size = sizeof(SunLightData) * MAX_LIGHTS_PER_TYPE,
			.usage = { .storage_buffer = true, .stream_update = true },
			.label = "State::sun_lights",
		});
	}
}

// Rebuilds the packed CPU light arrays from the scene when dirty
// (port of game/src/main.cpp:2830-2909)
void pack_lights(State& in_state)
{
	if (!in_state.lighting.needs_data_update)
	{
		return;
	}
	in_state.lighting.needs_data_update = false;

	State::LightingState& lighting = in_state.lighting;
	lighting.point_lights.clear();
	lighting.spot_lights.clear();
	lighting.sun_lights.clear();

	scene_ensure_indexes(in_state);
	in_state.data_oriented.frame.lighting_candidate_count += (i32) in_state.scene.indexes.light_object_ids.length();
	for (i32 light_object_id : in_state.scene.indexes.light_object_ids)
	{
		auto found = in_state.scene.objects.find(light_object_id);
		if (found == in_state.scene.objects.end())
		{
			continue;
		}

		Object& object = found->second;
		if (!object.has_light || !object.visibility)
		{
			continue;
		}
		in_state.data_oriented.frame.lighting_processed_count += 1;

		const Transform& transform = object.current_transform;
		const HMM_Vec4 location = HMM_V4(transform.location.X, transform.location.Y, transform.location.Z, 1.0f);
		const HMM_Vec4 color = HMM_V4(object.light.color.X, object.light.color.Y, object.light.color.Z, 1.0f);
		const HMM_Vec3 direction = HMM_NormV3(HMM_RotateV3Q(HMM_V3(0.0f, 0.0f, -1.0f), transform.rotation));

		switch (object.light.type)
		{
			case LightType::Point:
			{
				if (lighting.point_lights.length() >= MAX_LIGHTS_PER_TYPE) { break; }
				lighting.point_lights.add((PointLightData) {
					.location = location,
					.color = color,
					.power = object.light.point.power,
				});
				break;
			}
			case LightType::Spot:
			{
				if (lighting.spot_lights.length() >= MAX_LIGHTS_PER_TYPE) { break; }
				lighting.spot_lights.add((SpotLightData) {
					.location = location,
					.color = color,
					.power = object.light.spot.power,
					.spot_angle_radians = object.light.spot.beam_angle / 2.0f,
					.edge_blend = object.light.spot.edge_blend,
					.direction = HMM_V4V(direction, 0.0f),
				});
				break;
			}
			case LightType::Sun:
			{
				if (lighting.sun_lights.length() >= MAX_LIGHTS_PER_TYPE) { break; }
				lighting.sun_lights.add((SunLightData) {
					.location = location,
					.color = color,
					.power = object.light.sun.power,
					.cast_shadows = object.light.sun.cast_shadows ? 1 : 0,
					.direction = HMM_V4V(direction, 0.0f),
				});
				break;
			}
			default:
				break;
		}
	}
}

// Advances the light ring and uploads this frame's arrays (runs before
// begin_frame; ring depth 3 vs 2 frames in flight)
void upload_lights(State& in_state)
{
	State::LightingState& lighting = in_state.lighting;
	lighting.buffer_index = (lighting.buffer_index + 1) % RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT;

	if (lighting.point_lights.length() > 0)
	{
		lighting.point_buffers[lighting.buffer_index].update_gpu_buffer(
			lighting.point_lights.data(), sizeof(PointLightData) * lighting.point_lights.length());
	}
	if (lighting.spot_lights.length() > 0)
	{
		lighting.spot_buffers[lighting.buffer_index].update_gpu_buffer(
			lighting.spot_lights.data(), sizeof(SpotLightData) * lighting.spot_lights.length());
	}
	if (lighting.sun_lights.length() > 0)
	{
		lighting.sun_buffers[lighting.buffer_index].update_gpu_buffer(
			lighting.sun_lights.data(), sizeof(SunLightData) * lighting.sun_lights.length());
	}
}

bool is_key_pressed(i32 in_keycode)
{
	return state.input.keycodes[in_keycode];
}

HMM_Vec2 get_mouse_delta()
{
	return state.input.mouse_delta;
}

void reset_mouse_delta()
{
	state.input.mouse_delta = HMM_V2(0.0f, 0.0f);
}

bool is_mouse_locked()
{
	return state.input.is_mouse_locked;
}

void set_mouse_locked(bool in_locked)
{
	state.input.is_mouse_locked = in_locked;
	glfwSetInputMode(
		state.window.handle,
		GLFW_CURSOR,
		in_locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL
	);
}
