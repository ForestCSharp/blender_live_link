#pragma once

#include "types.h"
#include "gpu_buffer.h"

#include "handmade_math/HandmadeMath.h"

// Generated Shader File
#include "basic_draw.compiled.h"

// Physics System so we can add/remove bodies
#include "physics_system.h"


struct Vertex
{
	HMM_Vec4 position;
	HMM_Vec4 normal;
};

struct Mesh 
{
	u32 index_count;
	u32* indices;
	GpuBuffer<u32> index_buffer; 

	u32 vertex_count;
	Vertex* vertices;
	GpuBuffer<Vertex> vertex_buffer; 
};

// Takes ownership of vertices and indices
Mesh make_mesh(
	Vertex* vertices, 
	u32 vertices_len, 
	u32* indices, 
	u32 indices_len
)
{
	u64 indices_size = sizeof(u32) * indices_len;
	u64 vertices_size = sizeof(Vertex) * vertices_len;

	return (Mesh) {
		.index_count = indices_len,
		.indices = indices,
		.index_buffer = GpuBuffer((GpuBufferDesc<u32>){
			.data = indices,
			.data_size = indices_size,
			.max_size = indices_size,
			.type = SG_BUFFERTYPE_INDEXBUFFER,
			.is_dynamic = false,
			.label = "Mesh::index_buffer",
		}),
		.vertex_count = vertices_len,
		.vertices = vertices,
		.vertex_buffer = GpuBuffer((GpuBufferDesc<Vertex>){
			.data = vertices,
			.data_size = vertices_size,
			.max_size = vertices_size,
			.type = SG_BUFFERTYPE_VERTEXBUFFER,
			.is_dynamic = false,
			.label = "Mesh::vertex_buffer",
		}),	
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

struct Light 
{
	LightType type;
	HMM_Vec3 color;	
	union
	{
		PointLight point;
		SpotLight spot;
	};
};

struct RigidBody
{
	bool is_dynamic;
	float mass;

	// Jolt Body ID
	JPH::Body* jolt_body = nullptr;
};

struct Transform
{
	HMM_Vec4 location;
	HMM_Quat rotation;
	HMM_Vec3 scale;
};

struct Object 
{
	i32 unique_id;
	bool visibility;

	Transform initial_transform;
	Transform current_transform;

	GpuBuffer<ObjectData_t> storage_buffer; 
	bool storage_buffer_needs_update = false;

	// Mesh Data, stored inline
	bool has_mesh;
	Mesh mesh;

	// Light Data, stored inline
	bool has_light;
	Light light;

	// Rigid Body Data, stored inline
	bool has_rigid_body;
	RigidBody rigid_body;	
};

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
	BodyInterface& body_interface = jolt_state.physics_system.GetBodyInterface();

	//FCS TODO: Support various shape types from blender...

	Mesh& mesh = in_object.mesh;	

	Array<Vec3> convex_hull_points;
	for (i32 vertex_index = 0; vertex_index < mesh.vertex_count; ++vertex_index)
	{
		Vertex& mesh_vertex = mesh.vertices[vertex_index];
		HMM_Vec4 position = mesh_vertex.position;
		convex_hull_points.emplace_back(Vec3(position.X, position.Y, position.Z));
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
	ShapeSettings::ShapeResult shape_result = shape_settings.Create();

	const Transform& current_transform = in_object.current_transform;

	JPH::Vec3 object_scale(current_transform.scale.X, current_transform.scale.Y, current_transform.scale.Z);
	ShapeSettings::ShapeResult scaled_shape_result = shape_result.Get()->ScaleShape(object_scale);

	JPH::Vec3 object_location(current_transform.location.X, current_transform.location.Y, current_transform.location.Z);
	JPH::Quat object_rotation(current_transform.rotation.X, current_transform.rotation.Y, current_transform.rotation.Z, current_transform.rotation.W);

	// Create the settings for the body itself. Note that here you can also set other properties like the restitution / friction.
	BodyCreationSettings body_creation_settings(
		scaled_shape_result.Get(), 
		object_location,
		object_rotation, 
		in_object.rigid_body.is_dynamic ? EMotionType::Dynamic : EMotionType::Static, 
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
		in_object.rigid_body.is_dynamic ? EActivation::Activate : EActivation::DontActivate
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

	BodyInterface& body_interface = jolt_state.physics_system.GetBodyInterface();
	body_interface.RemoveBody(in_object.rigid_body.jolt_body->GetID());
	body_interface.DestroyBody(in_object.rigid_body.jolt_body->GetID());
	in_object.rigid_body.jolt_body = nullptr;
}

void object_reset_jolt_body(Object& in_object)
{
	object_remove_jolt_body(in_object);
	object_add_jolt_body(in_object);
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

	ObjectData_t object_data = {
		.model_matrix = HMM_MulM4(translation_matrix, HMM_MulM4(rotation_matrix, scale_matrix)),
		.rotation_matrix = rotation_matrix,
	};

	in_object.storage_buffer.update_gpu_buffer(
		(sg_range){
			.ptr = &object_data,
			.size = sizeof(ObjectData_t),
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
		.storage_buffer = GpuBuffer((GpuBufferDesc<ObjectData_t>){
			.data = nullptr,
			.data_size = 0, 
			.max_size = sizeof(ObjectData_t),
			.type = SG_BUFFERTYPE_STORAGEBUFFER,
			.is_dynamic = true,
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
