#pragma once

#include "types.h"
#include "gpu_buffer.h"

#include "handmade_math/HandmadeMath.h"

// Generated Shader File
#include "geometry.compiled.h"

// Physics System so we can add/remove bodies
#include "physics_system.h"

// Character code
#include "character.h"

// Camera code
#include "camera.h"

struct Vertex
{
	HMM_Vec4 position;
	HMM_Vec4 normal;
	HMM_Vec2 texcoord;
	float _padding[2];
};

struct Mesh 
{
	u32 index_count;
	u32* indices;
	GpuBuffer<u32> index_buffer; 

	u32 vertex_count;
	Vertex* vertices;
	GpuBuffer<Vertex> vertex_buffer;

	u32 material_indices_count;
	i32* material_indices;

	BoundingBox bounding_box;
};

// Takes ownership of vertices and indices
Mesh make_mesh(
	Vertex* vertices, u32 vertices_len, 
	u32* indices, u32 indices_len,
	i32* material_indices, u32 num_material_indices
)
{
	BoundingBox bounding_box = bounding_box_init();

	for (i32 vtx_idx = 0; vtx_idx < vertices_len; ++vtx_idx)
	{
		Vertex v = vertices[vtx_idx];
		HMM_Vec3 vtx_pos = v.position.XYZ;
		bounding_box.min = HMM_MinV3(bounding_box.min, vtx_pos);
		bounding_box.max = HMM_MaxV3(bounding_box.max, vtx_pos);
	}

	u64 indices_size = sizeof(u32) * indices_len;
	u64 vertices_size = sizeof(Vertex) * vertices_len;

	return (Mesh) {
		.index_count = indices_len,
		.indices = indices,
		.index_buffer = GpuBuffer((GpuBufferDesc<u32>){
			.data = indices,
			.size = indices_size,
			.usage = {
				.index_buffer = true,
			},
			.label = "Mesh::index_buffer",
		}),
		.vertex_count = vertices_len,
		.vertices = vertices,
		.vertex_buffer = GpuBuffer((GpuBufferDesc<Vertex>){
			.data = vertices,
			.size = vertices_size,
			.usage = {
				.vertex_buffer = true,
			},
			.label = "Mesh::vertex_buffer",
		}),	
		.material_indices_count = num_material_indices,
		.material_indices = material_indices,
		.bounding_box = bounding_box,
	};
}

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

	// Jolt Body ID
	JPH::Body* jolt_body = nullptr;
};

struct Object 
{
	i32 unique_id;
	bool visibility;

	Transform initial_transform;
	Transform current_transform;

	GpuBuffer<geometry_ObjectData_t> storage_buffer; 
	bool storage_buffer_needs_update = false;

	// Mesh Data, stored inline
	bool has_mesh = false;
	Mesh mesh;

	// Light Data, stored inline
	bool has_light = false;
	Light light;

	// Rigid Body Data, stored inline
	bool has_rigid_body = false;
	RigidBody rigid_body;

	// Character Data, stored inline
	bool has_character = false;
	Character character;

	// Camera Control Data, stored inline
	bool has_camera_control = false;
	CameraControl camera_control;
};

BoundingBox object_get_bounding_box(const Object& in_object)
{
	//FCS TODO: Eventually support other object bounding boxes (rigid_body, etc.)
	assert(in_object.has_mesh);
	return bounding_box_transform(in_object.mesh.bounding_box, in_object.current_transform);	
}

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

	// Get our body interface from the physics_system
	JPH::BodyInterface& body_interface = jolt_state.physics_system.GetBodyInterface();

	//FCS TODO: Support various shape types from blender...

	Mesh& mesh = in_object.mesh;	

	JPH::Array<JPH::Vec3> convex_hull_points;
	for (i32 vertex_index = 0; vertex_index < mesh.vertex_count; ++vertex_index)
	{
		Vertex& mesh_vertex = mesh.vertices[vertex_index];
		HMM_Vec4 position = mesh_vertex.position;
		convex_hull_points.emplace_back(JPH::Vec3(position.X, position.Y, position.Z));
	}

	// Create the settings object for a convex hull
    JPH::ConvexHullShapeSettings shape_settings(convex_hull_points, JPH::cDefaultConvexRadius);

	{
		// TODO: Will need to force mesh shapes to always be static...

		//VertexList vertices;
		//for (i32 vertex_index = 0; vertex_index < mesh.vertex_count; ++vertex_index)
		//{
		//	Vertex& mesh_vertex = mesh.vertices[vertex_index];
		//	HMM_Vec4 position = mesh_vertex.position;
		//	vertices.emplace_back(Float3(position.X, position.Y, position.Z));
		//}

		//IndexedTriangleList triangles;
		//for (i32 indices_index = 0; indices_index < mesh.index_count; indices_index += 3)
		//{
		//	u32 idx_0 = mesh.indices[indices_index + 0];
		//	u32 idx_1 = mesh.indices[indices_index + 1];
		//	u32 idx_2 = mesh.indices[indices_index + 2];
		//	triangles.emplace_back(IndexedTriangle(idx_0, idx_1, idx_2));
		//}

		//MeshShapeSettings shape_settings(vertices, triangles);
	}

	// Create the shape
	JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();

	const Transform& current_transform = in_object.current_transform;

	JPH::Vec3 object_scale(current_transform.scale.X, current_transform.scale.Y, current_transform.scale.Z);
	JPH::ShapeSettings::ShapeResult scaled_shape_result = shape_result.Get()->ScaleShape(object_scale);

	JPH::Vec3 object_location(current_transform.location.X, current_transform.location.Y, current_transform.location.Z);
	JPH::Quat object_rotation(current_transform.rotation.X, current_transform.rotation.Y, current_transform.rotation.Z, current_transform.rotation.W);

	// Create the settings for the body itself. Note that here you can also set other properties like the restitution / friction.
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

	// Actually add the body to the simulation
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

void object_reset_jolt_body(Object& in_object)
{
	object_remove_jolt_body(in_object);
	object_add_jolt_body(in_object);
}

void object_add_character(Object& in_object, const CharacterSettings& in_settings)
{
	in_object.character = character_create(jolt_state, in_settings);
	in_object.has_character = true;
}

void object_remove_character(Object& in_object)
{
	character_destroy(in_object.character);
	in_object.has_character = false;
	in_object.character = {};
}

void object_add_camera_control(Object& in_object, const CameraControlSettings& in_settings)
{
	in_object.camera_control = camera_control_create(in_settings);
	in_object.has_camera_control = true;
}

void object_remove_camera_control(Object& in_object)
{
	in_object.has_camera_control = false;
	in_object.camera_control = {};
}

void object_update_storage_buffer(Object& in_object)
{
	const Transform& current_transform = in_object.current_transform;
	HMM_Vec4 location = current_transform.location;
	HMM_Quat rotation = current_transform.rotation;
	HMM_Vec3 scale = current_transform.scale;

	HMM_Mat4 scale_matrix = HMM_Scale(HMM_V3(scale.X, scale.Y, scale.Z));
	HMM_Mat4 rotation_matrix = HMM_QToM4(rotation);
	HMM_Mat4 translation_matrix = HMM_Translate(HMM_V3(location.X, location.Y, location.Z));

	// Just set to first material index for now
	int material_index 	= (in_object.has_mesh && in_object.mesh.material_indices_count > 0)
						? in_object.mesh.material_indices[0] 
						: -1;

	geometry_ObjectData_t object_data = {
		.model_matrix = HMM_MulM4(translation_matrix, HMM_MulM4(rotation_matrix, scale_matrix)),
		.rotation_matrix = rotation_matrix,
		.material_index = material_index,
	};

	in_object.storage_buffer.update_gpu_buffer(
		(sg_range){
			.ptr = &object_data,
			.size = sizeof(geometry_ObjectData_t),
		}
	);
}

// Partially creates an object, but doesn't set up optional data (mesh, light, etc.)
Object object_create(
	i32 unique_id,	
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
		.visibility = visibility,
		.initial_transform = transform,
		.current_transform = transform,

		// Create our dynamic storage buffer and mark it for update later on the game thread
		.storage_buffer = GpuBuffer((GpuBufferDesc<geometry_ObjectData_t>){
			.data = nullptr,
			.size = sizeof(geometry_ObjectData_t),
			.usage = {
				.storage_buffer = true,
				.stream_update = true,
			},
			.label = "Object::storage_buffer",
		}),
		.storage_buffer_needs_update = true,

		// No mesh yet
		.has_mesh = false,
		.mesh = {},

		// No light yet
		.has_light = false,
		.light = {},

		// No rigid body yet
		.has_rigid_body = false,
		.rigid_body = {},
	};

	return out_object;
}

// Cleans up data on object
void object_cleanup_gpu_resources(Object& in_object)
{
	in_object.storage_buffer.destroy_gpu_buffer();
	if (in_object.has_mesh)
	{
		free(in_object.mesh.indices);
		in_object.mesh.index_buffer.destroy_gpu_buffer();

		free(in_object.mesh.vertices);
		in_object.mesh.vertex_buffer.destroy_gpu_buffer();
	}
}

void object_cleanup(Object& in_object)
{
	object_cleanup_gpu_resources(in_object);

	if (in_object.has_rigid_body)
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

void object_copy_physics_transform(Object& in_object, JPH::BodyInterface& in_body_interface)
{
	if (in_object.has_rigid_body && in_object.rigid_body.jolt_body)
	{
		const JPH::BodyID body_id = in_object.rigid_body.jolt_body->GetID();
		JPH::RVec3 body_position;
		JPH::Quat body_rotation;
		in_body_interface.GetPositionAndRotation(body_id, body_position, body_rotation);

		// Update Transform location and rotation and mark storage buffer for update
		Transform& transform = in_object.current_transform;
		transform.location = HMM_V4(body_position.GetX(), body_position.GetY(), body_position.GetZ(), 1.0);
		transform.rotation = HMM_Q(body_rotation.GetX(), body_rotation.GetY(), body_rotation.GetZ(), body_rotation.GetW());
		in_object.storage_buffer_needs_update = true;
	}
	else if (in_object.has_character && in_object.character.jph_character)
	{
		JPH::RVec3 body_position;
		JPH::Quat body_rotation;
		in_object.character.jph_character->GetPositionAndRotation(body_position, body_rotation);

		// Update Transform location and rotation and mark storage buffer for update
		Transform& transform = in_object.current_transform;
		transform.location = HMM_V4(body_position.GetX(), body_position.GetY(), body_position.GetZ(), 1.0);
		transform.rotation = HMM_Q(body_rotation.GetX(), body_rotation.GetY(), body_rotation.GetZ(), body_rotation.GetW());
		in_object.storage_buffer_needs_update = true;
	}
}
