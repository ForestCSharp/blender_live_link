#pragma once

#include <map>
#include <optional>
#include <string>
#include <thread>

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

static constexpr i32 MIN_RENDER_RESOLUTION_PERCENTAGE = 25;
static constexpr i32 MAX_RENDER_RESOLUTION_PERCENTAGE = 100;
static constexpr i32 DEFAULT_RENDER_RESOLUTION_PERCENTAGE = 100;

// Frame-order pass registry (game/'s lives in state.h too; game/'s full list
// slots back in here as passes are ported in Phase 3)
enum class ERenderPass : i32
{
	Forward,
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

	struct AnimationState
	{
		bool is_playing = true;
		f32 playback_rate = 1.0f;
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
		HMM_Vec2 mouse_delta = HMM_V2(0.0f, 0.0f);
		bool is_mouse_locked = false;
	} input;

	struct SceneState
	{
		std::map<i32, Object> objects;
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
		std::map<i32, i32> id_to_index;
		StretchyBuffer<Material> items;
		GpuBuffer<Material> buffer;
	} materials;

	// Registered images backing the bindless texture array
	// (port of game/src/state/state.h:210-216 minus debug fields)
	struct ImageState
	{
		std::map<i32, i32> id_to_index;
		StretchyBuffer<GpuImage> items;
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

	VulkanContext vk;
} state;

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
