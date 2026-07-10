#pragma once

#include "core/types.h"
#include "render/gpu_buffer.h"

#include "handmade_math/HandmadeMath.h"

// ObjectData (shared with shaders)
#include "shader_common.h"

// Physics System so we can add/remove bodies
#include "physics/physics_system.h"

// Character code
#include "character.h"

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

struct RigidBody
{
	bool is_dynamic;
	float mass;

	// Jolt Body
	JPH::Body* jolt_body = nullptr;
};

struct FogController
{
	bool enabled = true;
	f32 density = 0.015f;
	f32 base_height = 0.0f;
	f32 scale_height = 25.0f;
	f32 max_distance = 500.0f;
	bool ceiling_enabled = false;
	f32 ceiling_height = 100.0f;
	f32 ceiling_fade = 25.0f;
	HMM_Vec3 fog_color = HMM_V3(0.55f, 0.65f, 0.75f);
	f32 ambient_intensity = 0.4f;
	f32 sun_intensity = 1.0f;
	f32 anisotropy = 0.2f;
};

struct ArmatureBone
{
	char* name = nullptr;
	i32 parent_index = -1;
	HMM_Mat4 inverse_bind_matrix = HMM_M4D(1.0f);
};

struct AnimationClip
{
	char* name = nullptr;
	f32 frame_rate = 0.0f;
	f32 duration_seconds = 0.0f;
	i32 frame_count = 0;
	i32 bone_count = 0;
	HMM_Mat4* skin_matrices = nullptr;	// frame-major [frame_count * bone_count]
};

struct Armature
{
	u32 bone_count = 0;
	ArmatureBone* bones = nullptr;

	u32 animation_count = 0;
	AnimationClip* animations = nullptr;
	i32 active_animation_index = 0;
	f32 playback_time = 0.0f;
	i32 current_frame = 0;
};

struct Object
{
	i32 unique_id;
	char* name = nullptr;
	bool visibility;

	Transform initial_transform;
	Transform current_transform;

	// Index into the render-object snapshot SSBO, rebuilt each frame
	i32 render_object_index = -1;

	// Mesh Data, stored inline
	bool has_mesh = false;
	Mesh mesh;

	// Light Data, stored inline
	bool has_light = false;
	Light light;

	// Armature Data, stored inline
	bool has_armature = false;
	Armature armature;

	// Rigid Body Data, stored inline
	bool has_rigid_body = false;
	RigidBody rigid_body;

	// Character Data, stored inline
	bool has_character = false;
	Character character;

	// Camera Control Data, stored inline
	bool has_camera_control = false;
	CameraControl camera_control;

	// Fog Controller Data, stored inline (data only — the fog render pass
	// is Phase 3)
	bool has_fog_controller = false;
	FogController fog_controller;
};

bool object_has_dynamic_jolt_body(const Object& in_object)
{
	return in_object.has_rigid_body && in_object.rigid_body.is_dynamic;
}

bool object_has_dynamic_jolt_actor(const Object& in_object)
{
	return object_has_dynamic_jolt_body(in_object) || in_object.has_character;
}

void object_add_character(Object& in_object, const CharacterSettings& in_settings)
{
	in_object.has_character = true;
	in_object.character = character_create(jolt_state, in_settings);
}

void object_remove_character(Object& in_object)
{
	character_destroy(in_object.character);
	in_object.has_character = false;
}

// Builds a convex hull from the mesh vertices, scaled by the object's scale
// (port of game/src/game_object/game_object.h:175-249)
void object_add_jolt_body(Object& in_object)
{
	if (!in_object.has_mesh)
	{
		printf("jolt_add_body error: mesh is currently required\n");
		return;
	}

	if (!in_object.has_rigid_body)
	{
		printf("jolt_add_body error: in_object doesn't have a rigid_body\n");
		return;
	}

	if (in_object.rigid_body.jolt_body != nullptr)
	{
		printf("jolt_add_body error: in_object's rigid_body already has a jolt_body\n");
		return;
	}

	JPH::BodyInterface& body_interface = jolt_state.physics_system.GetBodyInterface();

	//FCS TODO: Support various shape types from blender...

	Mesh& mesh = in_object.mesh;

	JPH::Array<JPH::Vec3> convex_hull_points;
	for (u32 vertex_index = 0; vertex_index < mesh.vertex_count; ++vertex_index)
	{
		Vertex& mesh_vertex = mesh.vertices[vertex_index];
		HMM_Vec4 position = mesh_vertex.position;
		convex_hull_points.emplace_back(JPH::Vec3(position.X, position.Y, position.Z));
	}

	JPH::ConvexHullShapeSettings shape_settings(convex_hull_points, JPH::cDefaultConvexRadius);
	JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();

	const Transform& current_transform = in_object.current_transform;

	JPH::Vec3 object_scale(current_transform.scale.X, current_transform.scale.Y, current_transform.scale.Z);
	JPH::ShapeSettings::ShapeResult scaled_shape_result = shape_result.Get()->ScaleShape(object_scale);

	JPH::Vec3 object_location(current_transform.location.X, current_transform.location.Y, current_transform.location.Z);
	JPH::Quat object_rotation(current_transform.rotation.X, current_transform.rotation.Y, current_transform.rotation.Z, current_transform.rotation.W);

	JPH::BodyCreationSettings body_creation_settings(
		scaled_shape_result.Get(),
		object_location,
		object_rotation,
		in_object.rigid_body.is_dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
		in_object.rigid_body.is_dynamic ? Layers::MOVING : Layers::NON_MOVING
	);

	// Set Rigid Body Mass
	JPH::MassProperties msp;
	msp.ScaleToMass(in_object.rigid_body.mass);
	body_creation_settings.mMassPropertiesOverride = msp;
	body_creation_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;

	// Note that if we run out of bodies this can return nullptr
	in_object.rigid_body.jolt_body = body_interface.CreateBody(body_creation_settings);

	body_interface.AddBody(
		in_object.rigid_body.jolt_body->GetID(),
		in_object.rigid_body.is_dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate
	);
}

void object_remove_jolt_body(Object& in_object)
{
	if (!in_object.has_rigid_body)
	{
		printf("jolt_remove_body error: in_object doesn't have a rigid_body\n");
		return;
	}

	if (in_object.rigid_body.jolt_body == nullptr)
	{
		printf("jolt_remove_body error: in_object's rigid_body doesn't have a jolt_body\n");
		return;
	}

	JPH::BodyInterface& body_interface = jolt_state.physics_system.GetBodyInterface();
	body_interface.RemoveBody(in_object.rigid_body.jolt_body->GetID());
	body_interface.DestroyBody(in_object.rigid_body.jolt_body->GetID());
	in_object.rigid_body.jolt_body = nullptr;
}

// Recreates the body from current_transform (Ctrl+R restores the transform
// first, so this rebuilds the body at the initial pose)
void object_reset_jolt_body(Object& in_object)
{
	object_remove_jolt_body(in_object);
	object_add_jolt_body(in_object);
}

// Physics -> object transform writeback (location + rotation only)
void object_copy_physics_transform(Object& in_object, JPH::BodyInterface& in_body_interface)
{
	if (in_object.has_rigid_body && in_object.rigid_body.jolt_body)
	{
		const JPH::BodyID body_id = in_object.rigid_body.jolt_body->GetID();
		JPH::RVec3 body_position;
		JPH::Quat body_rotation;
		in_body_interface.GetPositionAndRotation(body_id, body_position, body_rotation);

		Transform& transform = in_object.current_transform;
		transform.location = HMM_V4(body_position.GetX(), body_position.GetY(), body_position.GetZ(), 1.0);
		transform.rotation = HMM_Q(body_rotation.GetX(), body_rotation.GetY(), body_rotation.GetZ(), body_rotation.GetW());
	}
	else if (in_object.has_character && in_object.character.jph_character)
	{
		JPH::RVec3 body_position;
		JPH::Quat body_rotation;
		in_object.character.jph_character->GetPositionAndRotation(body_position, body_rotation);

		Transform& transform = in_object.current_transform;
		transform.location = HMM_V4(body_position.GetX(), body_position.GetY(), body_position.GetZ(), 1.0);
		transform.rotation = HMM_Q(body_rotation.GetX(), body_rotation.GetY(), body_rotation.GetZ(), body_rotation.GetW());
	}
}

bool object_is_sun_light(const Object& in_object)
{
	return in_object.has_light && in_object.light.type == LightType::Sun;
}

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

// Builds this object's row of the render-object snapshot SSBO
// (port of game/src/game_object/game_object.h:303-324)
ObjectData object_make_render_data(const Object& in_object)
{
	const Transform& current_transform = in_object.current_transform;
	HMM_Vec4 location = current_transform.location;
	HMM_Quat rotation = current_transform.rotation;
	HMM_Vec3 scale = current_transform.scale;

	HMM_Mat4 scale_matrix = HMM_Scale(HMM_V3(scale.X, scale.Y, scale.Z));
	HMM_Mat4 rotation_matrix = HMM_QToM4(rotation);
	HMM_Mat4 translation_matrix = HMM_Translate(HMM_V3(location.X, location.Y, location.Z));

	// Just set to first material index for now (game/ parity — per-face
	// materials are not supported)
	int material_index = (in_object.has_mesh && in_object.mesh.material_indices_count > 0)
		? in_object.mesh.material_indices[0]
		: -1;

	return (ObjectData) {
		.model_matrix = HMM_MulM4(translation_matrix, HMM_MulM4(rotation_matrix, scale_matrix)),
		.rotation_matrix = rotation_matrix,
		.material_index = material_index,
	};
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

// Frees armature bones/clips (heap name strings + matrix arrays)
// (port of game/src/game_object/game_object.h:394-416)
void object_cleanup_armature(Object& in_object)
{
	if (!in_object.has_armature)
	{
		return;
	}

	for (u32 bone_idx = 0; bone_idx < in_object.armature.bone_count; ++bone_idx)
	{
		free(in_object.armature.bones[bone_idx].name);
	}
	free(in_object.armature.bones);

	for (u32 animation_idx = 0; animation_idx < in_object.armature.animation_count; ++animation_idx)
	{
		free(in_object.armature.animations[animation_idx].name);
		free(in_object.armature.animations[animation_idx].skin_matrices);
	}
	free(in_object.armature.animations);

	in_object.armature = {};
	in_object.has_armature = false;
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

		if (in_object.mesh.has_skinned_vertices)
		{
			free(in_object.mesh.skinned_vertices);
			in_object.mesh.skinned_vertex_buffer.destroy_gpu_buffer();
			free(in_object.mesh.skin_matrices);
		}

		free(in_object.mesh.material_indices);
	}

	object_cleanup_armature(in_object);

	if (in_object.has_rigid_body && in_object.rigid_body.jolt_body != nullptr)
	{
		object_remove_jolt_body(in_object);
	}

	if (in_object.has_character)
	{
		object_remove_character(in_object);
	}

	if (in_object.has_camera_control)
	{
		object_remove_camera_control(in_object);
	}
}
