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

// Scene object definitions and helpers for transforms, animation, physics,
// rendering, and live-link updates.

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

struct SkyAtmosphere
{
	bool enabled = true;
	f32 planet_center_z_m = -6360000.0f;
	f32 air_density = 1.0f;
	f32 aerosol_density = 1.0f;
	f32 ozone_density = 1.0f;
	HMM_Vec3 ground_albedo = HMM_V3(0.1f, 0.1f, 0.1f);
	f32 sky_intensity = 1.0f;
	f32 sun_disc_angular_diameter_degrees = 0.5357f;
	f32 sun_disc_intensity = 1.0f;
	f32 atmosphere_height_m = 60000.0f;
	f32 rayleigh_scale_height_m = 8000.0f;
	f32 mie_scale_height_m = 1200.0f;
	f32 mie_anisotropy = 0.8f;
	f32 max_sun_zenith_angle_degrees = 102.0f;
};

static constexpr i32 MAX_CLOUD_LAYERS = 4;

enum class CloudLayerProfile : u8
{
	Stratus = 0,
	Cumulus = 1,
	Cumulonimbus = 2,
	Cirrus = 3,
};

struct CloudLayer
{
	bool enabled = true;
	CloudLayerProfile profile = CloudLayerProfile::Cumulus;
	u32 seed_offset = 0;
	f32 base_altitude_m = 1800.0f;
	f32 thickness_m = 3000.0f;
	f32 coverage = 0.5f;
	f32 density = 1.0f;
	f32 shape_scale_m = 8000.0f;
	f32 detail_scale_m = 1000.0f;
	f32 erosion = 0.65f;
	f32 anvil_bias = 0.1f;
	f32 wind_multiplier = 1.0f;
	f32 phase_forward = 0.75f;
	f32 phase_backward = -0.25f;
	f32 phase_blend = 0.8f;
	f32 ambient_scale = 0.6f;
	f32 multi_scattering_strength = 0.8f;
};

struct CloudSystem
{
	bool enabled = true;
	u32 seed = 1;
	f32 weather_world_scale_m = 100000.0f;
	HMM_Vec2 wind_direction = HMM_V2(1.0f, 0.0f);
	f32 wind_speed_m_s = 20.0f;
	bool shadow_enabled = true;
	f32 shadow_extent_m = 8000.0f;
	i32 layer_count = 0;
	CloudLayer layers[MAX_CLOUD_LAYERS] = {};
};

enum class PartType : u8
{
	Body = 0,
	Legs,
	LeftArm,
	RightArm,
	Head,
	Count,
};

enum class AttachmentBindingType : u8
{
	Object = 0,
	Bone,
};

struct Part
{
	PartType type = PartType::Body;
};

struct AttachmentPoint
{
	i32 owner_part_id = -1;
	PartType part_type = PartType::Legs;
	AttachmentBindingType binding_type = AttachmentBindingType::Object;
	i32 armature_id = -1;
	char* bone_name = nullptr;
	HMM_Mat4 local_transform = HMM_M4D(1.0f);
	bool valid = false;
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

AnimationClip* armature_get_active_animation(Armature& in_armature)
{
	if (in_armature.animation_count == 0 || !in_armature.animations)
	{
		return nullptr;
	}

	in_armature.active_animation_index = CLAMP(
		in_armature.active_animation_index,
		0,
		(i32) in_armature.animation_count - 1
	);
	return &in_armature.animations[in_armature.active_animation_index];
}

enum class ObjectStorageKind : u8
{
	Authored,
	RuntimePart,
	RuntimeArmature,
};

struct Object
{
	i32 unique_id;
	char* name = nullptr;

	// Runtime mech instances live in the ordinary scene object map so all
	// render, culling, skinning, and animation paths can consume them. Their
	// immutable mesh/armature data is borrowed from a hidden catalog template.
	ObjectStorageKind storage_kind = ObjectStorageKind::Authored;
	i32 template_object_id = -1;
	i32 mech_instance_id = -1;
	bool authoring_visibility;
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

	bool has_sky_atmosphere = false;
	SkyAtmosphere sky_atmosphere;

	bool has_cloud_system = false;
	CloudSystem cloud_system;

	bool has_part = false;
	Part part;

	bool has_attachment_point = false;
	AttachmentPoint attachment_point;
};

bool object_is_runtime_instance(const Object& in_object)
{
	return in_object.storage_kind != ObjectStorageKind::Authored;
}

bool object_has_dynamic_jolt_body(const Object& in_object)
{
	return in_object.has_rigid_body && in_object.rigid_body.is_dynamic;
}

bool object_has_dynamic_jolt_actor(const Object& in_object)
{
	return object_has_dynamic_jolt_body(in_object) || in_object.has_character;
}

// Static visible meshes feed the GI probe layout (dynamic actors move too
// often to bake).
bool object_contributes_to_gi_scene(const Object& in_object)
{
	return in_object.visibility && in_object.has_mesh && !in_object.has_part &&
		!object_is_runtime_instance(in_object) && !object_has_dynamic_jolt_actor(in_object);
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

// Builds a convex hull from the mesh vertices, scaled by the object's scale,
// and creates the corresponding Jolt body.
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

// Builds this object's row of the render-object snapshot SSBO for the current
// frame.
ObjectData object_make_render_data(const Object& in_object)
{
	const Transform& current_transform = in_object.current_transform;
	HMM_Vec4 location = current_transform.location;
	HMM_Quat rotation = current_transform.rotation;
	HMM_Vec3 scale = current_transform.scale;

	HMM_Mat4 scale_matrix = HMM_Scale(HMM_V3(scale.X, scale.Y, scale.Z));
	HMM_Mat4 rotation_matrix = HMM_QToM4(rotation);
	HMM_Mat4 translation_matrix = HMM_Translate(HMM_V3(location.X, location.Y, location.Z));

	// Use the first material index for the whole mesh; per-face materials are
	// not supported.
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
		.authoring_visibility = visibility,
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

// Frees armature bones and clips, including heap name strings and matrix
// arrays owned by each animation clip.
void object_cleanup_armature(Object& in_object)
{
	if (!in_object.has_armature)
	{
		return;
	}

	if (in_object.storage_kind == ObjectStorageKind::RuntimeArmature)
	{
		in_object.armature = {};
		in_object.has_armature = false;
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
		if (in_object.storage_kind != ObjectStorageKind::RuntimePart)
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
			}

			free(in_object.mesh.material_indices);
		}

		if (in_object.mesh.has_skinned_vertices)
		{
			in_object.mesh.skinned_vertex_cache_buffer.destroy_gpu_buffer();
			free(in_object.mesh.skin_matrices);
		}
		mesh_cleanup_tessellated_geometry(in_object.mesh);
		in_object.mesh = {};
		in_object.has_mesh = false;
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

	if (in_object.has_attachment_point)
	{
		free(in_object.attachment_point.bone_name);
		in_object.attachment_point.bone_name = nullptr;
	}
}
