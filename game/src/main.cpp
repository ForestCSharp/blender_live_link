// C std lib I/O and stdlib
#include <cstdio>
#include <cstdlib>

// Simple Template Wrapper around stb_ds array
#define STB_DS_IMPLEMENTATION
#include "core/stretchy_buffer.h"

// C++ std lib Optional
#include <optional>
using std::optional;

// C++ std lib Random
#include <random>

// C++ std lib threading
#include <thread>

// C++ std lib atomic
#include <atomic>
using std::atomic;

// C++ std lib string
#include <string>

// For C++ std::function
#include <functional>

// Flatbuffers generated file
#include "blender_live_link_generated.h"

// Jolt Physics
//#include "Jolt/jolt_single_file.cpp"
#include "physics/physics_system.h"

// Basic Types
#include "core/types.h"

// Game Objects we receive from Blender
#include "game_object/game_object.h"

// Gpu Image and Buffer Wrappers
#include "render/gpu_buffer.h"
#include "render/gpu_image.h"

// Render Pass abstraction
#include "render/render_pass.h"

// Cubemap rendering
#include "render/cubemap_render.h"

// Ankerl's Segmented Vector and Fast Unordered Hash Math
#include "ankerl/unordered_dense.h"
using ankerl::unordered_dense::map;

// Handmade Math
#define HANDMADE_MATH_IMPLEMENTATION
#include "handmade_math/HandmadeMath.h"

// cxxopts
#include "cxxopts/cxxopts.hpp"

// Sokol
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_log.h"
#include "sokol/sokol_glue.h"

// Sokol Debug Text
#include "sokol/util/sokol_debugtext.h"
#define FONT_KC853 (0)
#define FONT_KC854 (1)
#define FONT_Z1013 (2)
#define FONT_CPC   (3)
#define FONT_C64   (4)
#define FONT_ORIC  (5)

#include "render/shader_files.h"

// geometry pass
#include "render/geometry_pass.h"

// Wrapper for sockets 
#include "network/socket_wrapper.h"

// Thread-Safe Channel
#include "network/channel.h"

// Culling
#include "render/culling.h"

// Global State
#include "state/state.h"

// Macro to define an event and run code in __VA_ARGS__ when it triggers
#define DEFINE_EVENT_TWO_KEYS(key_1, key_2, ...)\
	{\
		static bool was_key_pressed = false;\
		const bool is_key_1_pressed = is_key_pressed(key_1);\
		const bool is_key_2_pressed = (key_2 == SAPP_KEYCODE_INVALID) ? true : is_key_pressed(key_2);\
		if (is_key_1_pressed && is_key_2_pressed && !was_key_pressed)\
		{\
			{__VA_ARGS__}\
		}\
		was_key_pressed = is_key_1_pressed && is_key_2_pressed;\
	}


// Same as above but no mod key
#define DEFINE_EVENT_ONE_KEY(key, ...) DEFINE_EVENT_TWO_KEYS(key, SAPP_KEYCODE_INVALID, __VA_ARGS__)

// Macro to handle toggleable inputs. uses event macro above but just sets variable instead of passing in __VA_ARGS__
#define DEFINE_TOGGLE_TWO_KEYS(var_name, key_1, key_2)\
	DEFINE_EVENT_TWO_KEYS(key_1, key_2,\
		var_name = !var_name;\
	);

// Same as above but no mod key
#define DEFINE_TOGGLE_ONE_KEY(var_name, initial_state, key)\
	DEFINE_TOGGLE_TWO_KEYS(var_name, initial_state, key, SAPP_KEYCODE_INVALID,\
		var_name = !var_name;\
	)

#define WITH_DEBUG_UI 1
#if WITH_DEBUG_UI

// Only include ImGui when debug UI is enabled
#define IMGUI_IMPLEMENTATION
#define SOKOL_IMGUI_IMPL
#include "imgui/misc/single_file/imgui_single_file.h"
#include "sokol/util/sokol_imgui.h"

bool g_show_imgui = true;
// All imgui debug ui should be wrapped in this.
#define DEBUG_UI(...)\
	{\
		if (g_show_imgui)\
		{\
			__VA_ARGS__\
		}\
	}
#else
// when debug ui is disabled, this macro does nothing
#define DEBUG_UI(...)
#endif // WITH_DEBUG_UI

// Flatbuffer helper conversion functions
namespace flatbuffer_helpers
{
	HMM_Vec3 to_hmm_vec3(const Blender::LiveLink::Vec3* in_flatbuffers_vector)
	{
		assert(in_flatbuffers_vector);
		return HMM_V3(
			in_flatbuffers_vector->x(), 
			in_flatbuffers_vector->y(), 
			in_flatbuffers_vector->z()
		);
	}

	HMM_Vec4 to_hmm_vec4(const Blender::LiveLink::Vec4* in_flatbuffers_vector)
	{
		assert(in_flatbuffers_vector);
		return HMM_V4(
			in_flatbuffers_vector->x(), 
			in_flatbuffers_vector->y(), 
			in_flatbuffers_vector->z(),
			in_flatbuffers_vector->w()
		);
	}

	HMM_Vec4 to_hmm_vec4(const Blender::LiveLink::Vec3* in_flatbuffers_vector, float in_w)
	{
		assert(in_flatbuffers_vector);
		return HMM_V4(
			in_flatbuffers_vector->x(), 
			in_flatbuffers_vector->y(), 
			in_flatbuffers_vector->z(),
			in_w
		);
	}

	HMM_Quat to_hmm_quat(const Blender::LiveLink::Quat* in_flatbuffers_quat)
	{
		assert(in_flatbuffers_quat);
		return HMM_Q(
			in_flatbuffers_quat->x(),
			in_flatbuffers_quat->y(),
			in_flatbuffers_quat->z(),
			in_flatbuffers_quat->w()
		);
	}
}

static void draw_debug_text(int font_index, const char* title, uint8_t r, uint8_t g, uint8_t b)
{
    sdtx_font(font_index);
    sdtx_color3b(r, g, b);
    sdtx_puts(title);
    sdtx_crlf();
}

bool register_image(const Blender::LiveLink::Image& in_image)
{
	int image_id = in_image.unique_id();

	if (state.image_id_to_index.contains(image_id))
	{
		return false;
	}
	printf("Registering image with ID: %i\n", image_id);

	auto data_vec = in_image.data();

	GpuImageDesc image_desc = {
		.type = SG_IMAGETYPE_2D,
		.width = in_image.width(),
		.height = in_image.height(),
		.pixel_format = SG_PIXELFORMAT_RGBA32F, 
		.data = data_vec->data(),
	};

	const i32 array_index = state.images.length();
	state.images.add(GpuImage(image_desc));
	state.image_id_to_index[image_id] = array_index;
	return true;
}

void reset_images()
{
	state.image_id_to_index.clear();
	state.images.reset();
	state.enable_debug_image_fullscreen = false;
	state.debug_image_index = 0;
}

// Registers a material and adds it to our gpu buffer
bool register_material(const Blender::LiveLink::Material& in_material)
{
	int material_id = in_material.unique_id();
	if (state.material_id_to_index.contains(material_id))
	{
		return false;
	}

	printf("Registering material with ID: %i\n", material_id);
	if (state.materials.length() >= MAX_MATERIALS)
	{
		printf("Error Material Limit Reached: %i\n", MAX_MATERIALS);
		exit(0);
		return false;
	}

	geometry_Material_t new_material = {
		.base_color = flatbuffer_helpers::to_hmm_vec4(in_material.base_color()),
		.emission_color = flatbuffer_helpers::to_hmm_vec4(in_material.emission_color()),
		.metallic = in_material.metallic(),
		.roughness = in_material.roughness(),
		.emission_strength = in_material.emission_strength(),
		.base_color_image_index = -1,
		.metallic_image_index = -1,
		.roughness_image_index = -1,
		.emission_color_image_index = -1,
	};

	int base_color_image_id = in_material.base_color_image_id();
	if (base_color_image_id > 0)
	{
		printf("Found base color image id: %i\n", base_color_image_id);
		assert(state.image_id_to_index.contains(base_color_image_id));
		new_material.base_color_image_index = state.image_id_to_index[base_color_image_id];
	}

	int metallic_image_id = in_material.metallic_image_id();
	if (metallic_image_id > 0)
	{
		printf("Found metallic image id: %i\n", metallic_image_id);
		assert(state.image_id_to_index.contains(metallic_image_id));
		new_material.metallic_image_index = state.image_id_to_index[metallic_image_id];
	}

	int roughness_image_id = in_material.roughness_image_id();
	if (roughness_image_id > 0)
	{
		printf("Found roughness image id: %i\n", roughness_image_id);
		assert(state.image_id_to_index.contains(roughness_image_id));
		new_material.roughness_image_index = state.image_id_to_index[roughness_image_id];
	}

	int emission_color_image_id = in_material.emission_color_image_id();
	if (emission_color_image_id > 0)
	{
		printf("Found roughness image id: %i\n", emission_color_image_id);
		assert(state.image_id_to_index.contains(emission_color_image_id));
		new_material.emission_color_image_index = state.image_id_to_index[emission_color_image_id];
	}

	const i32 array_index = state.materials.length();
	state.materials.add(new_material);
	state.material_id_to_index[material_id] = array_index;
	return true;	
}

void reset_materials()
{
	state.material_id_to_index.clear();
	state.materials.reset();
	state.materials_buffer.destroy_gpu_buffer();
}

RenderPass& get_render_pass(const ERenderPass in_pass_id)
{
	return state.render_passes[static_cast<int>(in_pass_id)];
}

Camera& get_active_camera()
{
	if (state.camera_control_id && !state.debug_camera_active)
	{	
		Object& camera_control_target = state.objects[*state.camera_control_id];
		assert(camera_control_target.has_camera_control);
		return camera_control_target.camera_control.camera;
	}

	return state.debug_camera;
}

void parse_flatbuffer_data(StretchyBuffer<u8>& flatbuffer_data)
{
	if (flatbuffer_data.length() > 0)
	{
		// Interpret Flatbuffer data
		auto* update = Blender::LiveLink::GetSizePrefixedUpdate(flatbuffer_data.data());
		assert(update);

		// process images from update
		if (auto images = update->images())
		{
			bool needs_images_update = false;
			for (i32 idx = 0; idx < images->size(); ++idx)
			{
				auto image = images->Get(idx);
				assert(image);
				needs_images_update = register_image(*image);
			}
		}

		// process materials from update
		if (auto materials = update->materials())
		{	
			bool needs_buffer_update = false;
			for (i32 idx = 0; idx < materials->size(); ++idx)
			{
				auto material = materials->Get(idx);
				assert(material);
				needs_buffer_update = register_material(*material);
			}

			if (needs_buffer_update)
			{
				update_materials_buffer();	
			}
		}

		// process objects from update
		if (auto objects = update->objects())
		{
			for (i32 idx = 0; idx < objects->size(); ++idx)
			{
				auto object = objects->Get(idx);
				if (auto object_name = object->name())
				{
					printf("\tObject Name: %s\n", object_name->c_str());
				}

				int unique_id = object->unique_id();
				bool visibility = object->visibility();

				auto object_location = object->location();
				if (!object_location)
				{
					continue;
				}

				auto object_scale = object->scale();
				if (!object_scale)
				{
					continue;
				}

				auto object_rotation = object->rotation();
				if (!object_rotation)
				{
					continue;
				}

				HMM_Vec4 location 	= flatbuffer_helpers::to_hmm_vec4(object_location, 1.0f);
				HMM_Vec3 scale 		= flatbuffer_helpers::to_hmm_vec3(object_scale);
				HMM_Quat rotation 	= flatbuffer_helpers::to_hmm_quat(object_rotation);

				Object game_object = object_create(
					unique_id, 
					visibility, 
					location, 
					rotation,
					scale
				);

				if (auto object_mesh = object->mesh())
				{
					u32 num_vertices = 0;
					Vertex* vertices = nullptr;
					SkinnedVertex* skinned_vertices = nullptr;

					auto flatbuffer_positions = object_mesh->positions();
					auto flatbuffer_normals = object_mesh->normals();
					auto flatbuffer_texcoords = object_mesh->texcoords();
					if (flatbuffer_positions && flatbuffer_normals && flatbuffer_texcoords)
					{
						num_vertices = flatbuffer_positions->size() / 3;
						u32 num_normals = flatbuffer_normals->size() / 3;
						u32 num_texcoords = flatbuffer_texcoords->size() / 2;

						vertices = (Vertex*) malloc(sizeof(Vertex) * num_vertices);
						for (i32 vertex_idx = 0; vertex_idx < num_vertices; ++vertex_idx)
						{
							vertices[vertex_idx] = {
								.position = {
									.X = flatbuffer_positions->Get(vertex_idx * 3 + 0),
									.Y = flatbuffer_positions->Get(vertex_idx * 3 + 1),
									.Z = flatbuffer_positions->Get(vertex_idx * 3 + 2),
									.W = 1.0,
								},
								.normal = {
									.X = flatbuffer_normals->Get(vertex_idx * 3 + 0),
									.Y = flatbuffer_normals->Get(vertex_idx * 3 + 1),
									.Z = flatbuffer_normals->Get(vertex_idx * 3 + 2),
									.W = 0.0,
								},
								.texcoord = {
									.X = flatbuffer_texcoords->Get(vertex_idx * 2 + 0),
									.Y = flatbuffer_texcoords->Get(vertex_idx * 2 + 1),
								},
							};
						}
					}

					// Check for skinning data
					i32 armature_id = object_mesh->armature_id();
					auto flatbuffer_joint_indices = object_mesh->joint_indices();
					auto flatbuffer_joint_weights = object_mesh->joint_weights();
					if (armature_id > 0 && flatbuffer_joint_indices && flatbuffer_joint_weights)
					{
						// Make sure we already set up regular vertices and the skinned data matches that count
						assert(num_vertices > 0);
						
						const u32 num_joint_indices = flatbuffer_joint_indices->size();
						const u32 num_joint_weights = flatbuffer_joint_weights->size();
						assert(num_joint_indices == num_joint_weights);

						u32 num_skinned_vertices = num_joint_indices / 4;
						assert(num_vertices == num_skinned_vertices);

						printf("We have skinning data!\n");

						skinned_vertices = (SkinnedVertex*) malloc(sizeof(SkinnedVertex) * num_vertices);
						for (i32 vertex_idx = 0; vertex_idx < num_vertices; ++vertex_idx)
						{
							skinned_vertices[vertex_idx] = {
								.joint_indices = {	
									.X = (f32) flatbuffer_joint_indices->Get(vertex_idx * 4 + 0),
									.Y = (f32) flatbuffer_joint_indices->Get(vertex_idx * 4 + 1),
									.Z = (f32) flatbuffer_joint_indices->Get(vertex_idx * 4 + 2),
									.W = (f32) flatbuffer_joint_indices->Get(vertex_idx * 4 + 3),
								},
								.joint_weights = {
									.X = flatbuffer_joint_weights->Get(vertex_idx * 4 + 0),
									.Y = flatbuffer_joint_weights->Get(vertex_idx * 4 + 1),
									.Z = flatbuffer_joint_weights->Get(vertex_idx * 4 + 2),
									.W = flatbuffer_joint_weights->Get(vertex_idx * 4 + 3),
								},
							};

							//FCS TODO: PRINT
						}
					}

					u32 num_indices = 0;
					u32* indices = nullptr;
					if (auto flatbuffer_indices = object_mesh->indices())
					{
						num_indices = flatbuffer_indices->size();
						indices = (u32*) malloc(sizeof(u32) * num_indices);
						for (i32 indices_idx = 0; indices_idx < num_indices; ++indices_idx)
						{
							indices[indices_idx] = flatbuffer_indices->Get(indices_idx);
						}
					}

					u32 num_material_indices = 0;
					i32* material_indices = nullptr;
					if (auto flatbuffer_material_ids = object_mesh->material_ids())
					{
						num_material_indices = flatbuffer_material_ids->size();
						material_indices = (i32*) malloc(sizeof(i32) * num_material_indices);
						for (i32 material_id_idx = 0; material_id_idx < num_material_indices; ++material_id_idx)
						{
		   					const int material_id = flatbuffer_material_ids->Get(material_id_idx);
							if (!state.material_id_to_index.contains(material_id))
							{
								printf("\tFailed to find material with id: %i\n", material_id);
								continue;
							}
							material_indices[material_id_idx] = state.material_id_to_index[material_id];
						}
					}

					// Set Mesh Data on Game Object
					if (num_vertices > 0 && num_indices > 0)
					{
						const MeshInitData mesh_init_data = {
							.num_indices = num_indices,
							.indices = indices,

							.num_vertices = num_vertices,
							.vertices = vertices,
							.skinned_vertices = skinned_vertices,

							.num_material_indices = num_material_indices,
							.material_indices = material_indices,
						};
						game_object.mesh = make_mesh(mesh_init_data);
						game_object.has_mesh = true;
					}
				}

				if (auto object_light = object->light())
				{
					LightType light_type = (LightType) object_light->type();

					game_object.has_light = true;
					game_object.light = (Light){
						.type = light_type,
						.color = flatbuffer_helpers::to_hmm_vec3(object_light->color()),
					};

					switch (game_object.light.type)
					{
						case LightType::Point:
						{
							auto point_light = object_light->point_light();
							assert(point_light);
							game_object.light.point = (PointLight) {
							.power = point_light->power(),
							};
							break;
						}
						case LightType::Spot:
						{
							auto spot_light = object_light->spot_light();
							assert(spot_light);
							game_object.light.spot = (SpotLight) {
								.power = spot_light->power(),
								.beam_angle = spot_light->beam_angle(),
								.edge_blend = spot_light->edge_blend(),
							};
							break;
						}
						case LightType::Sun:
						{
							auto sun_light = object_light->sun_light();
							assert(sun_light);
							game_object.light.sun = (SunLight) {
								.power = sun_light->power(),
								.cast_shadows = sun_light->cast_shadows(),
							};
							break;
						}
						case LightType::Area:
						{
							//FCS TODO:
							break;
						}
						default:
							printf("\t\tUnsupported Light Type\n");
							exit(0);
					}
				}

				if (auto object_rigid_body = object->rigid_body())
				{
					game_object.has_rigid_body = true;
					game_object.rigid_body = (RigidBody){
						.is_dynamic = object_rigid_body->is_dynamic(),
						.mass = object_rigid_body->mass(),
						.jolt_body = nullptr,
					};
				}

				// Custom gameplay components we've specified on our blender objects
				if (auto object_components = object->components())
				{
					i32 num_components = object_components->size();
					for (i32 component_idx = 0; component_idx < num_components; ++component_idx)
					{
						auto component_container = object_components->Get(component_idx);
						if (!component_container)  { continue; }
						auto component = component_container->value();
						if (!component) { continue; }

						auto component_type = component_container->value_type();
						switch (component_type)
						{
							case Blender::LiveLink::GameplayComponent_GameplayComponentCharacter:
							{
								using Blender::LiveLink::GameplayComponentCharacter;
								const GameplayComponentCharacter* character_component = reinterpret_cast<const GameplayComponentCharacter*>(component);

								printf("\t\tCharacter Component\n");
								printf("\t\t\t Player Controlled: %s\n", character_component->player_controlled() ? "true" : "false");
								printf("\t\t\t Move Speed: %f\n", character_component->move_speed());

								CharacterSettings character_settings = {
									.initial_location = game_object.current_transform.location,
									.initial_rotation = game_object.current_transform.rotation,
									.player_controlled = character_component->player_controlled(),
									.move_speed = character_component->move_speed(),
									.jump_speed = character_component->jump_speed(),
								};
								object_add_character(game_object, character_settings);

								if (character_settings.player_controlled)
								{
									state.player_character_id = game_object.unique_id;	
								}
								break;
							}
							case Blender::LiveLink::GameplayComponent_GameplayComponentCameraControl:
							{
								using Blender::LiveLink::GameplayComponentCameraControl;
								const GameplayComponentCameraControl* cam_control_component = reinterpret_cast<const GameplayComponentCameraControl*>(component);

								const float cam_control_follow_distance = cam_control_component->follow_distance();
								const float cam_control_follow_speed = cam_control_component->follow_speed();

								printf("\t\tCamera Control Component\n");
								printf("\t\t\t Follow Distance: %f\n", cam_control_follow_distance);
								printf("\t\t\t Follow Speed: %f\n", cam_control_follow_speed);

								HMM_Vec3 initial_location = camera_control_get_desired_location(
									game_object.current_transform.location.XYZ, 
									quat_forward(game_object.current_transform.rotation),
									cam_control_follow_distance
								);
								HMM_Vec3 object_location = game_object.current_transform.location.XYZ;
								HMM_Vec3 initial_direction = HMM_NormV3(object_location - initial_location);

								CameraControlSettings cam_control_settings = {
									.initial_location = initial_location,
									.initial_direction = initial_direction,
									.follow_distance = cam_control_follow_distance,
									.follow_speed = cam_control_follow_speed,
								};
								object_add_camera_control(game_object, cam_control_settings);

								state.camera_control_id = game_object.unique_id;
								break;
							}
							default:
								assert(false);
						}
					}
				}

				// Send updated object data to main thread
				state.updated_objects.send(game_object);
			}

			state.blender_data_loaded = true;
		}

		if (auto deleted_object_uids = update->deleted_object_uids())
		{
			for (i32 deleted_object_uid : *deleted_object_uids)
			{
				state.deleted_objects.send(deleted_object_uid);
			}
		}	

		if (update->reset())
		{
			state.reset.send(true);
		}
	}
}

// Live Link Function. Runs on its own thread
void live_link_thread_function()
{
	socket_lib_init();

	// Init socket we'll use to talk to blender
	struct addrinfo hints, *res;
	// first, load up address structs with getaddrinfo():

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	//FCS TODO: Store magic IP and Port numbers in some shared file
	const char* HOST = "127.0.0.1";
	const char* PORT = "65432";
	getaddrinfo(HOST, PORT, &hints, &res);
	
	// make a socket
	state.blender_socket = socket_open(res->ai_family, res->ai_socktype, res->ai_protocol);

	// Allow us to reuse address and port
	socket_set_reuse_addr_and_port(state.blender_socket, true);

	// bind our socket
	SOCKET_OP(bind(state.blender_socket, res->ai_addr, res->ai_addrlen));

	const i32 backlog = 1;
	SOCKET_OP(listen(state.blender_socket, backlog));

	// accept connections from blender 
	struct sockaddr_storage their_addr;
	socklen_t addr_size = sizeof their_addr;
	do
	{
		state.connection_socket = accept(state.blender_socket, (struct sockaddr *) &their_addr, &addr_size);
	}
	while(!socket_is_valid(state.connection_socket) && state.game_running);

	// set recv timeout
	struct timeval recv_timeout = {
		.tv_sec = 1,
		.tv_usec = 0
	};
	socket_set_recv_timeout(state.connection_socket, recv_timeout);

	// infinite recv loop
	while (state.game_running)
	{
		StretchyBuffer<u8> flatbuffer_data; 

		int current_bytes_read = 0;
		int total_bytes_read = 0;
		int packets_read = 0;
		optional<flatbuffers::uoffset_t> flatbuffer_size;
		do 
		{
			const size_t buffer_len = 4096; 
			u8 buffer[buffer_len];
			const int flags = 0;
			current_bytes_read = socket_recv(state.connection_socket, buffer, buffer_len, flags);

			// Less than zero is an error
			if (current_bytes_read < 0)
			{
				int last_error = socket_get_last_error();
				if (	last_error == socket_error_again()
					|| 	last_error == socket_error_would_block() 
					|| 	last_error == socket_error_timed_out())
				{
					current_bytes_read = 0;
					continue;
				}
				else
				{
					printf("recv_error: %i\n", last_error);
					exit(0);
				}
			}

			// No bytes read this iteration. Try again
			if (current_bytes_read == 0)
		  	{
				continue;
			}

			// current_bytes_read > 0, we've got data!
			if (current_bytes_read > 0)
		  	{
				// Flatbuffer size will be prefixed to flatbuffer data. Set it when we encounter it
				if (!flatbuffer_size)
				{
					assert(current_bytes_read >= sizeof(flatbuffers::uoffset_t));
					flatbuffer_size = *(flatbuffers::uoffset_t*)(buffer);
				}

				total_bytes_read += current_bytes_read;
				i32 next_idx = flatbuffer_data.length(); 
				//arraddn(flatbuffer_data, current_bytes_read);
				flatbuffer_data.add_uninitialized(current_bytes_read);
				memcpy(&flatbuffer_data[next_idx], buffer, current_bytes_read);
				++packets_read;	
			}
		}
		while (state.game_running && (current_bytes_read == 0 || (flatbuffer_size && total_bytes_read < flatbuffer_size.value())));
	
		printf("We've got some data! Data Length: %td Packets Read: %i\n", flatbuffer_data.length(), packets_read);

		parse_flatbuffer_data(flatbuffer_data);
	}

	printf("Shutting down sockets\n");

	socket_close(state.connection_socket);
	socket_close(state.blender_socket);

	socket_lib_quit();
}

void handle_resize()
{
	int new_width = sapp_width();
    int new_height = sapp_height();

	if (new_width != state.width || new_height != state.height)
	{
		state.width = new_width;
		state.height = new_height;
	
		const int render_pass_count = (int) ERenderPass::COUNT;
		for (i32 pass_index = 0; pass_index < render_pass_count; ++pass_index)
		{
			state.render_passes[pass_index].handle_resize(state.width, state.height);
		}
	}
}

//FCS TODO: BEGIN Testing Cubemap
Cubemap test_cubemap;
//FCS TODO: END Testing Cubemap

void init(void)
{
	jolt_init();

	// Init sokol graphics
    sg_setup((sg_desc) {
		.buffer_pool_size = 4096,
		.image_pool_size = 4096,
		//.sampler_pool_size = 1024,
		.attachments_pool_size = 128,
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });

	// Create Default Image
	HMM_Vec4 default_image_data[1] = { HMM_V4(0,0,0,1) };
	GpuImageDesc default_image_desc = {
		.type = SG_IMAGETYPE_2D,
		.width = 1,
		.height = 1,
		.pixel_format = SG_PIXELFORMAT_RGBA32F,
		.data = (u8*) &default_image_data,
	};
	state.default_image = GpuImage(default_image_desc);

	// FCS TODO: BEGIN Testing Cubemap rendering
	const CubemapDesc cubemap_desc = {
		.size = 1024,
		.location = HMM_V3(0,0,0),
	};
	test_cubemap = Cubemap(cubemap_desc);
	// FCS TODO: END Testing Cubemap rendering

	#if WITH_DEBUG_UI
	// Init sokol imgui integration
	simgui_setup((simgui_desc_t) {
		.ini_filename = "bin/imgui.ini",
	});
	#endif // WITH_DEBUG_UI

	// Check for Storage Buffer Support
	if (!sg_query_features().compute)
	{
		printf("Sokol Gfx Error: Compute/Storage Buffers are Required\n");
		exit(0);
	}

	// Spin up a thread that blocks until we receive our init event, and then listens for updates
	state.live_link_thread = std::thread(live_link_thread_function);
	
	// setup sokol-debugtext
    sdtx_setup((sdtx_desc_t){
        .fonts = {
            [FONT_KC853] = sdtx_font_kc853(),
            [FONT_KC854] = sdtx_font_kc854(),
            [FONT_Z1013] = sdtx_font_z1013(),
            [FONT_CPC]   = sdtx_font_cpc(),
            [FONT_C64]   = sdtx_font_c64(),
            [FONT_ORIC]  = sdtx_font_oric()
        },
        .logger.func = slog_func,
    });

	state.sampler = sg_make_sampler((sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });

	sg_swapchain swapchain = sglue_swapchain();

	// Init main geometry pass
	get_render_pass(ERenderPass::Geometry).init(GeometryPass::make_render_pass_desc(swapchain.depth_format));
	
	RenderPassDesc ssao_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(ssao_ssao_shader_desc(sg_query_backend())),
			.cull_mode = SG_CULLMODE_NONE,
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.label = "ssao-pipeline",
		},
		.num_outputs = 1,
		.outputs[0] = {
			.pixel_format = swapchain.color_format,
			.load_action = SG_LOADACTION_CLEAR,
			.store_action = SG_STOREACTION_STORE,
			.clear_value = {0.0, 0.0, 0.0, 0.0},
		},
	};
	get_render_pass(ERenderPass::SSAO).init(ssao_pass_desc);
	
	{	//SSAO Noise Texture
		std::uniform_real_distribution<float> randomFloats(0.0, 1.0); // random floats between [0.0, 1.0]
		std::default_random_engine generator;
		HMM_Vec4 ssao_noise[SSAO_TEXTURE_SIZE];
		for (u32 i = 0; i < SSAO_TEXTURE_SIZE; ++i)
		{
			ssao_noise[i] = HMM_V4(
				randomFloats(generator) * 2.0f - 1.0f,
				randomFloats(generator) * 2.0f - 1.0f, 
				0.0f,
				0.0f
			);
		}

		sg_image_desc ssao_noise_desc = {
			.width = SSAO_TEXTURE_WIDTH,
			.height = SSAO_TEXTURE_WIDTH,
			.pixel_format = SG_PIXELFORMAT_RGBA32F,
			.sample_count = 1, 
			.data.subimage[0][0].ptr = &ssao_noise,
			.data.subimage[0][0].size = SSAO_TEXTURE_SIZE * sizeof(HMM_Vec4),
			.label = "ssao_noise_texture"
		};

		state.ssao_noise_texture = sg_make_image(ssao_noise_desc);

		// Init ssao_fs_params
		state.ssao_fs_params = {
			.screen_size = HMM_V2(sapp_widthf(), sapp_heightf()),	
		};

		//SSAO Kernel
		for (u32 i = 0; i < SSAO_KERNEL_SIZE; ++i)
		{
			HMM_Vec3 sample = HMM_V3(
				randomFloats(generator) * 2.0f - 1.0f,
				randomFloats(generator) * 2.0f - 1.0f,
				randomFloats(generator)
			);
			sample = HMM_NormV3(sample);
			sample *= randomFloats(generator);

			// scale samples s.t. they're more aligned to center of kernel
			float scale = float(i) / (float) SSAO_KERNEL_SIZE;
			scale = HMM_Lerp(0.1f, scale * scale, 1.0f);
			sample *= scale;

			state.ssao_fs_params.kernel_samples[i] = HMM_V4(sample.X, sample.Y, sample.Z, 0.0);
		}
	}

	RenderPassDesc ssao_blur_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(blur_blur_shader_desc(sg_query_backend())),
			.cull_mode = SG_CULLMODE_NONE,
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.label = "blur-pipeline",
		},
		.num_outputs = 1,
		.outputs[0] = {
			.pixel_format = swapchain.color_format,
			.load_action = SG_LOADACTION_LOAD,
			.store_action = SG_STOREACTION_STORE,
		},
	};
	get_render_pass(ERenderPass::SSAO_Blur).init(ssao_blur_pass_desc);

	RenderPassDesc lighting_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(lighting_lighting_shader_desc(sg_query_backend())),
			.cull_mode = SG_CULLMODE_NONE,
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.label = "lighting-pipeline",
		},
		.num_outputs = 1,
		.outputs[0] = {
			.pixel_format = swapchain.color_format,
			.load_action = SG_LOADACTION_LOAD,
			.store_action = SG_STOREACTION_STORE,
		},
	};
	get_render_pass(ERenderPass::Lighting).init(lighting_pass_desc);

	RenderPassDesc dof_blur_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(blur_blur_shader_desc(sg_query_backend())),
			.cull_mode = SG_CULLMODE_NONE,
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.label = "dof-blur-pipeline",
		},
		.num_outputs = 1,
		.outputs[0] = {
			.pixel_format = swapchain.color_format,
			.load_action = SG_LOADACTION_LOAD,
			.store_action = SG_STOREACTION_STORE,
		},
	};
	get_render_pass(ERenderPass::DOF_Blur).init(dof_blur_pass_desc);

	RenderPassDesc dof_combine_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(dof_combine_dof_combine_shader_desc(sg_query_backend())),
			.cull_mode = SG_CULLMODE_NONE,
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.label = "dof-combine-pipeline",
		},
		.num_outputs = 1,
		.outputs[0] = {
			.pixel_format = swapchain.color_format,
			.load_action = SG_LOADACTION_LOAD,
			.store_action = SG_STOREACTION_STORE,
		},
	};
	get_render_pass(ERenderPass::DOF_Combine).init(dof_combine_pass_desc);

	RenderPassDesc tonemapping_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(tonemapping_tonemapping_shader_desc(sg_query_backend())),
			.cull_mode = SG_CULLMODE_NONE,
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.label = "tonemapping-pipeline",
		},
		.num_outputs = 1,
		.outputs[0] = {
			.pixel_format = swapchain.color_format,
			.load_action = SG_LOADACTION_LOAD,
			.store_action = SG_STOREACTION_STORE,
		},
	};
	get_render_pass(ERenderPass::Tonemapping).init(tonemapping_pass_desc);

	RenderPassDesc debug_text_pass_desc = {
		// Don't set optional pipeline_desc
		.num_outputs = 1,
		.outputs[0] = {
			.pixel_format = swapchain.color_format,
			.load_action = SG_LOADACTION_CLEAR,
			.store_action = SG_STOREACTION_STORE,
			.clear_value = {0.0, 0.0, 0.0, 0.0},
		},
		.depth_output = {
			.pixel_format = swapchain.depth_format,
		},
	};
	get_render_pass(ERenderPass::DebugText).init(debug_text_pass_desc);

	RenderPassDesc copy_to_swapchain_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(overlay_texture_overlay_texture_shader_desc(sg_query_backend())),
			.cull_mode = SG_CULLMODE_NONE,
			.depth = {
				.pixel_format = swapchain.depth_format,
				.compare = SG_COMPAREFUNC_ALWAYS,
				.write_enabled = false,
			},
			.label = "copy-to-swapchain-pipeline",
		},
		.depth_output = {
			.pixel_format = swapchain.depth_format,
		},
		.type = ERenderPassType::Swapchain,
	};
	get_render_pass(ERenderPass::CopyToSwapchain).init(copy_to_swapchain_pass_desc);

	handle_resize();

	if (state.init_file)
	{
		if (FILE* file = fopen(state.init_file->c_str(), "rb"))
		{
			fseek(file, 0, SEEK_END);
    		long file_size = ftell(file);
			rewind(file);

			assert(file_size > 0);

			StretchyBuffer<u8> flatbuffer_data;
			flatbuffer_data.add_uninitialized(file_size);

			size_t bytes_read = fread(flatbuffer_data.data(), 1, file_size, file);
			assert(bytes_read == (size_t) file_size);
			parse_flatbuffer_data(flatbuffer_data);			
		}
	}
}

struct AppState {
	bool keycodes[SAPP_MAX_KEYCODES];

	static const i32 num_mouse_buttons = 3;
	bool mouse_buttons[num_mouse_buttons];

	HMM_Vec2 mouse_delta;

	bool is_mouse_locked = false;
} app_state;

bool is_key_pressed(sapp_keycode keycode)
{
	assert((i32)keycode < SAPP_MAX_KEYCODES);
	return app_state.keycodes[keycode];
}

bool is_mouse_down(sapp_mousebutton in_mouse_button)
{
	assert(in_mouse_button < app_state.num_mouse_buttons);
	return app_state.mouse_buttons[in_mouse_button];
}

HMM_Vec2 get_mouse_delta()
{
	return app_state.mouse_delta;
}

void reset_mouse_delta()
{
	app_state.mouse_delta = HMM_V2(0.f,0.f);
}

bool is_mouse_locked()
{
	return app_state.is_mouse_locked;
}

void set_mouse_locked(bool in_locked)
{
	app_state.is_mouse_locked = in_locked;
	sapp_lock_mouse(app_state.is_mouse_locked);
}

void frame(void)
{
	double delta_time = sapp_frame_duration();

	DEBUG_UI(
		simgui_new_frame((simgui_frame_desc_t){
			.width = state.width,
			.height = state.height, 
			.delta_time = delta_time,
			//.dpi_scale = ,
		});
	);

	// Receive Any Updated Objects
	while (optional<Object> received_updated_object = state.updated_objects.receive())
	{
		Object& updated_object = *received_updated_object;
		i32 updated_object_uid = updated_object.unique_id;

		printf("Updating Object. UID: %i\n", updated_object_uid);

		// Cleanup old object
		if (state.objects.contains(updated_object_uid))
		{	
			Object& existing_object = state.objects[updated_object_uid];
			object_cleanup(existing_object);
		}

		if (updated_object.has_light)
		{
			state.needs_light_data_update = true;
		}

		if (updated_object.has_rigid_body)
		{
			object_add_jolt_body(updated_object);
		}

		state.objects[updated_object_uid] = updated_object;
	}

	// Receive Any Deleted Objects
	while (optional<i32> received_deleted_object = state.deleted_objects.receive())
	{
		i32 deleted_object_uid = *received_deleted_object;
		if (state.objects.contains(deleted_object_uid))
		{
			printf("Removing object. UID: %i\n", deleted_object_uid);
			Object& object_to_delete = state.objects[deleted_object_uid];

			if (object_to_delete.has_light)
			{
				state.needs_light_data_update = true;
			}
			
			object_cleanup(object_to_delete);
			state.objects.erase(deleted_object_uid);
		}
	}

	// Receive any Reset Messages
	while(optional<bool> received_reset = state.reset.receive())
	{
		state.blender_data_loaded = false;
		state.needs_light_data_update = true;

		for (auto& [unique_id, object] : state.objects)
		{
			if (object.has_rigid_body)
			{
				object_remove_jolt_body(object);
			}

			object_cleanup_gpu_resources(object);
		}

		state.objects.clear();
		state.camera_control_id.reset();
		state.player_character_id.reset();

		reset_materials();
		reset_images();
	}

	// Space Bar + Left Control Starts/Stops simulation 
	DEFINE_TOGGLE_TWO_KEYS(state.is_simulating, SAPP_KEYCODE_SPACE, SAPP_KEYCODE_LEFT_CONTROL);

	#if WITH_DEBUG_UI
	// Control + I toggles imgui debug window
	DEFINE_TOGGLE_TWO_KEYS(g_show_imgui, SAPP_KEYCODE_I, SAPP_KEYCODE_LEFT_CONTROL);
	#endif // WITH_DEBUG_UI

	// D + Left Control toggle debug camera
	DEFINE_EVENT_TWO_KEYS(SAPP_KEYCODE_D, SAPP_KEYCODE_LEFT_CONTROL,
		if (!state.debug_camera_active) state.debug_camera = get_active_camera();
		state.debug_camera_active = !state.debug_camera_active;
	);

	// Reset State
	DEFINE_EVENT_TWO_KEYS(SAPP_KEYCODE_R, SAPP_KEYCODE_LEFT_CONTROL,
		// Reset object transforms and recreate physics state
		for (auto& [unique_id, object] : state.objects)
		{
			object.current_transform = object.initial_transform;

			if (object.has_rigid_body)
			{
				object_reset_jolt_body(object);
			}
		}
	);

	DEBUG_UI(
		ImGui::Begin("DEBUG");
		if (ImGui::CollapsingHeader("FPS", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Text("frame time: %f ms", delta_time * 1000.f);
			ImGui::Text("FPS: %f", 1.0 / delta_time);
			ImGui::Spacing();
		}
	
		if (ImGui::CollapsingHeader("Rendering Features", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Checkbox("SSAO", &state.ssao_enable);
			ImGui::Checkbox("Depth-of-Field", &state.dof_enable);
		}
	);

	if (state.is_simulating)
	{
		//FCS TODO: Game logic update here

		// Jolt Physics Update
		jolt_update(delta_time);
	}
	
	// Debug Camera Control
	if (state.debug_camera_active && !is_key_pressed(SAPP_KEYCODE_LEFT_CONTROL))
	{
		Camera& camera = state.debug_camera;	
		const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));

		float move_speed = 10.0f * delta_time;
		if (is_key_pressed(SAPP_KEYCODE_LEFT_SHIFT))
		{
			move_speed *= 5.0f;
		}

		if (is_key_pressed(SAPP_KEYCODE_W))
		{
			camera.location += camera.forward * move_speed;	
		}
		if (is_key_pressed(SAPP_KEYCODE_S))
		{
			camera.location -= camera.forward * move_speed;	
		}	
		if (is_key_pressed(SAPP_KEYCODE_D))
		{
			camera.location += camera_right * move_speed;	
		}
		if (is_key_pressed(SAPP_KEYCODE_A))
		{
			camera.location -= camera_right * move_speed;	
		}
		if (is_key_pressed(SAPP_KEYCODE_E))
		{
			camera.location += camera.up * move_speed;	
		}
		if (is_key_pressed(SAPP_KEYCODE_Q))
		{
			camera.location -= camera.up * move_speed;	
		}

		if (is_mouse_locked())
		{
			const float look_speed = 1.0f * delta_time;
			const HMM_Vec2 mouse_delta = get_mouse_delta();
			
			const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));
			camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera.up, -mouse_delta.X * look_speed));
			camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera_right, -mouse_delta.Y * look_speed));
		}
	}

	// Player Camera Control
	if (state.camera_control_id && !state.debug_camera_active)
	{	
		Object& camera_control_object = state.objects[*state.camera_control_id];
		CameraControl& camera_control = camera_control_object.camera_control;
		Camera& camera = camera_control.camera;

		if (is_mouse_locked())
		{
			//FCS TODO: Add max angle property (angle above XY plane) that we can rotate camera
			//FCS TODO: Add rotation speed property to camera control component
			const float look_speed = 1.0f * delta_time;
			const HMM_Vec2 mouse_delta = get_mouse_delta();

			// Get current target at old forward vector
			const HMM_Vec3 camera_old_target = camera.location + camera.forward * camera_control.follow_distance;
			
			const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));
			camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera.up, -mouse_delta.X * look_speed));
			camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera_right, -mouse_delta.Y * look_speed));

			// Use old target and new forward vector to get our rotated desired location
			camera_control.camera.location = camera_control_get_desired_location(
				camera_old_target, 
				camera.forward,
				camera_control.follow_distance
			);
		}

		HMM_Vec3 desired_location = camera_control_get_desired_location(
			camera_control_object.current_transform.location.XYZ, 
			camera.forward,
			camera_control.follow_distance
		);
		camera_control.camera.location = HMM_LerpV3(
			camera.location, 
			camera_control.follow_speed * delta_time, 
			desired_location
		);
	}

	// Player Character Control
	if (state.player_character_id && !state.debug_camera_active)
	{
		const Camera& camera = get_active_camera();
		const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));

		Object& player_character_object = state.objects[*state.player_character_id];
		Character& player_character_state = player_character_object.character;
		float character_move_speed = player_character_state.settings.move_speed * delta_time;

		HMM_Vec3 projected_cam_forward = HMM_NormV3(vec3_plane_projection(camera.forward, UnitVectors::Up));
		HMM_Vec3 projected_cam_right = HMM_NormV3(vec3_plane_projection(camera_right, UnitVectors::Up));

		if (is_key_pressed(SAPP_KEYCODE_LEFT_SHIFT))
		{
			character_move_speed *= 3.0f;
		}

		HMM_Vec3 move_vec = HMM_V3(0,0,0);
		if (is_key_pressed(SAPP_KEYCODE_W))
		{
			move_vec += projected_cam_forward * character_move_speed;	
		}
		if (is_key_pressed(SAPP_KEYCODE_S))
		{
			move_vec -= projected_cam_forward * character_move_speed;	
		}	
		if (is_key_pressed(SAPP_KEYCODE_D))
		{
			move_vec += projected_cam_right * character_move_speed;	
		}
		if (is_key_pressed(SAPP_KEYCODE_A))
		{
			move_vec -= projected_cam_right * character_move_speed;	
		}

		bool jump = is_key_pressed(SAPP_KEYCODE_SPACE);

		character_move(player_character_state, move_vec, jump);
	}

	// Rendering
	{
		test_cubemap.render(state);

		DEBUG_UI(
			const i32 num_images = NUM_CUBE_FACES;
			if (ImGui::CollapsingHeader("Cubemap Viewer"))
			{
				static i32 cube_face_idx = 0;
				ImGui::SliderInt(
					"Cube Face Index", 
					&cube_face_idx, 
					0, NUM_CUBE_FACES - 1, 
					"%d", ImGuiSliderFlags_ClampOnInput
				);

				static i32 color_output_idx = 0;
				ImGui::SliderInt(
					"Colour Output Index", 
					&color_output_idx, 
					0, 3, 
					"%d", ImGuiSliderFlags_ClampOnInput
				);

				RenderPass& geometry_pass = test_cubemap.geometry_passes[cube_face_idx];
				sg_image color_output = geometry_pass.color_outputs[color_output_idx];

				ImVec2 size = ImVec2(256, 256);
				ImGui::Image(simgui_imtextureid(color_output), size);
			}
		);

		{
			// Run initially, and then only if our updates from blender encounter a light
			if (state.needs_light_data_update)
			{
				state.needs_light_data_update = false;

				const i32 MAX_LIGHTS_PER_TYPE = 1024;

				// Init point_lights_buffer if we haven't already
				if (!state.point_lights_buffer.is_gpu_buffer_valid())
				{
					state.point_lights_buffer = GpuBuffer((GpuBufferDesc<lighting_PointLight_t>){
						.data = nullptr,
						.size = sizeof(lighting_PointLight_t) * MAX_LIGHTS_PER_TYPE,
						.usage = {
							.storage_buffer = true,
							.stream_update = true,
						},
						.label = "point_lights",
					});
				}

				// Init spot_lights_buffer if we haven't already
				if (!state.spot_lights_buffer.is_gpu_buffer_valid())
				{
					state.spot_lights_buffer = GpuBuffer((GpuBufferDesc<lighting_SpotLight_t>){
						.data = nullptr,
						.size = sizeof(lighting_SpotLight_t) * MAX_LIGHTS_PER_TYPE,
						.usage = {
							.storage_buffer = true,
							.stream_update = true,
						},
						.label = "spot_lights",
					});
				}

				// Init sun_lights_buffer if we haven't already
				if (!state.sun_lights_buffer.is_gpu_buffer_valid())
				{
					state.sun_lights_buffer = GpuBuffer((GpuBufferDesc<lighting_SunLight_t>){
						.data = nullptr,
						.size = sizeof(lighting_SunLight_t) * MAX_LIGHTS_PER_TYPE,
						.usage = {
							.storage_buffer = true,
							.stream_update = true,
						},
						.label = "sun_lights",
					});
				}

				state.point_lights.reset();
				state.spot_lights.reset();
				state.sun_lights.reset();

				for (auto const& [unique_id, object] : state.objects)
				{
					if (object.has_light)
					{
						const Light& light = object.light;
						switch(light.type)
						{
							case LightType::Point:
							{
								if (state.point_lights.length() >= MAX_LIGHTS_PER_TYPE)
								{
									printf("Exceeded Max Number of Point Lights (%i)\n", MAX_LIGHTS_PER_TYPE);
									continue;
								}

								lighting_PointLight_t new_point_light = {};
								const Transform& transform = object.current_transform;
								memcpy(&new_point_light.location, &transform.location, sizeof(float) * 4);
								memcpy(&new_point_light.color, &object.light.color, sizeof(float) * 3);
								new_point_light.color[3] = 1.0; // Force alpha to 1.0
								new_point_light.power = object.light.point.power;
								state.point_lights.add(new_point_light);
								break;
							}
							case LightType::Spot:
							{
								if (state.spot_lights.length() >= MAX_LIGHTS_PER_TYPE)
								{
									printf("Exceeded Max Number of Spot Lights (%i)\n", MAX_LIGHTS_PER_TYPE);
									continue;
								}

								lighting_SpotLight_t new_spot_light = {};
								const Transform& transform = object.current_transform;
								memcpy(&new_spot_light.location, &transform.location, sizeof(float) * 4);
								memcpy(&new_spot_light.color, &object.light.color, sizeof(float) * 3);
								new_spot_light.color[3] = 1.0; // Force alpha to 1.0

								HMM_Vec3 spot_light_dir = HMM_NormV3(HMM_RotateV3Q(HMM_V3(0,0,-1), transform.rotation));
								memcpy(&new_spot_light.direction, &spot_light_dir, sizeof(float) * 3);

								new_spot_light.spot_angle_radians = object.light.spot.beam_angle / 2.0f;	
								new_spot_light.power = object.light.spot.power;
								new_spot_light.edge_blend = object.light.spot.edge_blend;
								state.spot_lights.add(new_spot_light);
								break;
							}
							case LightType::Sun:
							{
								if (state.sun_lights.length() >= MAX_LIGHTS_PER_TYPE)
								{
									printf("Exceeded Max Number of Sun Lights (%i)\n", MAX_LIGHTS_PER_TYPE);
									continue;
								}

								lighting_SunLight_t new_sun_light = {};
								const Transform& transform = object.current_transform;
								memcpy(&new_sun_light.location, &transform.location, sizeof(float) * 4);
								memcpy(&new_sun_light.color, &object.light.color, sizeof(float) * 3);
								new_sun_light.color[3] = 1.0; // Force alpha to 1.0

								HMM_Vec3 spot_light_dir = HMM_NormV3(HMM_RotateV3Q(HMM_V3(0,0,-1), transform.rotation));
								memcpy(&new_sun_light.direction, &spot_light_dir, sizeof(float) * 3);

								new_sun_light.power = object.light.sun.power;
								new_sun_light.cast_shadows = object.light.sun.cast_shadows;
								state.sun_lights.add(new_sun_light);
								break;
							}
							case LightType::Area:
							{
								//FCS TODO: 
								break;
							}
							default: break;
						}
					}
				}

				state.lighting_fs_params.num_point_lights = state.point_lights.length();
				printf("Num Point Lights: %i\n", state.lighting_fs_params.num_point_lights);
				if (state.lighting_fs_params.num_point_lights > 0)
				{
					state.point_lights_buffer.update_gpu_buffer(
						(sg_range){
							.ptr = state.point_lights.data(),
							.size = sizeof(lighting_PointLight_t) * state.lighting_fs_params.num_point_lights,
						}
					);
				}

				state.lighting_fs_params.num_spot_lights = state.spot_lights.length();	
				printf("Num Spot Lights: %i\n", state.lighting_fs_params.num_spot_lights);
				if (state.lighting_fs_params.num_spot_lights > 0)
				{
					state.spot_lights_buffer.update_gpu_buffer(
						(sg_range){
							.ptr = state.spot_lights.data(),
							.size = sizeof(lighting_SpotLight_t) * state.lighting_fs_params.num_spot_lights,
						}
					);
				}

				state.lighting_fs_params.num_sun_lights = state.sun_lights.length();
				printf("Num Sun Lights: %i\n", state.lighting_fs_params.num_sun_lights);
				if (state.lighting_fs_params.num_sun_lights > 0)
				{
					state.sun_lights_buffer.update_gpu_buffer(
						(sg_range){
							.ptr = state.sun_lights.data(),
							.size = sizeof(lighting_SunLight_t) * state.lighting_fs_params.num_sun_lights,
						}
					);
				}
			}
		}

		// View + Projection matrix setup
		const float w = sapp_widthf();
		const float h = sapp_heightf();
		const float t = (float)(sapp_frame_duration() * 60.0);
		const float projection_near = 0.01f;
		const float projection_far = 10000.0f;
		HMM_Mat4 projection_matrix = HMM_Perspective_RH_NO(HMM_AngleDeg(60.0f), w/h, projection_near, projection_far);

		Camera& camera = get_active_camera();
		HMM_Vec3 target = camera.location + camera.forward * 10;
		HMM_Mat4 view_matrix = HMM_LookAt_RH(camera.location, target, camera.up);
		HMM_Mat4 view_projection_matrix = HMM_MulM4(projection_matrix, view_matrix);

		// Get our jolt body interface
		JPH::BodyInterface& body_interface = jolt_state.physics_system.GetBodyInterface();

		// Update Objects
		for (auto& [unique_id, object] : state.objects)
		{
			// For objects that simulate physics, copy their physics transforms into their uniform buffers
			object_copy_physics_transform(object, body_interface);

			if (object.storage_buffer_needs_update)
			{
				object.storage_buffer_needs_update = false;
				object_update_storage_buffer(object);
			}
		}

		{ // Geometry Pass 	
			get_render_pass(ERenderPass::Geometry).execute(
				[&]()
				{
				    geometry_vs_params_t vs_params;
					vs_params.view = view_matrix;
					vs_params.projection = projection_matrix;

					// Apply Vertex Uniforms
					sg_apply_uniforms(0, SG_RANGE(vs_params));

					// Get our jolt body interface
					JPH::BodyInterface& body_interface = jolt_state.physics_system.GetBodyInterface();

					// Cull objects
					CullResult cull_result = cull_objects(state.objects, view_projection_matrix);

					DEBUG_UI(
						if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_DefaultOpen))
						{
							ImGui::Text("Total Objects: %zu", state.objects.size());

							ImGui::SetNextItemOpen(true, ImGuiCond_Always);
							if (ImGui::TreeNode("CulledObjects", "Culled Objects: %i", cull_result.cull_count))
							{
								#if WITH_VERBOSE_CULL_RESULTS
								ImGui::Text("Non-Renderable: %i", cull_result.non_renderable_cull_count);
								ImGui::Text("Invisible: %i", cull_result.visibility_cull_count);
								ImGui::Text("Frustum Culled: %i", cull_result.frustum_cull_count);
								#else
								ImGui::Text("#define WITH_VERBOSE_CULL_RESULTS 1 for additional culling stats");
								#endif
								ImGui::TreePop();
							}

							ImGui::SetNextItemOpen(true, ImGuiCond_Always);
							if (ImGui::TreeNode("Lights", "Lights"))
							{
								ImGui::Text("Num Point Lights: %i", state.point_lights.length());
								ImGui::Text("Num Spot Lights:  %i", state.spot_lights.length());
								ImGui::Text("Num Sun Lights:   %i", state.sun_lights.length());
								ImGui::TreePop();
							}

						}
					);

					// Submit draw calls for culled objects
					for (auto& [unique_id, object_ptr] : cull_result.objects)
					{
						assert(object_ptr);
						Object& object = *object_ptr;

						if (object.has_mesh)
						{
							Mesh& mesh = object.mesh;

							int mesh_material_idx = mesh.material_indices[0];
							assert(mesh_material_idx >= 0);
							const geometry_Material_t& material = state.materials[mesh_material_idx]; 

							GpuImage& base_color_image = material.base_color_image_index >= 0 ? state.images[material.base_color_image_index] : state.default_image;
							GpuImage& metallic_image = material.metallic_image_index >= 0 ? state.images[material.metallic_image_index] : state.default_image;
							GpuImage& roughness_image = material.roughness_image_index >= 0 ? state.images[material.roughness_image_index] : state.default_image;
							GpuImage& emission_color_image = material.emission_color_image_index >= 0 ? state.images[material.emission_color_image_index] : state.default_image;

							sg_bindings bindings = {
								.vertex_buffers[0] = mesh.vertex_buffer.get_gpu_buffer(),
								.index_buffer = mesh.index_buffer.get_gpu_buffer(),
								.images = {
									[0] = base_color_image.get_gpu_image(),
									[1] = metallic_image.get_gpu_image(),
									[2] = roughness_image.get_gpu_image(),
									[3] = emission_color_image.get_gpu_image(),
								},
								.samplers[0] = state.sampler,
								.storage_buffers = {
									[0] = object.storage_buffer.get_gpu_buffer(),
									[1] = get_materials_buffer().get_gpu_buffer(),
								},
							};
							sg_apply_bindings(&bindings);
							sg_draw(0, mesh.index_count, 1);
						}
					}	
				}
			);
		}

		{ // SSAO
			get_render_pass(ERenderPass::SSAO).execute(
				[&]()
				{	
					state.ssao_fs_params.screen_size = HMM_V2(sapp_widthf(), sapp_heightf());
					state.ssao_fs_params.view = view_matrix;
					state.ssao_fs_params.projection = projection_matrix;
					state.ssao_fs_params.ssao_enable = state.ssao_enable;
					sg_apply_uniforms(0, SG_RANGE(state.ssao_fs_params));

					RenderPass& geometry_pass = get_render_pass(ERenderPass::Geometry);

					sg_bindings bindings = (sg_bindings){
						.images = {
							[0] = geometry_pass.color_outputs[1],	// geometry pass position
							[1] = geometry_pass.color_outputs[2],	// geometry pass normal
							[2] = state.ssao_noise_texture,			// ssao noise texture
						},
						.samplers[0] = state.sampler,
					};
					sg_apply_bindings(&bindings);

					sg_draw(0,6,1);
				}
			);
		}

		{ // SSAO Blur
			get_render_pass(ERenderPass::SSAO_Blur).execute(
				[&]()
				{	
					const blur_fs_params_t blur_fs_params = {
						.screen_size = HMM_V2(sapp_widthf(), sapp_heightf()),
						.blur_size = 4,
					};
					sg_apply_uniforms(0, SG_RANGE(blur_fs_params));

					sg_bindings bindings = (sg_bindings){
						.images[0] = get_render_pass(ERenderPass::SSAO).color_outputs[0],
						.samplers[0] = state.sampler,
					};

					sg_apply_bindings(&bindings);

					sg_draw(0,6,1);
				}
			);
		}

		{ // Lighting Pass
			get_render_pass(ERenderPass::Lighting).execute(
				[&]()
				{	
					state.lighting_fs_params.view_position = get_active_camera().location;

					// Apply Fragment Uniforms
					sg_apply_uniforms(0, SG_RANGE(state.lighting_fs_params));

					RenderPass& geometry_pass = get_render_pass(ERenderPass::Geometry);
					RenderPass& ssao_blur_pass = get_render_pass(ERenderPass::SSAO_Blur);

					sg_image color_texture = geometry_pass.color_outputs[0];
					sg_image position_texture = geometry_pass.color_outputs[1];
					sg_image normal_texture = geometry_pass.color_outputs[2];
					sg_image roughness_metallic_texture = geometry_pass.color_outputs[3];
					sg_image blurred_ssao_texture = ssao_blur_pass.color_outputs[0];

					sg_bindings bindings = {
						.images = {
							[0] = color_texture,
							[1] = position_texture,
							[2] = normal_texture,
							[3] = roughness_metallic_texture,
							[4] = blurred_ssao_texture,
						},
						.samplers[0] = state.sampler,
						.storage_buffers = {
							[0] = state.point_lights_buffer.get_gpu_buffer(),
							[1] = state.spot_lights_buffer.get_gpu_buffer(),
							[2] = state.sun_lights_buffer.get_gpu_buffer(),
						},
					};
					sg_apply_bindings(&bindings);

					sg_draw(0,6,1);
				}
			);
		}

		{ // DOF

			// Blurred lighting texture
			get_render_pass(ERenderPass::DOF_Blur).execute(
				[&]()
				{	
					const blur_fs_params_t blur_fs_params = {
						.screen_size = HMM_V2(sapp_widthf(), sapp_heightf()),
						.blur_size = 8,
					};
					sg_apply_uniforms(0, SG_RANGE(blur_fs_params));

					sg_bindings bindings = (sg_bindings){
						.images[0] = get_render_pass(ERenderPass::Lighting).color_outputs[0],
						.samplers[0] = state.sampler,
					};

					sg_apply_bindings(&bindings);

					sg_draw(0,6,1);
				}
			);

			// Mix blurred and unblurred texture based on camera distance
			get_render_pass(ERenderPass::DOF_Combine).execute(
				[&]()
				{	
					RenderPass& lighting_pass = get_render_pass(ERenderPass::Lighting);
					RenderPass& dof_blur_pass = get_render_pass(ERenderPass::DOF_Blur);
					RenderPass& geometry_pass = get_render_pass(ERenderPass::Geometry);

					const dof_combine_fs_params_t dof_combine_fs_params = {
						.cam_pos = camera.location,
						.min_distance = 50.0f,
						.max_distance = 100.0f,
						.dof_enabled = state.dof_enable,
					};
					sg_apply_uniforms(0, SG_RANGE(dof_combine_fs_params));

					sg_image position_texture = geometry_pass.color_outputs[1];

					sg_bindings bindings = (sg_bindings){
						.images[0] = lighting_pass.color_outputs[0], 
						.images[1] = dof_blur_pass.color_outputs[0],
						.images[2] = position_texture,
						.samplers[0] = state.sampler,
					};

					sg_apply_bindings(&bindings);

					sg_draw(0,6,1);
				}
			);	
		}

		{ // Tonemapping Pass
			get_render_pass(ERenderPass::Tonemapping).execute(
				[&]()
				{	
					RenderPass& dof_combine_pass = get_render_pass(ERenderPass::DOF_Combine);

					sg_bindings bindings = (sg_bindings){
						.images[0] = dof_combine_pass.color_outputs[0], 
						.samplers[0] = state.sampler,
					};

					sg_apply_bindings(&bindings);

					sg_draw(0,6,1);
				}
			);
		}

		{ // Debug Text
			get_render_pass(ERenderPass::DebugText).execute(
				[&]()
				{
					// Larger numbers scales down text
					const float text_scale = 0.5f;
					sdtx_canvas(sapp_width() * text_scale, sapp_height() * text_scale);
					sdtx_origin(1.0f, 2.0f);
					sdtx_home();

					if (!state.blender_data_loaded)
					{
						draw_debug_text(FONT_C64, "Waiting on data from Blender\n", 255,255,255);
					}

					if (state.debug_camera_active)
					{
						draw_debug_text(FONT_C64, "Debug Camera Active\n", 255,255,255);
					}

					if (!state.is_simulating)
					{
						draw_debug_text(FONT_C64, "Simulation Paused\n", 255,255,255);
					}

					sdtx_draw();
				}
			);
		}

		{ // Copy To Swapchain Pass	
			get_render_pass(ERenderPass::CopyToSwapchain).execute(
				[&]()
				{	
					RenderPass& tonemapping_pass = get_render_pass(ERenderPass::Tonemapping);
					RenderPass& debug_text_pass = get_render_pass(ERenderPass::DebugText);

					// This can be overridden by the debug image viewer below
					sg_image image_to_copy_to_swapchain = tonemapping_pass.color_outputs[0];

					DEBUG_UI(
						const i32 num_images = state.images.length();
						if (num_images > 0)
						{
							if (ImGui::CollapsingHeader("Debug Image Viewer"))
							{
								ImGui::Checkbox("Fullscreen", &state.enable_debug_image_fullscreen);
								ImGui::SliderInt(
									"Image Index", 
									&state.debug_image_index, 
									0, num_images - 1, 
									"%d", ImGuiSliderFlags_ClampOnInput
								);

								GpuImage& image = state.images[state.debug_image_index];
								ImVec2 size = ImVec2(256, 256);
								ImGui::Image(simgui_imtextureid(image.get_gpu_image()), size);
							}

							if (state.enable_debug_image_fullscreen)
							{
								image_to_copy_to_swapchain = state.images[state.debug_image_index].get_gpu_image();		
							}
						}
					);	

					sg_bindings bindings = (sg_bindings){
						.images[0] = image_to_copy_to_swapchain, 
						.images[1] = debug_text_pass.color_outputs[0],
						.samplers[0] = state.sampler,
					};
					sg_apply_bindings(&bindings);

					sg_draw(0,6,1);

					DEBUG_UI(
						ImGui::End();
						simgui_render();
					);
				}
			);
		}

		sg_commit();
	}

	reset_mouse_delta();
}

void cleanup(void)
{
	// Tell live_link_thread we're done running and wait for it to complete
	state.game_running = false;
	state.live_link_thread.join();

	jolt_shutdown();

	#if WITH_DEBUG_UI
	simgui_shutdown();
	#endif // WITH_DEBUG_UI

    sg_shutdown();
}

void event(const sapp_event* event)
{
	#if WITH_DEBUG_UI
	// Pass events to sokol_imgui
	const bool simgui_wants_keyboard_capture = simgui_handle_event(event);
	#else
	const bool simgui_wants_keyboard_capture = false;
	#endif // WITH_DEBUG_UI

	switch(event->type)
	{
		case SAPP_EVENTTYPE_KEY_DOWN:
		{
			// stop execution on escape key 
			if (event->key_code == SAPP_KEYCODE_ESCAPE)
			{
			 	if (event->modifiers & SAPP_MODIFIER_SHIFT)
				{
					set_mouse_locked(false);
				}
				else
				{
					sapp_quit();
				}
			}

			app_state.keycodes[event->key_code] = true;
			break;
		}
		case SAPP_EVENTTYPE_KEY_UP:
		{
			app_state.keycodes[event->key_code] = false;
			break;
		}
		case SAPP_EVENTTYPE_MOUSE_DOWN:
		{
			app_state.mouse_buttons[event->mouse_button] = true;

			// Lock Mouse on left click into game space
			if (!simgui_wants_keyboard_capture && event->mouse_button == SAPP_MOUSEBUTTON_LEFT)
			{
				if (!is_mouse_locked())
				{
					set_mouse_locked(true);
				}
            }

			break;
		}
		case SAPP_EVENTTYPE_MOUSE_UP:
		{
			app_state.mouse_buttons[event->mouse_button] = false;
			break;
		}
		case SAPP_EVENTTYPE_MOUSE_MOVE:
		{
			app_state.mouse_delta.X = event->mouse_dx;
			app_state.mouse_delta.Y = event->mouse_dy;
			break;
		}
		case SAPP_EVENTTYPE_RESIZED:
		{
			handle_resize();
			break;
		}
		default: break;
	}
}

sapp_desc sokol_main(int argc, char* argv[])
{	
	cxxopts::Options options("Game", "Game that uses blender as its tooling");
	
	options.add_options()
	  ("f,file", "File name", cxxopts::value<std::string>())
  	;

	// First positional arg can be file to load
	options.parse_positional({"file"});

	auto result = options.parse(argc, argv);

	// If we passed an init file, load it on startup
	if (result.count("file") > 0)
	{
		state.init_file = result["f"].as<std::string>();
	}

    return (sapp_desc) {
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
		.event_cb = event,
        .width = 1920,
        .height = 1080,
        .sample_count = 1,
        .window_title = "Blender Game",
        .icon = {
			.sokol_default = true,
		},
        .logger.func = slog_func,
		.win32_console_attach = true,
    };
}
