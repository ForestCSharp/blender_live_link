#pragma once

#include "core/types.h"
#include "render/gpu_buffer.h"

#include "handmade_math/HandmadeMath.h"

// Camera code
#include "camera.h"

#include "game_object/mesh.h"

// Stripped-down port of game/'s game_object.h: no Jolt rigid bodies,
// characters, armatures, or fog controllers yet.

enum class LightType : u8
{
	Point	= 0,
	Spot 	= 1,
	Sun 	= 2,
	Area	= 3,
};

struct PointLight {
	float power;
};

struct SpotLight {
	float power;
	float beam_angle;
	float edge_blend;
};

struct SunLight {
	float power;
	bool cast_shadows;
};

struct Light
{
	LightType type;
	HMM_Vec3 color;
	union
	{
		PointLight	point;
		SpotLight	spot;
		SunLight	sun;
	};
};

struct Object
{
	i32 unique_id;
	char* name = nullptr;
	bool visibility;

	Transform initial_transform;
	Transform current_transform;

	// Mesh Data, stored inline
	bool has_mesh = false;
	Mesh mesh;

	// Light Data, stored inline
	bool has_light = false;
	Light light;

	// Camera Control Data, stored inline
	bool has_camera_control = false;
	CameraControl camera_control;
};

BoundingBox object_get_bounding_box(const Object& in_object)
{
	assert(in_object.has_mesh);
	return bounding_box_transform(in_object.mesh.bounding_box, in_object.current_transform);
}

void object_add_camera_control(Object& in_object, const CameraControlSettings& in_settings)
{
	in_object.has_camera_control = true;
	in_object.camera_control = camera_control_create(in_settings);
}

void object_remove_camera_control(Object& in_object)
{
	in_object.has_camera_control = false;
	in_object.camera_control = {};
}

HMM_Mat4 object_get_model_matrix(const Object& in_object)
{
	const Transform& current_transform = in_object.current_transform;
	HMM_Vec4 location = current_transform.location;
	HMM_Quat rotation = current_transform.rotation;
	HMM_Vec3 scale = current_transform.scale;

	HMM_Mat4 scale_matrix = HMM_Scale(HMM_V3(scale.X, scale.Y, scale.Z));
	HMM_Mat4 rotation_matrix = HMM_QToM4(rotation);
	HMM_Mat4 translation_matrix = HMM_Translate(HMM_V3(location.X, location.Y, location.Z));

	return HMM_MulM4(translation_matrix, HMM_MulM4(rotation_matrix, scale_matrix));
}

// Partially creates an object, but doesn't set up optional data (mesh, light, etc.)
Object object_create(
	i32 unique_id,
	char* name,
	bool visibility,
	HMM_Vec4 location,
	HMM_Quat rotation,
	HMM_Vec3 scale
)
{
	Transform transform = {
		.location = location,
		.rotation = rotation,
		.scale = scale,
	};

	Object out_object = {
		.unique_id = unique_id,
		.name = name,
		.visibility = visibility,
		.initial_transform = transform,
		.current_transform = transform,

		// No mesh yet
		.has_mesh = false,
		.mesh = {},

		// No light yet
		.has_light = false,
		.light = {},
	};

	return out_object;
}

// Cleans up data on object. GPU buffer destruction is deferred through the
// deletion queue, so this is safe to call while frames are in flight.
void object_cleanup(Object& in_object)
{
	free(in_object.name);
	in_object.name = nullptr;

	if (in_object.has_mesh)
	{
		free(in_object.mesh.indices);
		in_object.mesh.index_buffer.destroy_gpu_buffer();

		free(in_object.mesh.wire_indices);
		in_object.mesh.wire_index_buffer.destroy_gpu_buffer();

		if (in_object.mesh.vertices)
		{
			free(in_object.mesh.vertices);
			in_object.mesh.vertex_buffer.destroy_gpu_buffer();
		}
	}

	if (in_object.has_camera_control)
	{
		object_remove_camera_control(in_object);
	}
}
