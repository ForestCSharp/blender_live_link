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
	u32 idx_count;
	GpuBuffer<u32> index_buffer; 
	GpuBuffer<Vertex> vertex_buffer; 
};

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
		.idx_count = indices_len,
		.index_buffer = GpuBuffer((GpuBufferDesc<u32>){
			.data = indices,
			.data_size = indices_size,
			.max_size = indices_size,
			.type = SG_BUFFERTYPE_INDEXBUFFER,
			.is_dynamic = false,
			.label = "Mesh::index_buffer",
		}),
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

struct Object 
{
	i32 unique_id;
	bool visibility;

	// Object's world location
	HMM_Vec4 location;
	HMM_Quat rotation;
	HMM_Vec3 scale;

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

//FCS TODO: Setup Object Collision Shape
//FCS TODO: Setup Object Position
//FCS TODO: Setup Object Rotation

//FCS TODO: SEtup Object Scale
// Shapes can be scaled using the ScaledShape class. You can scale a shape like:
//JPH::Shape::ShapeResult my_scaled_shape = my_non_scaled_shape->ScaleShape(JPH::Vec3(x_scale, y_scale, z_scale));

void object_add_jolt_body(Object& in_object)
{
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

	//FCS TODO: Create Real Shape from mesh data
	BoxShapeSettings shape_settings(Vec3(10.0f, 10.0f, 10.0f));

	// Create the shape
	ShapeSettings::ShapeResult shape_result = shape_settings.Create();

	JPH::Vec3 object_scale(in_object.scale.X, in_object.scale.Y, in_object.scale.Z);
	ShapeSettings::ShapeResult scaled_shape_result = shape_result.Get()->ScaleShape(object_scale);

	JPH::Vec3 object_location(in_object.location.X, in_object.location.Y, in_object.location.Z);
	JPH::Quat object_rotation(in_object.rotation.X, in_object.rotation.Y, in_object.rotation.Z, in_object.rotation.W);

	// Create the settings for the body itself. Note that here you can also set other properties like the restitution / friction.
	BodyCreationSettings body_creation_settings(
		scaled_shape_result.Get(), 
		object_location,
		object_rotation, 
		in_object.rigid_body.is_dynamic ? EMotionType::Dynamic : EMotionType::Static, 
		in_object.rigid_body.is_dynamic ? Layers::MOVING : Layers::NON_MOVING
	);

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

void object_update_storage_buffer(Object& in_object)
{
	HMM_Vec4 location = in_object.location;
	HMM_Quat rotation = in_object.rotation;
	HMM_Vec3 scale = in_object.scale;

	HMM_Mat4 scale_matrix = HMM_Scale(HMM_V3(scale.X, scale.Y, scale.Z));
	HMM_Mat4 rotation_matrix = HMM_QToM4(rotation);
	HMM_Mat4 translation_matrix = HMM_Translate(HMM_V3(location.X, location.Y, location.Z));

	ObjectData_t object_data = {
		.model_matrix = HMM_MulM4(translation_matrix, HMM_MulM4(rotation_matrix, scale_matrix)),
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
	Object out_object = {
		.unique_id = unique_id,
		.visibility = visibility,
		.location = location,
		.rotation = rotation,
		.scale = scale,

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
		.has_mesh = false,
		.mesh = {},
	};

	return out_object;
}

// Cleans up data on object
void object_cleanup_gpu_resources(Object& in_object)
{
	in_object.storage_buffer.destroy_gpu_buffer();
	if (in_object.has_mesh)
	{
		in_object.mesh.index_buffer.destroy_gpu_buffer();
		in_object.mesh.vertex_buffer.destroy_gpu_buffer();
	}
}
