#pragma once

#include <optional>

#include "core/types.h"
#include "game_object/camera.h"
#include "game_object/game_object.h"

// ---- Live link messages ----
// Images and materials must not be registered on the live-link thread: GPU
// image creation submits to the graphics queue, and the id->index maps are
// read by the main thread. Instead the parse thread packages one
// SceneUpdate per flatbuffer Update and ALL registration happens at drain
// on the main thread in this order:
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
	DynamicArray<PendingImage> images;
	DynamicArray<PendingMaterial> materials;

	// Note: each Object's mesh.material_indices still holds raw material IDS
	// here; resolve_mesh_material_indices converts them at drain
	DynamicArray<Object> objects;

	DynamicArray<i32> deleted_object_uids;
	std::optional<Camera> editor_camera;
	bool has_object_batch = false;
	bool reset = false;
};

