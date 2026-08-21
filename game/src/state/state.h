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
#include "tonemapping_shared.h"

static constexpr i32 RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT = 3;
static constexpr i32 RENDER_OBJECT_SNAPSHOT_INITIAL_CAPACITY = 64;
static constexpr i32 MAX_LIGHTS_PER_TYPE = 1024;

static constexpr i32 MIN_RENDER_RESOLUTION_PERCENTAGE = 5;
static constexpr i32 MAX_RENDER_RESOLUTION_PERCENTAGE = 100;
static constexpr i32 DEFAULT_RENDER_RESOLUTION_PERCENTAGE = 50;

// Frame-order pass registry and render limits shared across scene state and
// render passes.
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
	Specular = 6,
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

enum class ETonemappingMethod : i32
{
	GT7 = TONEMAP_METHOD_GT7,
	AgX = TONEMAP_METHOD_AGX,
	Aces2 = TONEMAP_METHOD_ACES_2,
	KhronosPBRNeutral = TONEMAP_METHOD_KHRONOS_PBR_NEUTRAL,
	MAX = TONEMAP_METHOD_COUNT,
};

inline const char* ETessellationModeNames[(i32) ETessellationMode::MAX] = {
	"Fixed",
	"Adaptive Angular (Per Mesh)",
	"Adaptive Angular (Per Triangle)",
};

inline const char* ETonemappingMethodNames[(i32) ETonemappingMethod::MAX] = {
	"GT7",
	"AgX",
	"ACES 2.0",
	"Khronos PBR Neutral",
};

#include "live_link/live_link_types.h"

enum class MechLoadoutSelectionType : u8
{
	Default = 0,
	TemplateUid,
};

struct MechLoadoutSlot
{
	MechLoadoutSelectionType selection = MechLoadoutSelectionType::Default;
	i32 template_uid = -1;
};

struct MechLoadout
{
	MechLoadoutSlot slots[(i32) PartType::Count];
};

struct MechArmatureInstance
{
	i32 template_uid = -1;
	i32 instance_uid = -1;
};

struct MechInstance
{
	i32 runtime_id = -1;
	i32 character_uid = -1;
	MechLoadout loadout;
	i32 part_template_uids[(i32) PartType::Count] = {-1, -1, -1, -1, -1};
	i32 part_instance_uids[(i32) PartType::Count] = {-1, -1, -1, -1, -1};
	i32 socket_template_uids[(i32) PartType::Count] = {-1, -1, -1, -1, -1};
	DynamicArray<MechArmatureInstance> armature_instances;
	std::string last_diagnostic_signature;
};

// Global runtime state.
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
		bool show_local_tonemapping_debug = false;
		i32 local_tonemapping_debug_mip = 2;
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
		bool render_resolution_dirty = false;
	} window;

	RenderTargetRegistry render_targets;

	struct InputState
	{
		bool keycodes[GLFW_KEY_LAST + 1] = {};
		HMM_Vec2 mouse_position = HMM_V2(0.0f, 0.0f);
		HMM_Vec2 mouse_delta = HMM_V2(0.0f, 0.0f);
		bool is_mouse_locked = false;
		bool action_latches[4] = {};
		bool gi_probe_pick_requested = false;
	} input;

	struct SceneState
	{
		ankerl::unordered_dense::map<i32, Object> objects;
		std::optional<i32> camera_control_id;
		std::optional<i32> primary_sun_id;
		std::optional<i32> active_sky_controller_id;
		std::optional<i32> active_cloud_controller_id;
		std::optional<i32> player_character_id;
		i32 sky_controller_candidate_count = 0;
		i32 invalid_sky_controller_count = 0;
		i32 invalid_cloud_controller_count = 0;

		// Per-kind object id lists are rebuilt lazily when dirty and cached
		// until the next scene mutation.
		struct IndexState
		{
			bool dirty = true;
			DynamicArray<i32> mesh_object_ids;
			DynamicArray<i32> light_object_ids;
			DynamicArray<i32> armature_object_ids;
			DynamicArray<i32> skinned_mesh_object_ids;
			DynamicArray<i32> part_object_ids;
			DynamicArray<i32> attachment_point_object_ids;
		} indexes;
	} scene;

	struct MechState
	{
		ankerl::unordered_dense::map<i32, MechInstance> instances;
		ankerl::unordered_dense::map<i32, i32> character_to_instance;
		ankerl::unordered_dense::map<i32, bool> auto_spawn_opt_outs;
		i32 next_instance_id = 1;
		// -1 is the empty-slot sentinel throughout mech descriptors.
		i32 next_runtime_object_uid = -2;
	} mech;

	struct LiveLinkState
	{
		std::string port = "65432";
		std::thread thread;

		SOCKET blender_socket = socket_invalid();
		SOCKET connection_socket = socket_invalid();

		Channel<SceneUpdate> scene_updates;
	} live_link;

	// Batched per-object GPU data, rebuilt each frame and triple-buffered so
	// the CPU never writes a buffer that a frame in flight still reads.
	// Each ring slot has an independent allocation.
	ResizableGpuStreamRing<ObjectData> render_objects;

	// Registered materials. The GPU
	// buffer is a fixed MAX_MATERIALS-slot stream buffer created at init and
	// kept alive across resets — descriptor set 0 binding 2 is written every
	// frame and must always have a buffer.
	// Registered entries are append-only between resets.
	struct MaterialState
	{
		ankerl::unordered_dense::map<i32, i32> id_to_index;
		DynamicArray<Material> items;
		GpuBuffer<Material> buffer;
	} materials;

	// Registered images backing the bindless texture array. Indices remain
	// stable until a scene reset.
	struct ImageState
	{
		ankerl::unordered_dense::map<i32, i32> id_to_index;
		DynamicArray<GpuImage> items;
		bool enable_debug_fullscreen = false;
		i32 debug_index = 0;
	} images;

	// Per-frame skin matrix arena: every skinned mesh's matrices are packed
	// into one SSBO each frame (mesh.skin_matrix_arena_offset indexes it).
	// The triple-buffered ring prevents mapped writes from racing a frame in
	// flight, using the same lifetime scheme as the render-object snapshot.
	// Offsets are valid only for the current ring slot.
	ResizableGpuStreamRing<HMM_Mat4> skin_matrices;

	// Packed light data for the lighting pass. CPU arrays rebuilt when
	// needs_data_update; GPU side is a 3-ring per type uploaded every frame
	// so mapped writes cannot race frames in flight.
	// Point, spot, and sun lights use independent rings.
	struct LightingState
	{
		bool direct_enable = true;
		bool needs_data_update = true;

		DynamicArray<PointLightData> point_lights;
		DynamicArray<SpotLightData> spot_lights;
		DynamicArray<SunLightData> sun_lights;

		GpuBuffer<PointLightData> point_buffers[RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT];
		GpuBuffer<SpotLightData> spot_buffers[RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT];
		GpuBuffer<SunLightData> sun_buffers[RENDER_OBJECT_SNAPSHOT_BUFFER_COUNT];
		i32 buffer_index = 0;
		i32 active_atmosphere_sun_index = -1;
	} lighting;

	struct TonemappingState
	{
		const struct Defaults
		{
			f32 exposure_bias = 0.0f;
			bool auto_exposure_enabled = true;
			bool auto_white_balance_enabled = true;
			f32 auto_exposure_min_ev = -8.0f;
			f32 auto_exposure_max_ev = 8.0f;
			f32 auto_exposure_brightening_seconds = 1.0f;
			f32 auto_exposure_darkening_seconds = 0.35f;
			f32 auto_white_balance_seconds = 1.0f;
			f32 auto_white_balance_strength = 1.0f;
			ETonemappingMethod method = ETonemappingMethod::GT7;

			bool local_enabled = true;
			f32 local_shadow_recovery = 1.5f;
			f32 local_highlight_recovery = 2.0f;
			f32 local_exposure_preference_sigma = 5.0f;
			i32 local_coarsest_mip = 9;
			i32 local_reconstruction_mip = 2;
			bool local_contrast_boost = true;
		} DEFAULTS;

		f32 exposure_bias = DEFAULTS.exposure_bias;
		bool auto_exposure_enabled = DEFAULTS.auto_exposure_enabled;
		bool auto_white_balance_enabled = DEFAULTS.auto_white_balance_enabled;
		f32 auto_exposure_min_ev = DEFAULTS.auto_exposure_min_ev;
		f32 auto_exposure_max_ev = DEFAULTS.auto_exposure_max_ev;
		f32 auto_exposure_brightening_seconds = DEFAULTS.auto_exposure_brightening_seconds;
		f32 auto_exposure_darkening_seconds = DEFAULTS.auto_exposure_darkening_seconds;
		f32 auto_white_balance_seconds = DEFAULTS.auto_white_balance_seconds;
		f32 auto_white_balance_strength = DEFAULTS.auto_white_balance_strength;
		bool adaptation_reset_requested = true;
		bool adaptation_measurement_valid = false;
		f32 adaptation_current_ev = 0.0f;
		f32 adaptation_target_ev = 0.0f;
		f32 adaptation_measured_white_x = 0.3127f;
		f32 adaptation_measured_white_y = 0.3290f;
		f32 adaptation_current_l_gain = 1.0f;
		f32 adaptation_current_m_gain = 1.0f;
		f32 adaptation_current_s_gain = 1.0f;
		i32 adaptation_accepted_sample_count = 0;
		f32 adaptation_base_target_ev = 0.0f;
		f32 adaptation_guarded_target_ev = 0.0f;
		f32 adaptation_solar_guard_weight = 0.0f;
		f32 adaptation_solar_disc_ev = 0.0f;
		ETonemappingMethod method = DEFAULTS.method;
		bool local_enabled = DEFAULTS.local_enabled;

		f32 local_shadow_recovery = DEFAULTS.local_shadow_recovery;
		f32 local_highlight_recovery = DEFAULTS.local_highlight_recovery;
		f32 local_exposure_preference_sigma = DEFAULTS.local_exposure_preference_sigma;
		i32 local_coarsest_mip = DEFAULTS.local_coarsest_mip;
		i32 local_reconstruction_mip = DEFAULTS.local_reconstruction_mip;
		bool local_contrast_boost = DEFAULTS.local_contrast_boost;

		void reset_defaults()
		{
			exposure_bias = DEFAULTS.exposure_bias;
			auto_exposure_enabled = DEFAULTS.auto_exposure_enabled;
			auto_white_balance_enabled = DEFAULTS.auto_white_balance_enabled;
			auto_exposure_min_ev = DEFAULTS.auto_exposure_min_ev;
			auto_exposure_max_ev = DEFAULTS.auto_exposure_max_ev;
			auto_exposure_brightening_seconds = DEFAULTS.auto_exposure_brightening_seconds;
			auto_exposure_darkening_seconds = DEFAULTS.auto_exposure_darkening_seconds;
			auto_white_balance_seconds = DEFAULTS.auto_white_balance_seconds;
			auto_white_balance_strength = DEFAULTS.auto_white_balance_strength;
			method = DEFAULTS.method;
			local_enabled = DEFAULTS.local_enabled;
			local_shadow_recovery = DEFAULTS.local_shadow_recovery;
			local_highlight_recovery = DEFAULTS.local_highlight_recovery;
			local_exposure_preference_sigma = DEFAULTS.local_exposure_preference_sigma;
			local_coarsest_mip = DEFAULTS.local_coarsest_mip;
			local_reconstruction_mip = DEFAULTS.local_reconstruction_mip;
			local_contrast_boost = DEFAULTS.local_contrast_boost;
			adaptation_reset_requested = true;
		}
	} tonemapping;

	struct BloomState
	{
		inline static constexpr f32 MAX_INTENSITY = 5.0f;

		const struct Defaults
		{
			bool enable = true;
			f32 threshold = 1.0f;
			f32 soft_knee = 0.5f;
			f32 intensity = 3.0f;
			f32 auto_exposure_influence = 0.0f;
			i32 requested_mip_count = 6;
		} DEFAULTS;

		bool enable = DEFAULTS.enable;
		f32 threshold = DEFAULTS.threshold;
		f32 soft_knee = DEFAULTS.soft_knee;
		f32 intensity = DEFAULTS.intensity;
		f32 auto_exposure_influence = DEFAULTS.auto_exposure_influence;
		i32 requested_mip_count = DEFAULTS.requested_mip_count;

		void reset_defaults()
		{
			threshold = DEFAULTS.threshold;
			soft_knee = DEFAULTS.soft_knee;
			intensity = DEFAULTS.intensity;
			auto_exposure_influence = DEFAULTS.auto_exposure_influence;
			requested_mip_count = DEFAULTS.requested_mip_count;
		}
	} bloom;

	struct GiState
	{
		bool enable = true;
		bool probe_influence_culling = true;
		bool probe_occlusion = true;
		i32 octree_depth = 4;
		bool layout_dirty = true;
		EProbeOcclusionMode probe_occlusion_mode = EProbeOcclusionMode::Chebyshev;
		EProbeRadianceMode probe_radiance_mode = EProbeRadianceMode::Octahedral;
		bool render_sky_to_probes = true;
		bool debug_constant_white_probes = false;
		bool probe_specular_enable = true;
		f32 intensity = 1.0f;
		bool show_probes = false;
		bool probe_level_filter_enable = false;
		i32 probe_level_filter_selection = 0;
		EProbeVisMode probe_vis_mode = EProbeVisMode::Irradiance;
		f32 specular_debug_roughness = 0.0f;
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

	// Cascaded shadow settings shared by depth, blur, and lighting passes.
	// Placement supports frustum-fit and centered-squares modes.
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

	// Fog controller selection. The lowest-id enabled and visible controller
	// supplies the fog render pass parameters.
	// The active id is refreshed as scene visibility changes.
	struct FogState
	{
		bool debug_active = true;
		bool active = false;
		std::optional<i32> active_fog_controller_id;
	} fog;

	struct CloudState
	{
		static constexpr f32 DEFAULT_RESOLUTION_SCALE = 1.0f;
		static constexpr i32 DEFAULT_VIEW_STEPS = 40;
		static constexpr f32 DEFAULT_DENSE_STEP_SCALE = 0.75f;
		static constexpr f32 DEFAULT_EMPTY_STEP_SCALE = 2.0f;
		static constexpr i32 DEFAULT_SUN_CONE_SAMPLES = 6;
		static constexpr f32 DEFAULT_HISTORY_WEIGHT = 0.94f;
		static constexpr f32 DEFAULT_DEPTH_REJECTION = 0.4f;
		static constexpr f32 DEFAULT_LOW_DENSITY_EDGE_FADE = 0.08f;
		static constexpr f32 DEFAULT_MINIMUM_DENSITY = 0.002f;
		static constexpr f32 DEFAULT_HISTORY_CLIP_SIGMA = 1.25f;
		static constexpr f32 DEFAULT_OPACITY_REJECTION = 0.35f;

		bool active = false;
		bool shadow_lighting_enabled = true;
		bool debug_show_shadow_map_fullscreen = false;
		bool history_reset_requested = true;
		f32 elapsed_time_seconds = 0.0f;
		i32 active_layer_count = 0;
		bool layer_budget_warning = false;
		f32 gpu_time_ms = 0.0f;
		f32 resolution_scale = DEFAULT_RESOLUTION_SCALE;
		i32 view_steps = DEFAULT_VIEW_STEPS;
		f32 dense_step_scale = DEFAULT_DENSE_STEP_SCALE;
		f32 empty_step_scale = DEFAULT_EMPTY_STEP_SCALE;
		i32 sun_cone_samples = DEFAULT_SUN_CONE_SAMPLES;
		f32 history_weight = DEFAULT_HISTORY_WEIGHT;
		f32 depth_rejection = DEFAULT_DEPTH_REJECTION;
		f32 low_density_edge_fade = DEFAULT_LOW_DENSITY_EDGE_FADE;
		f32 minimum_density = DEFAULT_MINIMUM_DENSITY;
		f32 history_clip_sigma = DEFAULT_HISTORY_CLIP_SIGMA;
		f32 opacity_rejection = DEFAULT_OPACITY_REJECTION;
	} clouds;

	struct DebugCameraState
	{
		bool active = true;
		bool live_link_initialization_complete = false;
		f32 move_speed = 10.0f;
		HMM_Vec3 initial_location = HMM_V3(2.5f, -15.0f, 3.0f);
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
			i32 cull_influence_count = 0;
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
		DynamicArray<LiveLinkImportStats> import_history;
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

RenderPass& get_render_target(RenderTargetId in_target)
{
	return state.render_targets.get(in_target);
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
	in_state.scene.indexes.part_object_ids.clear();
	in_state.scene.indexes.attachment_point_object_ids.clear();
	in_state.scene.indexes.dirty = true;
}

void scene_rebuild_indexes(State& in_state)
{
	State::SceneState::IndexState& indexes = in_state.scene.indexes;
	indexes.mesh_object_ids.clear();
	indexes.light_object_ids.clear();
	indexes.armature_object_ids.clear();
	indexes.skinned_mesh_object_ids.clear();
	indexes.part_object_ids.clear();
	indexes.attachment_point_object_ids.clear();

	ankerl::unordered_dense::map<i32, bool> catalog_armature_ids;
	for (auto& [unique_id, object] : in_state.scene.objects)
	{
		if (object.has_part && !object_is_runtime_instance(object) && object.has_mesh &&
			object.mesh.has_skinned_vertices && object.mesh.armature_id >= 0)
		{
			catalog_armature_ids[object.mesh.armature_id] = true;
		}
	}

	for (auto& [unique_id, object] : in_state.scene.objects)
	{
		// Catalog Parts and sockets are immutable authoring templates. Runtime
		// clones deliberately omit those components and enter normal render and
		// skinning indexes below.
		if (object.has_mesh && !object.has_part && !object.has_attachment_point)
		{
			indexes.mesh_object_ids.add(unique_id);

			if (object.mesh.has_skinned_vertices)
			{
				indexes.skinned_mesh_object_ids.add(unique_id);
			}
		}
		if (object.has_light && !object.has_part && !object.has_attachment_point)
		{
			indexes.light_object_ids.add(unique_id);
		}
		if (object.has_armature &&
			(object_is_runtime_instance(object) || !catalog_armature_ids.contains(unique_id)))
		{
			indexes.armature_object_ids.add(unique_id);
		}
		if (object.has_part && !object_is_runtime_instance(object))
		{
			indexes.part_object_ids.add(unique_id);
		}
		if (object.has_attachment_point && !object_is_runtime_instance(object))
		{
			indexes.attachment_point_object_ids.add(unique_id);
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

// Grows all snapshot buffers (doubling) when the mesh count exceeds capacity.
// Old buffers go through the
// deletion queue, so this is safe while frames are in flight.
void render_object_snapshot_ensure_capacity(State& in_state, i32 in_required_capacity)
{
	in_state.render_objects.configure(
		"State::render_objects",
		RENDER_OBJECT_SNAPSHOT_INITIAL_CAPACITY
	);
	in_state.render_objects.ensure_capacity(in_required_capacity);
}

// Rebuilds the per-object GPU snapshot for this frame and advances the buffer
// ring. Runs before begin_frame:
// with a 3-buffer ring and 2 frames in flight, the buffer written here was
// last consumed 3 frames ago.
void build_render_object_snapshot(State& in_state)
{
	scene_ensure_indexes(in_state);

	ResizableGpuStreamRing<ObjectData>& render_objects = in_state.render_objects;

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

	render_objects.upload();
}

// The buffer descriptor writes bind every frame (always valid — capacity is
// pre-created at init so empty scenes still have a buffer to bind)
GpuBuffer<ObjectData>& get_render_object_snapshot_buffer(State& in_state)
{
	return in_state.render_objects.current();
}

// Fixed-size materials SSBO, created once and kept valid across scene resets.
// Descriptor binding 2 always points at this allocation.
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
// values because registered materials never change.
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
	in_state.skin_matrices.configure(
		"State::skin_matrices",
		RENDER_OBJECT_SNAPSHOT_INITIAL_CAPACITY
	);
	in_state.skin_matrices.ensure_capacity(in_required_capacity);
}

// Advances the arena ring and uploads this frame's packed matrices. Runs
// before begin_frame (same ring-depth safety argument as the snapshot).
void skin_matrix_arena_upload(State& in_state)
{
	in_state.skin_matrices.upload();
}

GpuBuffer<HMM_Mat4>& get_skin_matrix_arena_buffer(State& in_state)
{
	return in_state.skin_matrices.current();
}

void mark_lighting_dirty(State& in_state)
{
	in_state.lighting.needs_data_update = true;
}

void scene_invalidate_cached_object_ids(State& in_state, i32 in_unique_id)
{
	if (in_state.scene.camera_control_id == in_unique_id)
	{
		in_state.scene.camera_control_id.reset();
	}
	if (in_state.scene.player_character_id == in_unique_id)
	{
		in_state.scene.player_character_id.reset();
	}
	if (in_state.scene.primary_sun_id == in_unique_id)
	{
		in_state.scene.primary_sun_id.reset();
	}
	if (in_state.scene.active_sky_controller_id == in_unique_id)
	{
		in_state.scene.active_sky_controller_id.reset();
	}
	if (in_state.scene.active_cloud_controller_id == in_unique_id)
	{
		in_state.scene.active_cloud_controller_id.reset();
		in_state.clouds.active = false;
		in_state.clouds.history_reset_requested = true;
	}
	if (in_state.fog.active_fog_controller_id == in_unique_id)
	{
		in_state.fog.active_fog_controller_id.reset();
		in_state.fog.active = false;
	}
}

void scene_insert_or_replace_object(State& in_state, Object&& in_object)
{
	const i32 unique_id = in_object.unique_id;
	bool lighting_changed = in_object.has_light;
	bool gi_scene_geometry_changed = object_contributes_to_gi_scene(in_object);

	auto found = in_state.scene.objects.find(unique_id);
	if (found != in_state.scene.objects.end())
	{
		lighting_changed = lighting_changed || found->second.has_light;
		gi_scene_geometry_changed =
			gi_scene_geometry_changed || object_contributes_to_gi_scene(found->second);
		scene_invalidate_cached_object_ids(in_state, unique_id);
		object_cleanup(found->second);
		found->second = std::move(in_object);
	}
	else
	{
		in_state.scene.objects[unique_id] = std::move(in_object);
	}

	scene_mark_indexes_dirty(in_state);
	if (lighting_changed)
	{
		mark_lighting_dirty(in_state);
	}
	if (gi_scene_geometry_changed)
	{
		in_state.gi.layout_dirty = true;
	}
}

bool scene_remove_object(State& in_state, i32 in_unique_id)
{
	auto found = in_state.scene.objects.find(in_unique_id);
	if (found == in_state.scene.objects.end())
	{
		return false;
	}

	const bool lighting_changed = found->second.has_light;
	const bool gi_scene_geometry_changed = object_contributes_to_gi_scene(found->second);
	scene_invalidate_cached_object_ids(in_state, in_unique_id);
	object_cleanup(found->second);
	in_state.scene.objects.erase(found);
	scene_mark_indexes_dirty(in_state);

	if (lighting_changed)
	{
		mark_lighting_dirty(in_state);
	}
	if (gi_scene_geometry_changed)
	{
		in_state.gi.layout_dirty = true;
	}
	return true;
}

void scene_clear_objects(State& in_state)
{
	for (auto& [unique_id, object] : in_state.scene.objects)
	{
		object_cleanup(object);
	}
	in_state.scene.objects.clear();
	in_state.scene.camera_control_id.reset();
	in_state.scene.player_character_id.reset();
	in_state.scene.primary_sun_id.reset();
	in_state.scene.active_sky_controller_id.reset();
	in_state.scene.active_cloud_controller_id.reset();
	in_state.scene.sky_controller_candidate_count = 0;
	in_state.scene.invalid_sky_controller_count = 0;
	in_state.scene.invalid_cloud_controller_count = 0;
	in_state.clouds.active = false;
	in_state.clouds.history_reset_requested = true;
	in_state.fog.active_fog_controller_id.reset();
	in_state.fog.active = false;
	in_state.tonemapping.adaptation_reset_requested = true;
	scene_reset_indexes(in_state);
	mark_lighting_dirty(in_state);
	in_state.gi.layout_dirty = true;
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

// Rebuilds the packed CPU light arrays from the scene when dirty.
// The lighting pass uploads the packed arrays through per-type stream rings.
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
	lighting.active_atmosphere_sun_index = -1;

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
				const i32 packed_sun_index = (i32)lighting.sun_lights.length();
				lighting.sun_lights.add((SunLightData) {
					.location = location,
					.color = color,
					.power = object.light.sun.power,
					.cast_shadows = object.light.sun.cast_shadows ? 1 : 0,
					.direction = HMM_V4V(direction, 0.0f),
				});
				if (in_state.scene.active_sky_controller_id == light_object_id)
					lighting.active_atmosphere_sun_index = packed_sun_index;
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
