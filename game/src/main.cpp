// C std lib I/O and stdlib
#include <cstdio>
#include <cstdlib>

// Simple Template Wrapper around stb_ds array
#define STB_DS_IMPLEMENTATION
#include "core/stretchy_buffer.h"

#include "render/gi.h"

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

// C++ std lib numeric limits
#include <limits>

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
#include "sokol/sokol_time.h"

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
#include "render/lighting_pass.h"
#include "render/blur_pass.h"
#include "render/sky_pass.h"
#include "render/shadow_depth_pass.h"
#include "render/shadow_blur_pass.h"
#include "render/shadow_cascade_debug_pass.h"

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

	HMM_Vec4 to_hmm_vec4(const Blender::LiveLink::Vec3* in_flatbuffers_vector, f32 in_w)
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

	if (state.images.id_to_index.contains(image_id))
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

	const i32 array_index = state.images.items.length();
	state.images.items.add(GpuImage(image_desc));
	state.images.id_to_index[image_id] = array_index;
	return true;
}

void reset_images()
{
	state.images.id_to_index.clear();
	state.images.items.reset();
	state.images.enable_debug_fullscreen = false;
	state.images.debug_index = 0;
}

// Registers a material and adds it to our gpu buffer
bool register_material(const Blender::LiveLink::Material& in_material)
{
	int material_id = in_material.unique_id();
	if (state.materials.id_to_index.contains(material_id))
	{
		return false;
	}

	printf("Registering material with ID: %i\n", material_id);
	if (state.materials.items.length() >= MAX_MATERIALS)
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
		.emission_color_image_index = -1,
		.metallic_image_index = -1,
		.roughness_image_index = -1,
	};

	int base_color_image_id = in_material.base_color_image_id();
	if (base_color_image_id > 0)
	{
		printf("Found base color image id: %i\n", base_color_image_id);
		assert(state.images.id_to_index.contains(base_color_image_id));
		new_material.base_color_image_index = state.images.id_to_index[base_color_image_id];
	}

	int metallic_image_id = in_material.metallic_image_id();
	if (metallic_image_id > 0)
	{
		printf("Found metallic image id: %i\n", metallic_image_id);
		assert(state.images.id_to_index.contains(metallic_image_id));
		new_material.metallic_image_index = state.images.id_to_index[metallic_image_id];
	}

	int roughness_image_id = in_material.roughness_image_id();
	if (roughness_image_id > 0)
	{
		printf("Found roughness image id: %i\n", roughness_image_id);
		assert(state.images.id_to_index.contains(roughness_image_id));
		new_material.roughness_image_index = state.images.id_to_index[roughness_image_id];
	}

	int emission_color_image_id = in_material.emission_color_image_id();
	if (emission_color_image_id > 0)
	{
		printf("Found roughness image id: %i\n", emission_color_image_id);
		assert(state.images.id_to_index.contains(emission_color_image_id));
		new_material.emission_color_image_index = state.images.id_to_index[emission_color_image_id];
	}

	const i32 array_index = state.materials.items.length();
	state.materials.items.add(new_material);
	state.materials.id_to_index[material_id] = array_index;
	return true;
}

void reset_materials()
{
	state.materials.id_to_index.clear();
	state.materials.items.reset();
	state.materials.buffer.destroy_gpu_buffer();
}

RenderPass& get_render_pass(const ERenderPass in_pass_id)
{
	return state.render_passes.passes[static_cast<int>(in_pass_id)];
}

bool object_is_sun_light(const Object& in_object)
{
	return in_object.has_light && in_object.light.type == LightType::Sun;
}

void refresh_primary_sun_id()
{
	if (state.scene.primary_sun_id.has_value())
	{
		const i32 primary_sun_id = state.scene.primary_sun_id.value();
		if (state.scene.objects.contains(primary_sun_id) && object_is_sun_light(state.scene.objects[primary_sun_id]))
		{
			return;
		}

		state.scene.primary_sun_id.reset();
	}

	for (auto const& [unique_id, object] : state.scene.objects)
	{
		if (object_is_sun_light(object))
		{
			state.scene.primary_sun_id = unique_id;
			printf("Found Primary Sun ID: %i\n", unique_id);
			return;
		}
	}
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
							if (!state.materials.id_to_index.contains(material_id))
							{
								printf("\tFailed to find material with id: %i\n", material_id);
								continue;
							}
							material_indices[material_id_idx] = state.materials.id_to_index[material_id];
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
									state.scene.player_character_id = game_object.unique_id;
								}
								break;
							}
							case Blender::LiveLink::GameplayComponent_GameplayComponentCameraControl:
							{
								using Blender::LiveLink::GameplayComponentCameraControl;
								const GameplayComponentCameraControl* cam_control_component = reinterpret_cast<const GameplayComponentCameraControl*>(component);

								const f32 cam_control_follow_distance = cam_control_component->follow_distance();
								const f32 cam_control_follow_speed = cam_control_component->follow_speed();

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

								state.scene.camera_control_id = game_object.unique_id;
								break;
							}
							default:
								assert(false);
						}
					}
				}

				// Send updated object data to main thread
				state.live_link.updated_objects.send(game_object);
			}

			state.runtime.blender_data_loaded = true;
		}

		if (auto deleted_object_uids = update->deleted_object_uids())
		{
			for (i32 deleted_object_uid : *deleted_object_uids)
			{
				state.live_link.deleted_objects.send(deleted_object_uid);
			}
		}

		if (update->reset())
		{
			state.live_link.reset.send(true);
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
	state.live_link.blender_socket = socket_open(res->ai_family, res->ai_socktype, res->ai_protocol);

	// Allow us to reuse address and port
	socket_set_reuse_addr_and_port(state.live_link.blender_socket, true);

	// bind our socket
	SOCKET_OP(bind(state.live_link.blender_socket, res->ai_addr, res->ai_addrlen));

	const i32 backlog = 1;
	SOCKET_OP(listen(state.live_link.blender_socket, backlog));

	// accept connections from blender
	struct sockaddr_storage their_addr;
	socklen_t addr_size = sizeof their_addr;
	do
	{
		state.live_link.connection_socket = accept(state.live_link.blender_socket, (struct sockaddr *) &their_addr, &addr_size);
	}
	while(!socket_is_valid(state.live_link.connection_socket) && state.runtime.game_running);

	// set recv timeout
	struct timeval recv_timeout = {
		.tv_sec = 1,
		.tv_usec = 0
	};
	socket_set_recv_timeout(state.live_link.connection_socket, recv_timeout);

	// infinite recv loop
	while (state.runtime.game_running)
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
			current_bytes_read = socket_recv(state.live_link.connection_socket, buffer, buffer_len, flags);

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
		while (state.runtime.game_running && (current_bytes_read == 0 || (flatbuffer_size && total_bytes_read < flatbuffer_size.value())));

		printf("We've got some data! Data Length: %td Packets Read: %i\n", flatbuffer_data.length(), packets_read);

		parse_flatbuffer_data(flatbuffer_data);
	}

	printf("Shutting down sockets\n");

	socket_close(state.live_link.connection_socket);
	socket_close(state.live_link.blender_socket);

	socket_lib_quit();
}

void handle_resize()
{
	int new_width = sapp_width();
    int new_height = sapp_height();

	if (new_width != state.window.width || new_height != state.window.height)
	{
		state.window.width = new_width;
		state.window.height = new_height;

		const int render_pass_count = (int) ERenderPass::COUNT;
		for (i32 pass_index = 0; pass_index < render_pass_count; ++pass_index)
		{
			state.render_passes.passes[pass_index].handle_resize(state.window.width, state.window.height);
		}
	}
}

GI_Scene gi_scene = {};

void init(void)
{
	stm_setup();

	jolt_init();

	// Init sokol graphics
    sg_setup((sg_desc) {
		.buffer_pool_size = 4096,
		.image_pool_size = 4096,
		//.sampler_pool_size = 1024,
		//.shader_pool_size =
		//.pipeline_pool_size =
		.view_pool_size = 8192,
        .logger.func = slog_func,
	   	.environment = sglue_environment(),
    });

	sg_swapchain swapchain = sglue_swapchain();

	state_init();

	// Spin up a thread that blocks until we receive our init event, and then listens for updates
	state.live_link.thread = std::thread(live_link_thread_function);

	// GI Scene Setup
	gi_scene_init(gi_scene);
	printf("gi_scene num cells: %zu num probes: %zu\n",
		gi_scene.cells.length(),
		gi_scene.probes.length()
	);

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

	// Init shadow depth pass
	get_render_pass(ERenderPass::ShadowDepth).init(ShadowDepthPass::make_render_pass_desc(SG_PIXELFORMAT_DEPTH));

	// Init shadow VSM blur pass
	get_render_pass(ERenderPass::ShadowBlur).init(ShadowBlurPass::make_render_pass_desc());

	// Init shadow cascade debug pass
	get_render_pass(ERenderPass::ShadowCascadeDebug).init(ShadowCascadeDebugPass::make_render_pass_desc());

	// Init main geometry pass
	get_render_pass(ERenderPass::Geometry).init(GeometryPass::make_render_pass_desc(swapchain.depth_format));

	RenderPassDesc ssao_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(ssao_ssao_shader_desc(sg_query_backend())),
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.cull_mode = SG_CULLMODE_NONE,
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
		std::uniform_real_distribution<f32> randomf32s(0.0, 1.0); // random f32s between [0.0, 1.0]
		std::default_random_engine generator;
		HMM_Vec4 ssao_noise[SSAO_TEXTURE_SIZE];
		for (u32 i = 0; i < SSAO_TEXTURE_SIZE; ++i)
		{
			ssao_noise[i] = HMM_V4(
				randomf32s(generator) * 2.0f - 1.0f,
				randomf32s(generator) * 2.0f - 1.0f,
				0.0f,
				0.0f
			);
		}

		GpuImageDesc ssao_noise_desc = {
			.width = SSAO_TEXTURE_WIDTH,
			.height = SSAO_TEXTURE_WIDTH,
			.pixel_format = SG_PIXELFORMAT_RGBA32F,
			.data = (u8*) &ssao_noise,
			.label = "ssao_noise_texture"
		};
		state.ssao.noise_texture = GpuImage(ssao_noise_desc);

		// Init ssao_fs_params
		state.ssao.fs_params = {
			.screen_size = HMM_V2(sapp_widthf(), sapp_heightf()),
		};

		//SSAO Kernel
		for (u32 i = 0; i < SSAO_KERNEL_SIZE; ++i)
		{
			HMM_Vec3 sample = HMM_V3(
				randomf32s(generator) * 2.0f - 1.0f,
				randomf32s(generator) * 2.0f - 1.0f,
				randomf32s(generator)
			);
			sample = HMM_NormV3(sample);
			sample *= randomf32s(generator);

			// scale samples s.t. they're more aligned to center of kernel
			f32 scale = f32(i) / (f32) SSAO_KERNEL_SIZE;
			scale = HMM_Lerp(0.1f, scale * scale, 1.0f);
			sample *= scale;

			state.ssao.fs_params.kernel_samples[i] = HMM_V4(sample.X, sample.Y, sample.Z, 0.0);
		}
	}

	RenderPassDesc ssao_blur_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(blur_blur_shader_desc(sg_query_backend())),
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.cull_mode = SG_CULLMODE_NONE,
			.label = "blur-pipeline",
		},
		.num_outputs = 1,
		.outputs[0] = {
			.pixel_format = swapchain.color_format,
			.load_action = SG_LOADACTION_LOAD,
			.store_action = SG_STOREACTION_STORE,
		},
		.num_scratch_outputs = 1,
		.scratch_outputs = {
			[0] = {
				.pixel_format = swapchain.color_format,
				.load_action = SG_LOADACTION_CLEAR,
				.store_action = SG_STOREACTION_STORE,
			},
		},
	};
	get_render_pass(ERenderPass::SSAO_Blur).init(ssao_blur_pass_desc);

	RenderPassDesc lighting_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(lighting_lighting_shader_desc(sg_query_backend())),
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.cull_mode = SG_CULLMODE_NONE,
			.label = "lighting-pipeline",
		},
		.num_outputs = 1,
		.outputs[0] = {
			.pixel_format = swapchain.color_format,
			.load_action = SG_LOADACTION_LOAD,
			.store_action = SG_STOREACTION_STORE,
		},
	};
	//get_render_pass(ERenderPass::Lighting).init(lighting_pass_desc);
	get_render_pass(ERenderPass::Lighting).init(LightingPass::make_render_pass_desc(swapchain.color_format));

	RenderPassDesc dof_combine_pass_desc = {
		.pipeline_desc = (sg_pipeline_desc) {
			.shader = sg_make_shader(dof_combine_dof_combine_shader_desc(sg_query_backend())),
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.cull_mode = SG_CULLMODE_NONE,
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
			.depth = {
				.pixel_format = SG_PIXELFORMAT_NONE,
			},
			.cull_mode = SG_CULLMODE_NONE,
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
			.depth = {
				.pixel_format = swapchain.depth_format,
				.compare = SG_COMPAREFUNC_ALWAYS,
				.write_enabled = false,
			},
			.cull_mode = SG_CULLMODE_NONE,
			.label = "copy-to-swapchain-pipeline",
		},
		.depth_output = {
			.pixel_format = swapchain.depth_format,
		},
		.type = ERenderPassType::Swapchain,
	};
	get_render_pass(ERenderPass::CopyToSwapchain).init(copy_to_swapchain_pass_desc);

	handle_resize();

	if (state.runtime.init_file)
	{
		if (FILE* file = fopen(state.runtime.init_file->c_str(), "rb"))
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

	HMM_Vec2 mouse_position;
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

bool ray_sphere_intersect(
	const HMM_Vec3& in_ray_origin,
	const HMM_Vec3& in_ray_direction,
	const HMM_Vec3& in_sphere_center,
	const f32 in_sphere_radius,
	f32& out_t
)
{
	const HMM_Vec3 oc = in_ray_origin - in_sphere_center;
	const f32 b = HMM_DotV3(oc, in_ray_direction);
	const f32 c = HMM_DotV3(oc, oc) - in_sphere_radius * in_sphere_radius;
	const f32 discriminant = b * b - c;
	if (discriminant < 0.0f)
	{
		return false;
	}

	const f32 sqrt_discriminant = HMM_SqrtF(discriminant);
	f32 t = -b - sqrt_discriminant;
	if (t < 0.0f)
	{
		t = -b + sqrt_discriminant;
	}
	if (t < 0.0f)
	{
		return false;
	}

	out_t = t;
	return true;
}

void pick_isolated_gi_probe()
{
	const f32 w = sapp_widthf();
	const f32 h = sapp_heightf();
	if (w <= 0.0f || h <= 0.0f)
	{
		return;
	}

	const f32 fov = HMM_AngleDeg(60.0f);
	const f32 aspect_ratio = w / h;

	Camera& camera = get_active_camera();
	const f32 ndc_x = (2.0f * app_state.mouse_position.X / w) - 1.0f;
	const f32 ndc_y = 1.0f - (2.0f * app_state.mouse_position.Y / h);

	const f32 tan_half_fov = HMM_TanF(fov * 0.5f);
	const HMM_Vec3 camera_forward = HMM_NormV3(camera.forward);
	const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera_forward, camera.up));
	const HMM_Vec3 camera_up = HMM_NormV3(HMM_Cross(camera_right, camera_forward));
	const HMM_Vec3 ray_origin = camera.location;
	const HMM_Vec3 ray_direction = HMM_NormV3(
		camera_forward +
		camera_right * (ndc_x * aspect_ratio * tan_half_fov) +
		camera_up * (ndc_y * tan_half_fov)
	);
	const f32 probe_radius = GI_CELL_EXTENT * 0.1f;

	i32 closest_probe_index = -1;
	f32 closest_t = std::numeric_limits<f32>::max();
	for (i32 probe_index = 0; probe_index < GI_PROBE_COUNT; ++probe_index)
	{
		f32 t = 0.0f;
		const HMM_Vec3 probe_position = gi_probe_position_from_index(probe_index);
		if (ray_sphere_intersect(ray_origin, ray_direction, probe_position, probe_radius, t) && t < closest_t)
		{
			closest_t = t;
			closest_probe_index = probe_index;
		}
	}

	if (closest_probe_index >= 0)
	{
		state.gi.isolated_probe_index = closest_probe_index;
	}
}

void frame(void)
{
	// Delta Time Calculation
	static u64 last_frame_time = 0;
	const u64 lap_time = stm_laptime(&last_frame_time);
	const double delta_time = stm_sec(lap_time);
    const float ui_dpi_scale = sapp_dpi_scale();

	state.debug_ui.stats_sample_elapsed += delta_time;
	state.debug_ui.stats_sample_count += 1;
	if (state.debug_ui.fps == 0.0f && delta_time > 0.0)
	{
		state.debug_ui.frame_time_ms = delta_time * 1000.0;
		state.debug_ui.fps = 1.0 / delta_time;
	}
	if (state.debug_ui.stats_sample_elapsed >= 0.25)
	{
		const double average_delta_time = state.debug_ui.stats_sample_elapsed / state.debug_ui.stats_sample_count;
		state.debug_ui.frame_time_ms = average_delta_time * 1000.0;
		state.debug_ui.fps = state.debug_ui.stats_sample_count / state.debug_ui.stats_sample_elapsed;
		state.debug_ui.stats_sample_elapsed = 0.0;
		state.debug_ui.stats_sample_count = 0;
	}

	DEBUG_UI(
		simgui_new_frame((simgui_frame_desc_t){
			.width = state.window.width,
			.height = state.window.height,
			.delta_time = delta_time,
			.dpi_scale = ui_dpi_scale,
		});
	);

	// Receive Any Updated Objects
	while (optional<Object> received_updated_object = state.live_link.updated_objects.receive())
	{
		Object& updated_object = *received_updated_object;
		i32 updated_object_uid = updated_object.unique_id;

		printf("Updating Object. UID: %i\n", updated_object_uid);

		// Cleanup old object
		if (state.scene.objects.contains(updated_object_uid))
		{
			Object& existing_object = state.scene.objects[updated_object_uid];
			object_cleanup(existing_object);
		}

		if (updated_object.has_light)
		{
			state.lighting.needs_data_update = true;
		}

		if (updated_object.has_rigid_body)
		{
			object_add_jolt_body(updated_object);
		}

		state.scene.objects[updated_object_uid] = updated_object;
	}

	// Receive Any Deleted Objects
	while (optional<i32> received_deleted_object = state.live_link.deleted_objects.receive())
	{
		i32 deleted_object_uid = *received_deleted_object;
		if (state.scene.objects.contains(deleted_object_uid))
		{
			printf("Removing object. UID: %i\n", deleted_object_uid);
			Object& object_to_delete = state.scene.objects[deleted_object_uid];

			if (object_to_delete.has_light)
			{
				state.lighting.needs_data_update = true;
			}

			object_cleanup(object_to_delete);
			state.scene.objects.erase(deleted_object_uid);
		}
	}

	// Receive any Reset Messages
	while(optional<bool> received_reset = state.live_link.reset.receive())
	{
		state.runtime.blender_data_loaded = false;
		state.lighting.needs_data_update = true;

		for (auto& [unique_id, object] : state.scene.objects)
		{
			if (object.has_rigid_body)
			{
				object_remove_jolt_body(object);
			}

			object_cleanup_gpu_resources(object);
		}

		//FCS TODO: Should also reset these if they're equal to a removed object...
		state.scene.objects.clear();
		state.scene.camera_control_id.reset();
		state.scene.player_character_id.reset();
		state.scene.primary_sun_id.reset();

		reset_materials();
		reset_images();
	}

	refresh_primary_sun_id();

	// Space Bar + Left Control Starts/Stops simulation
	DEFINE_TOGGLE_TWO_KEYS(state.runtime.is_simulating, SAPP_KEYCODE_SPACE, SAPP_KEYCODE_LEFT_CONTROL);

	#if WITH_DEBUG_UI
	// Control + I toggles imgui debug window
	DEFINE_TOGGLE_TWO_KEYS(g_show_imgui, SAPP_KEYCODE_I, SAPP_KEYCODE_LEFT_CONTROL);
	#endif // WITH_DEBUG_UI

	// D + Left Control toggle debug camera
	DEFINE_EVENT_TWO_KEYS(SAPP_KEYCODE_D, SAPP_KEYCODE_LEFT_CONTROL,
		if (!state.debug_camera.active) state.debug_camera.camera = get_active_camera();
		state.debug_camera.active = !state.debug_camera.active;
	);

	// Reset State
	DEFINE_EVENT_TWO_KEYS(SAPP_KEYCODE_R, SAPP_KEYCODE_LEFT_CONTROL,
		// Reset object transforms and recreate physics state
		for (auto& [unique_id, object] : state.scene.objects)
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
		if (ImGui::CollapsingHeader("General Stats", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Text("Resolution: %d x %d", state.window.width, state.window.height);
			ImGui::Text("frame time: %.2f ms", state.debug_ui.frame_time_ms);
			ImGui::Text("FPS: %.1f", state.debug_ui.fps);
			ImGui::Spacing();
		}

		if (ImGui::CollapsingHeader("Rendering Features", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Indent();
			if (ImGui::CollapsingHeader("Image Effects", ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::SliderFloat("Exposure (EV)", &state.tonemapping.fs_params.exposure_bias, -5.0f, 5.0f, "%.2f stops");
				ImGui::Checkbox("SSAO", &state.ssao.enable);
				if (ImGui::CollapsingHeader("Depth-of-Field", ImGuiTreeNodeFlags_DefaultOpen))
				{
					ImGui::Checkbox("Enable DoF", &state.dof.enable);
					ImGui::BeginDisabled(!state.dof.enable);
					ImGui::SliderFloat("Focus Distance", &state.dof.focus_distance, 0.1f, 500.0f, "%.1f");
					ImGui::SliderFloat("Focus Range", &state.dof.focus_range, 0.1f, 200.0f, "%.1f");
					ImGui::SliderFloat("Max CoC Radius", &state.dof.max_coc_radius, 0.0f, 32.0f, "%.1f px");
					ImGui::SliderFloat("Foreground Scale", &state.dof.foreground_blur_scale, 0.0f, 4.0f, "%.2f");
					ImGui::SliderFloat("Background Scale", &state.dof.background_blur_scale, 0.0f, 4.0f, "%.2f");
					const char* dof_debug_modes[] = { "Final", "CoC" };
					ImGui::Combo("Debug", &state.dof.debug_mode, dof_debug_modes, IM_ARRAYSIZE(dof_debug_modes));
					ImGui::EndDisabled();
				}
			}
			ImGui::Unindent();

			ImGui::Separator();

			ImGui::Indent();
			if (ImGui::CollapsingHeader("Shadows", ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::Checkbox("Shadow Rendering", &state.shadow.rendering_enable);
				ImGui::Checkbox("Shadow Blur", &state.shadow.blur_enable);
				ImGui::Checkbox("Freeze Shadow Depth", &state.shadow.depth_freeze);
				if (ImGui::SliderInt("Num Cascades", &state.shadow.num_cascades, 1, MAX_SHADOW_CASCADES))
				{
					ShadowDepthPass::has_valid_shadow_map = false;
					ShadowDepthPass::has_valid_shadow_blur = false;
				}
				f32& active_cascade_distance_scale = state.shadow.cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares
					? state.shadow.centered_square_cascade_distance_scale
					: state.shadow.frustum_cascade_distance_scale;
				if (ImGui::SliderFloat("Cascade Distance Scale", &active_cascade_distance_scale, 0.25f, 4.0f, "%.2f"))
				{
					ShadowDepthPass::has_valid_shadow_map = false;
					ShadowDepthPass::has_valid_shadow_blur = false;
				}
				if (ImGui::Combo("Cascade Placement", (i32*) &state.shadow.cascade_placement_mode, EShadowCascadePlacementModeNames, IM_ARRAYSIZE(EShadowCascadePlacementModeNames)))
				{
					ShadowDepthPass::has_valid_shadow_map = false;
					ShadowDepthPass::has_valid_shadow_blur = false;
				}
				if (state.shadow.cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares)
				{
					if (ImGui::SliderFloat("Centered Square Lookahead", &state.shadow.centered_square_lookahead_distance, 0.0f, 1000.0f, "%.2f"))
					{
						if (!state.shadow.depth_freeze)
						{
							ShadowDepthPass::has_valid_shadow_map = false;
							ShadowDepthPass::has_valid_shadow_blur = false;
						}
					}
					bool center_changed = false;
					ImGui::BeginDisabled(!state.shadow.depth_freeze);
					center_changed = ImGui::DragFloat3("Centered Square Center", &state.shadow.centered_square_center.X, 0.25f, -10000.0f, 10000.0f, "%.2f");
					ImGui::EndDisabled();
					if (center_changed)
					{
						state.shadow.force_recapture = true;
						ShadowDepthPass::has_valid_shadow_map = false;
						ShadowDepthPass::has_valid_shadow_blur = false;
					}
				}
				ImGui::Checkbox("Show Cascade Selection", &state.shadow.debug_show_cascade_selection);
			}
			ImGui::Unindent();

			ImGui::Separator();

			ImGui::Indent();
			if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::Checkbox("Sky Rendering", &state.sky.rendering_enable);
				ImGui::Checkbox("Direct Lighting", &state.lighting.direct_enable);

				ImGui::Separator();

				ImGui::Indent();
				if (ImGui::CollapsingHeader("Global Illumination", ImGuiTreeNodeFlags_DefaultOpen))
				{
					ImGui::Checkbox("GI", &state.gi.enable);
					ImGui::Checkbox("GI Probe Occlusion", &state.gi.probe_occlusion);
					if (ImGui::Combo("Probe Radiance Mode", (i32*) &state.gi.probe_radiance_mode, EProbeRadianceModeNames, IM_ARRAYSIZE(EProbeRadianceModeNames)))
					{
						state.gi.is_updating = true;
					}
					if (ImGui::Combo("Probe Occlusion Mode", (i32*) &state.gi.probe_occlusion_mode, EProbeOcclusionModeNames, IM_ARRAYSIZE(EProbeOcclusionModeNames)))
					{
						state.gi.is_updating = true;
					}
					if (ImGui::Checkbox("render sky to probes", &state.gi.render_sky_to_probes))
					{
						state.gi.is_updating = true;
					}
					ImGui::Checkbox("Show Probes", &state.gi.show_probes);
					if (ImGui::Checkbox("Probe Isolation", &state.gi.probe_isolation_enable))
					{
						if (state.gi.probe_isolation_enable)
						{
							state.gi.show_probes = true;
							set_mouse_locked(false);
						}
						else
						{
							state.gi.isolated_probe_index = -1;
						}
					}
					if (state.gi.probe_isolation_enable)
					{
						state.gi.show_probes = true;

						ImGui::SameLine();
						if (ImGui::SmallButton("Clear"))
						{
							state.gi.isolated_probe_index = -1;
						}

						if (state.gi.isolated_probe_index >= 0)
						{
							ImGui::Text("Isolated Probe: %d", state.gi.isolated_probe_index);
						}
						else
						{
							ImGui::Text("Isolated Probe: None");
						}
					}
					ImGui::SliderFloat("GI Intensity", &state.gi.intensity, 0.0f, 10.0f, "%.2f");
					if (ImGui::Button("Update GI Probes") && !state.gi.is_updating)
					{
						state.gi.is_updating = true;
					}
					ImGui::SameLine();
					ImGui::Checkbox("Compute Irradiance", &state.gi.compute_irradiance);

					if (state.gi.is_updating)
					{
						ImGui::SameLine();
						ImGui::Text("Updating...");
					}

					if (ImGui::Combo("Probe Vis Mode", (i32*) &state.gi.probe_vis_mode, EProbeVisModeNames, IM_ARRAYSIZE(EProbeVisModeNames)))
					{
						if (state.gi.probe_vis_mode == EProbeVisMode::SH9Irradiance || state.gi.probe_vis_mode == EProbeVisMode::SG9Irradiance)
						{
							state.gi.is_updating = true;
						}
					}
					if (ImGui::Checkbox("Debug Constant White Probes", &state.gi.debug_constant_white_probes))
					{
						state.gi.is_updating = true;
					}
				}
				ImGui::Unindent();
			}
			ImGui::Unindent();
		}

		if (ImGui::CollapsingHeader("Render Texture Viewer", ImGuiTreeNodeFlags_DefaultOpen))
		{
			{
				RenderPass& depth_pass = get_render_pass(ERenderPass::ShadowDepth);
				GpuImage& image = depth_pass.get_depth_output();
				const GpuImageDesc& desc = image.get_desc();
				ImGui::Text("Shadow Cascades");
				ImGui::Text("%d x %d x %d", desc.width, desc.height, ShadowDepthPass::get_active_cascade_count(state));
				const f32 active_cascade_distance_scale = state.shadow.cascade_placement_mode == EShadowCascadePlacementMode::CenteredSquares
					? state.shadow.centered_square_cascade_distance_scale
					: state.shadow.frustum_cascade_distance_scale;
				ImGui::Text("Distance Scale: %.2f", active_cascade_distance_scale);
				for (i32 cascade_idx = 0; cascade_idx < ShadowDepthPass::get_active_cascade_count(state); ++cascade_idx)
				{
					ImGui::Text("Cascade %d: %.2f", cascade_idx, ShadowDepthPass::cascade_distances[cascade_idx]);
				}
				const i32 active_cascade_count = ShadowDepthPass::get_active_cascade_count(state);
				if (state.shadow.debug_cascade_index >= active_cascade_count)
				{
					state.shadow.debug_cascade_index = active_cascade_count - 1;
				}
				ImGui::SliderInt("Debug Cascade", &state.shadow.debug_cascade_index, 0, active_cascade_count - 1);
				const char* shadow_debug_modes[] = { "Moments", "Depth" };
				ImGui::Combo("Debug View", &state.shadow.debug_view_mode, shadow_debug_modes, IM_ARRAYSIZE(shadow_debug_modes));

				GpuImage& debug_image = get_render_pass(ERenderPass::ShadowCascadeDebug).get_color_output(0);
				const GpuImageDesc& debug_desc = debug_image.get_desc();
				const f32 max_debug_size = 256.0f;
				const f32 aspect_ratio = (f32)debug_desc.width / (f32)debug_desc.height;
				const ImVec2 image_size = aspect_ratio >= 1.0f
					? ImVec2(max_debug_size, max_debug_size / aspect_ratio)
					: ImVec2(max_debug_size * aspect_ratio, max_debug_size);
				ImGui::Image(simgui_imtextureid(debug_image.get_texture_view(0)), image_size);
			}

			ImGui::Separator();

			if (ImGui::TreeNode("Main Pass"))
			{
				RenderPass& geometry_pass = get_render_pass(ERenderPass::Geometry);
				for (i32 i = 0; i < geometry_pass.get_num_color_outputs(); ++i)
				{
					GpuImage& image = get_render_pass(ERenderPass::Geometry).get_color_output(i);
					const GpuImageDesc& desc = image.get_desc();
					const ImVec2 image_size = ImVec2(desc.width / 4.0, desc.height / 4.0);

					// Use a solid background color (e.g., Black) to hide transparency
					const ImVec4 bg_col = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
					const ImVec4 tint_col = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // Pure white tint

					ImGui::ImageWithBg(
						simgui_imtextureid(image.get_texture_view(0)),
						image_size,
						ImVec2(0,0), ImVec2(1,1),
						bg_col, tint_col
					);
				}

				ImGui::TreePop(); // Remember to pop!
			}

			const ImVec2 debug_texture_size(256,256);

			// Octahedral Atlas Visualization
			ImGui::Text("Octahedral Atlas: Lighting");
			ImGui::Image(simgui_imtextureid(gi_scene_get_octahedral_lighting_view(gi_scene)), debug_texture_size);
			ImGui::Text("Octahedral Atlas: Depth");
			ImGui::Image(simgui_imtextureid(gi_scene_get_octahedral_depth_view(gi_scene)), debug_texture_size);

			// Baked Sky Visualization
			ImGui::Text("Baked Sky");
			ImGui::Image(simgui_imtextureid(SkyBakePass::get_baked_sky_image_view()), debug_texture_size);
		}
	);

	if (state.runtime.is_simulating)
	{
		//FCS TODO: Game logic update here

		// Jolt Physics Update
		jolt_update(delta_time);
	}

	// Debug Camera Control
	if (state.debug_camera.active && !is_key_pressed(SAPP_KEYCODE_LEFT_CONTROL))
	{
		Camera& camera = state.debug_camera.camera;
		const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));

		f32 move_speed = 10.0f * delta_time;
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
			const f32 look_speed = 1.0f * delta_time;
			const HMM_Vec2 mouse_delta = get_mouse_delta();

			const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));
			camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera.up, -mouse_delta.X * look_speed));
			camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera_right, -mouse_delta.Y * look_speed));
		}
	}

	// Player Camera Control
	if (state.scene.camera_control_id && !state.debug_camera.active)
	{
		Object& camera_control_object = state.scene.objects[*state.scene.camera_control_id];
		CameraControl& camera_control = camera_control_object.camera_control;
		Camera& camera = camera_control.camera;

		if (is_mouse_locked())
		{
			//FCS TODO: Add max angle property (angle above XY plane) that we can rotate camera
			//FCS TODO: Add rotation speed property to camera control component
			const f32 look_speed = 1.0f * delta_time;
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
	if (state.scene.player_character_id && !state.debug_camera.active)
	{
		const Camera& camera = get_active_camera();
		const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));

		Object& player_character_object = state.scene.objects[*state.scene.player_character_id];
		Character& player_character_state = player_character_object.character;
		f32 character_move_speed = player_character_state.settings.move_speed * delta_time;

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
		{
			// Run initially, and then only if our updates from blender encounter a light
			if (state.lighting.needs_data_update)
			{
				state.lighting.needs_data_update = false;

				const i32 MAX_LIGHTS_PER_TYPE = 1024;

				// Init point_lights_buffer if we haven't already
				if (!state.lighting.point_lights_buffer.is_gpu_buffer_valid())
				{
					state.lighting.point_lights_buffer = GpuBuffer((GpuBufferDesc<lighting_PointLight_t>){
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
				if (!state.lighting.spot_lights_buffer.is_gpu_buffer_valid())
				{
					state.lighting.spot_lights_buffer = GpuBuffer((GpuBufferDesc<lighting_SpotLight_t>){
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
				if (!state.lighting.sun_lights_buffer.is_gpu_buffer_valid())
				{
					state.lighting.sun_lights_buffer = GpuBuffer((GpuBufferDesc<lighting_SunLight_t>){
						.data = nullptr,
						.size = sizeof(lighting_SunLight_t) * MAX_LIGHTS_PER_TYPE,
						.usage = {
							.storage_buffer = true,
							.stream_update = true,
						},
						.label = "sun_lights",
					});
				}

				state.lighting.point_lights.reset();
				state.lighting.spot_lights.reset();
				state.lighting.sun_lights.reset();

				for (auto const& [unique_id, object] : state.scene.objects)
				{
					if (object.has_light)
					{
						const Light& light = object.light;
						switch(light.type)
						{
							case LightType::Point:
							{
								if (state.lighting.point_lights.length() >= MAX_LIGHTS_PER_TYPE)
								{
									printf("Exceeded Max Number of Point Lights (%i)\n", MAX_LIGHTS_PER_TYPE);
									continue;
								}

								lighting_PointLight_t new_point_light = {};
								const Transform& transform = object.current_transform;
								memcpy(&new_point_light.location, &transform.location, sizeof(f32) * 4);
								memcpy(&new_point_light.color, &object.light.color, sizeof(f32) * 3);
								new_point_light.color[3] = 1.0; // Force alpha to 1.0
								new_point_light.power = object.light.point.power;
								state.lighting.point_lights.add(new_point_light);
								break;
							}
							case LightType::Spot:
							{
								if (state.lighting.spot_lights.length() >= MAX_LIGHTS_PER_TYPE)
								{
									printf("Exceeded Max Number of Spot Lights (%i)\n", MAX_LIGHTS_PER_TYPE);
									continue;
								}

								lighting_SpotLight_t new_spot_light = {};
								const Transform& transform = object.current_transform;
								memcpy(&new_spot_light.location, &transform.location, sizeof(f32) * 4);
								memcpy(&new_spot_light.color, &object.light.color, sizeof(f32) * 3);
								new_spot_light.color[3] = 1.0; // Force alpha to 1.0

								HMM_Vec3 spot_light_dir = HMM_NormV3(HMM_RotateV3Q(HMM_V3(0,0,-1), transform.rotation));
								memcpy(&new_spot_light.direction, &spot_light_dir, sizeof(f32) * 3);

								new_spot_light.spot_angle_radians = object.light.spot.beam_angle / 2.0f;
								new_spot_light.power = object.light.spot.power;
								new_spot_light.edge_blend = object.light.spot.edge_blend;
								state.lighting.spot_lights.add(new_spot_light);
								break;
							}
							case LightType::Sun:
							{
								if (state.lighting.sun_lights.length() >= MAX_LIGHTS_PER_TYPE)
								{
									printf("Exceeded Max Number of Sun Lights (%i)\n", MAX_LIGHTS_PER_TYPE);
									continue;
								}

								lighting_SunLight_t new_sun_light = {};
								const Transform& transform = object.current_transform;
								memcpy(&new_sun_light.location, &transform.location, sizeof(f32) * 4);
								memcpy(&new_sun_light.color, &object.light.color, sizeof(f32) * 3);
								new_sun_light.color[3] = 1.0; // Force alpha to 1.0

								HMM_Vec3 sun_light_dir = HMM_NormV3(HMM_RotateV3Q(HMM_V3(0,0,-1), transform.rotation));
								memcpy(&new_sun_light.direction, &sun_light_dir, sizeof(f32) * 3);

								new_sun_light.power = object.light.sun.power;
								new_sun_light.cast_shadows = object.light.sun.cast_shadows;
								state.lighting.sun_lights.add(new_sun_light);
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

				state.lighting.fs_params.num_point_lights = state.lighting.point_lights.length();
				printf("Num Point Lights: %i\n", state.lighting.fs_params.num_point_lights);
				if (state.lighting.fs_params.num_point_lights > 0)
				{
					state.lighting.point_lights_buffer.update_gpu_buffer(
						(sg_range){
							.ptr = state.lighting.point_lights.data(),
							.size = sizeof(lighting_PointLight_t) * state.lighting.fs_params.num_point_lights,
						}
					);
				}

				state.lighting.fs_params.num_spot_lights = state.lighting.spot_lights.length();
				printf("Num Spot Lights: %i\n", state.lighting.fs_params.num_spot_lights);
				if (state.lighting.fs_params.num_spot_lights > 0)
				{
					state.lighting.spot_lights_buffer.update_gpu_buffer(
						(sg_range){
							.ptr = state.lighting.spot_lights.data(),
							.size = sizeof(lighting_SpotLight_t) * state.lighting.fs_params.num_spot_lights,
						}
					);
				}

				state.lighting.fs_params.num_sun_lights = state.lighting.sun_lights.length();
				printf("Num Sun Lights: %i\n", state.lighting.fs_params.num_sun_lights);
				if (state.lighting.fs_params.num_sun_lights > 0)
				{
					state.lighting.sun_lights_buffer.update_gpu_buffer(
						(sg_range){
							.ptr = state.lighting.sun_lights.data(),
							.size = sizeof(lighting_SunLight_t) * state.lighting.fs_params.num_sun_lights,
						}
					);
				}
			}
		}

		// Bake Sky
		if (state.sky.rendering_enable || state.gi.render_sky_to_probes)
		{
			SkyBakePass::render(state);
		}

		// Update our GI Scene
		gi_scene_update(gi_scene, state);

		// View + Projection matrix setup
		const f32 w = sapp_widthf();
		const f32 h = sapp_heightf();
		const f32 fov = HMM_AngleDeg(60.0f);
		const f32 aspect_ratio = w/h;
		HMM_Mat4 projection_matrix = mat4_perspective(fov, aspect_ratio);

		Camera& camera = get_active_camera();
		HMM_Vec3 target = camera.location + camera.forward * 10;
		HMM_Mat4 view_matrix = HMM_LookAt_RH(camera.location, target, camera.up);
		HMM_Mat4 view_projection_matrix = HMM_MulM4(projection_matrix, view_matrix);
		if (!state.shadow.depth_freeze)
		{
			state.shadow.centered_square_center = camera.location + HMM_NormV3(camera.forward) * state.shadow.centered_square_lookahead_distance;
		}

		// Get our jolt body interface
		JPH::BodyInterface& body_interface = jolt_state.physics_system.GetBodyInterface();

		// Update Objects
		for (auto& [unique_id, object] : state.scene.objects)
		{
			// For objects that simulate physics, copy their physics transforms into their uniform buffers
			object_copy_physics_transform(object, body_interface);

			if (object.storage_buffer_needs_update)
			{
				object.storage_buffer_needs_update = false;
				object_update_storage_buffer(object);
			}
		}

		if (state.shadow.rendering_enable)
		{
			if (!ShadowDepthPass::get_valid_shadow_sun(state))
			{
				ShadowDepthPass::has_valid_shadow_map = false;
				ShadowDepthPass::has_valid_shadow_blur = false;
			}
			else
			{
				// Only update shadow depth if we're not freezing it, or if we don't have a valid shadow map yet
				const bool should_update_shadow_depth = !state.shadow.depth_freeze || !ShadowDepthPass::has_valid_shadow_map || state.shadow.force_recapture;
				if (should_update_shadow_depth)
				{
					get_render_pass(ERenderPass::ShadowDepth).execute(
						[&](const i32 pass_idx)
						{
							ShadowDepthPass::render(state, pass_idx);
						}
					);
					state.shadow.force_recapture = false;
					ShadowDepthPass::has_valid_shadow_blur = false;
				}

				if (state.shadow.blur_enable && ShadowDepthPass::has_valid_shadow_map && !ShadowDepthPass::has_valid_shadow_blur)
				{
					ShadowBlurPass::execute_separable(
						get_render_pass(ERenderPass::ShadowBlur),
						get_render_pass(ERenderPass::ShadowDepth).get_color_output(0).get_texture_array_view(),
						state.gpu.linear_sampler,
						HMM_V2(
							(f32)ShadowDepthPass::ShadowMapResolution,
							(f32)ShadowDepthPass::ShadowMapResolution
						),
						21
					);
					ShadowDepthPass::has_valid_shadow_blur = true;
				}
				else if (!state.shadow.blur_enable)
				{
					ShadowDepthPass::has_valid_shadow_blur = false;
				}

				if (ShadowDepthPass::has_valid_shadow_map)
				{
					GpuImage& shadow_moments_texture = state.shadow.blur_enable && ShadowDepthPass::has_valid_shadow_blur
						? get_render_pass(ERenderPass::ShadowBlur).get_color_output(0)
						: get_render_pass(ERenderPass::ShadowDepth).get_color_output(0);
					const i32 active_cascade_count = ShadowDepthPass::get_active_cascade_count(state);
					if (state.shadow.debug_cascade_index >= active_cascade_count)
					{
						state.shadow.debug_cascade_index = active_cascade_count - 1;
					}
					ShadowCascadeDebugPass::render(
						get_render_pass(ERenderPass::ShadowCascadeDebug),
						shadow_moments_texture.get_texture_array_view(),
						state.gpu.linear_sampler,
						state.shadow.debug_cascade_index,
						state.shadow.debug_view_mode
					);
				}
			}
		}
		else
		{
			ShadowDepthPass::has_valid_shadow_map = false;
			ShadowDepthPass::has_valid_shadow_blur = false;
		}

		{ // Geometry Pass
			get_render_pass(ERenderPass::Geometry).execute(
				[&](const i32 pass_idx)
				{
				    geometry_vs_params_t vs_params;
					vs_params.view = view_matrix;
					vs_params.projection = projection_matrix;

					// Apply Vertex Uniforms
					sg_apply_uniforms(0, SG_RANGE(vs_params));

					// Get our jolt body interface
					JPH::BodyInterface& body_interface = jolt_state.physics_system.GetBodyInterface();

					// Cull objects
					CullResult cull_result = cull_objects(state.scene.objects, view_projection_matrix);

					DEBUG_UI(
						if (ImGui::CollapsingHeader("Scene Stats", ImGuiTreeNodeFlags_DefaultOpen))
						{
							ImGui::Text("Total Objects: %zu", state.scene.objects.size());

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
								ImGui::Text("Num Point Lights: %i", state.lighting.point_lights.length());
								ImGui::Text("Num Spot Lights:  %i", state.lighting.spot_lights.length());
								ImGui::Text("Num Sun Lights:   %i", state.lighting.sun_lights.length());
								ImGui::TreePop();
							}

						}
					);

					// Submit draw calls for objects after culling
					for (auto& [unique_id, object_ptr] : cull_result.objects)
					{
						assert(object_ptr);
						Object& object = *object_ptr;

						if (object.has_mesh)
						{
							Mesh& mesh = object.mesh;

							int mesh_material_idx = mesh.material_indices[0];
							assert(mesh_material_idx >= 0);
							const geometry_Material_t& material = state.materials.items[mesh_material_idx];

							GpuImage& base_color_image = material.base_color_image_index >= 0 ? state.images.items[material.base_color_image_index] : state.gpu.default_image;
							GpuImage& metallic_image = material.metallic_image_index >= 0 ? state.images.items[material.metallic_image_index] : state.gpu.default_image;
							GpuImage& roughness_image = material.roughness_image_index >= 0 ? state.images.items[material.roughness_image_index] : state.gpu.default_image;
							GpuImage& emission_color_image = material.emission_color_image_index >= 0 ? state.images.items[material.emission_color_image_index] : state.gpu.default_image;

							sg_bindings bindings = {
								.vertex_buffers[0] = mesh.vertex_buffer.get_gpu_buffer(),
								.index_buffer = mesh.index_buffer.get_gpu_buffer(),
								.views = {
									[0] = object.storage_buffer.get_storage_view(),
									[1] = get_materials_buffer().get_storage_view(),
									[2] = base_color_image.get_texture_view(0),
									[3] = metallic_image.get_texture_view(0),
									[4] = roughness_image.get_texture_view(0),
									[5] = emission_color_image.get_texture_view(0),
								},
								.samplers[0] = state.gpu.linear_sampler,
							};
							sg_apply_bindings(&bindings);
							sg_draw(0, mesh.index_count, 1);
						}
					}

					if (state.gi.show_probes)
					{
						gi_scene_render_debug(gi_scene, view_matrix, projection_matrix);
					}

					if (state.sky.rendering_enable)
					{
						SkyPass::render(
							view_projection_matrix,
							get_active_camera().location,
							sglue_swapchain().depth_format
						);
					}
				}
			);
		}

		{ // SSAO
			get_render_pass(ERenderPass::SSAO).execute(
				[&](const i32 pass_idx)
				{
					state.ssao.fs_params.screen_size = HMM_V2(sapp_widthf(), sapp_heightf());
					state.ssao.fs_params.view = view_matrix;
					state.ssao.fs_params.projection = projection_matrix;
					state.ssao.fs_params.ssao_enable = state.ssao.enable;
					sg_apply_uniforms(0, SG_RANGE(state.ssao.fs_params));

					RenderPass& geometry_pass = get_render_pass(ERenderPass::Geometry);

					sg_bindings bindings = (sg_bindings){
						.views = {
							[0] = geometry_pass.get_color_output(1).get_texture_view(0),	// geometry pass position
							[1] = geometry_pass.get_color_output(2).get_texture_view(0),	// geometry pass normal
							[2] = state.ssao.noise_texture.get_texture_view(0),			// ssao noise texture
						},
						.samplers[0] = state.gpu.linear_sampler,
					};
					sg_apply_bindings(&bindings);

					sg_draw(0,6,1);
				}
			);
		}

		{ // SSAO Blur
			BlurPass::execute_separable(
				get_render_pass(ERenderPass::SSAO_Blur),
				get_render_pass(ERenderPass::SSAO).get_color_output(0).get_texture_view(0),
				state.gpu.linear_sampler,
				HMM_V2(sapp_widthf(), sapp_heightf()),
				4
			);
		}

		{ // Lighting Pass
			get_render_pass(ERenderPass::Lighting).execute(
				[&](const i32 pass_idx)
				{
					state.lighting.fs_params.view_position = get_active_camera().location;
					state.lighting.fs_params.view_forward = get_active_camera().forward;
					state.lighting.fs_params.ssao_enable = state.ssao.enable;
					state.lighting.fs_params.direct_lighting_enable = state.lighting.direct_enable;
					state.lighting.fs_params.gi_enable = state.gi.enable;
					state.lighting.fs_params.gi_probe_occlusion = state.gi.probe_occlusion;
					state.lighting.fs_params.probe_occlusion_mode = static_cast<i32>(state.gi.probe_occlusion_mode);
					state.lighting.fs_params.probe_radiance_mode = static_cast<i32>(state.gi.probe_radiance_mode);
					state.lighting.fs_params.gi_intensity = state.gi.intensity;
					state.lighting.fs_params.atlas_total_size = gi_scene.atlas_total_size;
					state.lighting.fs_params.atlas_entry_size = gi_scene.atlas_entry_size;
					state.lighting.fs_params.shadow_map_enable = state.shadow.rendering_enable && ShadowDepthPass::has_valid_shadow_map ? 1 : 0;
					state.lighting.fs_params.shadow_num_cascades = ShadowDepthPass::get_active_cascade_count(state);
					state.lighting.fs_params.shadow_cascade_placement_mode = (i32) state.shadow.cascade_placement_mode;
					state.lighting.fs_params.shadow_debug_show_cascade_selection = state.shadow.debug_show_cascade_selection ? 1 : 0;
					state.lighting.fs_params.isolated_probe_index = state.gi.probe_isolation_enable
						? (state.gi.isolated_probe_index >= 0 ? state.gi.isolated_probe_index : -2)
						: -1;
					state.lighting.fs_params.shadow_bias = 0.001f;
					state.lighting.fs_params.shadow_map_texel_size = HMM_V2(
						1.0f / (f32)ShadowDepthPass::ShadowMapResolution,
						1.0f / (f32)ShadowDepthPass::ShadowMapResolution
					);
					state.lighting.fs_params.shadow_cascade_distances = HMM_V4(
						ShadowDepthPass::cascade_distances[0],
						ShadowDepthPass::cascade_distances[1],
						ShadowDepthPass::cascade_distances[2],
						ShadowDepthPass::cascade_distances[3]
					);
					for (i32 i = 0; i < MAX_SHADOW_CASCADES; ++i)
					{
						state.lighting.fs_params.shadow_view_projections[i] = ShadowDepthPass::shadow_view_projections[i];
					}

					// Apply Fragment Uniforms
					sg_apply_uniforms(0, SG_RANGE(state.lighting.fs_params));

					RenderPass& geometry_pass = get_render_pass(ERenderPass::Geometry);
					RenderPass& ssao_blur_pass = get_render_pass(ERenderPass::SSAO_Blur);

					GpuImage& color_texture = geometry_pass.get_color_output(0);
					GpuImage& position_texture = geometry_pass.get_color_output(1);
					GpuImage& normal_texture = geometry_pass.get_color_output(2);
					GpuImage& roughness_metallic_texture = geometry_pass.get_color_output(3);
					GpuImage& blurred_ssao_texture = ssao_blur_pass.get_color_output(0);
					GpuImage& shadow_moments_texture = state.shadow.blur_enable && ShadowDepthPass::has_valid_shadow_blur
						? get_render_pass(ERenderPass::ShadowBlur).get_color_output(0)
						: get_render_pass(ERenderPass::ShadowDepth).get_color_output(0);

					sg_bindings bindings = {
						.views = {
							[0] = color_texture.get_texture_view(0),
							[1] = position_texture.get_texture_view(0),
							[2] = normal_texture.get_texture_view(0),
							[3] = roughness_metallic_texture.get_texture_view(0),
							[4] = blurred_ssao_texture.get_texture_view(0),
							[5] = state.lighting.point_lights_buffer.get_storage_view(),
							[6] = state.lighting.spot_lights_buffer.get_storage_view(),
							[7] = state.lighting.sun_lights_buffer.get_storage_view(),
							[8] = gi_scene.probes_buffer.get_storage_view(),
							[9] = gi_scene.cells_buffer.get_storage_view(),
							[10] = gi_scene_get_octahedral_lighting_view(gi_scene),
							[11] = gi_scene_get_octahedral_depth_view(gi_scene),
							[12] = shadow_moments_texture.get_texture_array_view(),
							[13] = gi_scene.sh9_coefficients_buffer.get_storage_view(),
							[14] = gi_scene.sg9_lobes_buffer.get_storage_view(),
						},
						.samplers = {
							[0] = state.gpu.linear_sampler,
							[1] = state.gpu.linear_sampler,
						},
					};
					sg_apply_bindings(&bindings);

					sg_draw(0,6,1);
				}
			);
		}

		{ // DOF

			if (state.dof.enable)
			{
				get_render_pass(ERenderPass::DOF_Combine).execute(
					[&](const i32 pass_idx)
					{
						RenderPass& lighting_pass = get_render_pass(ERenderPass::Lighting);
						RenderPass& geometry_pass = get_render_pass(ERenderPass::Geometry);

						const dof_combine_fs_params_t dof_combine_fs_params = {
							.cam_pos = HMM_V4(camera.location.X, camera.location.Y, camera.location.Z, 1.0f),
							.cam_forward = HMM_V4(camera.forward.X, camera.forward.Y, camera.forward.Z, 0.0f),
							.screen_size = HMM_V2(sapp_widthf(), sapp_heightf()),
							.focus_distance = state.dof.focus_distance,
							.focus_range = state.dof.focus_range,
							.max_coc_radius = state.dof.max_coc_radius,
							.foreground_blur_scale = state.dof.foreground_blur_scale,
							.background_blur_scale = state.dof.background_blur_scale,
							.debug_mode = state.dof.debug_mode,
						};
						sg_apply_uniforms(0, SG_RANGE(dof_combine_fs_params));

						GpuImage& position_texture = geometry_pass.get_color_output(1);

						sg_bindings bindings = (sg_bindings){
							.views = {
								[0] = lighting_pass.get_color_output(0).get_texture_view(0),
								[1] = position_texture.get_texture_view(0),
							},
							.samplers[0] = state.gpu.linear_sampler,
						};

						sg_apply_bindings(&bindings);

						sg_draw(0,6,1);
					}
				);
			}
		}

		{ // Tonemapping Pass
			get_render_pass(ERenderPass::Tonemapping).execute(
				[&](const i32 pass_idx)
				{
					RenderPass& lighting_pass = get_render_pass(ERenderPass::Lighting);
					RenderPass& dof_combine_pass = get_render_pass(ERenderPass::DOF_Combine);

					sg_apply_uniforms(0, SG_RANGE(state.tonemapping.fs_params));

					sg_bindings bindings = (sg_bindings){
						.views = {
							[0] = state.dof.enable
								? dof_combine_pass.get_color_output(0).get_texture_view(0)
								: lighting_pass.get_color_output(0).get_texture_view(0),
						},
						.samplers[0] = state.gpu.linear_sampler,
					};

					sg_apply_bindings(&bindings);

					sg_draw(0,6,1);
				}
			);
		}

		{ // Debug Text
			get_render_pass(ERenderPass::DebugText).execute(
				[&](const i32 pass_idx)
				{
					// Larger numbers scales down text
					const f32 text_scale = 0.5f;
					sdtx_canvas(sapp_width() * text_scale, sapp_height() * text_scale);
					sdtx_origin(1.0f, 2.0f);
					sdtx_home();

					if (!state.runtime.blender_data_loaded)
					{
						draw_debug_text(FONT_C64, "Waiting on data from Blender\n", 255,255,255);
					}

					if (state.debug_camera.active)
					{
						draw_debug_text(FONT_C64, "Debug Camera Active\n", 255,255,255);
					}

					if (!state.runtime.is_simulating)
					{
						draw_debug_text(FONT_C64, "Simulation Paused\n", 255,255,255);
					}

					sdtx_draw();
				}
			);
		}

		{ // Copy To Swapchain Pass
			get_render_pass(ERenderPass::CopyToSwapchain).execute(
				[&](const i32 pass_idx)
				{
					RenderPass& tonemapping_pass = get_render_pass(ERenderPass::Tonemapping);
					RenderPass& debug_text_pass = get_render_pass(ERenderPass::DebugText);

					// This can be overridden by the debug image viewer below
					GpuImage& image_to_copy_to_swapchain = tonemapping_pass.get_color_output(0);

					DEBUG_UI(
						const i32 num_images = state.images.items.length();
						if (num_images > 0)
						{
							if (ImGui::CollapsingHeader("Debug Image Viewer"))
							{
								ImGui::Checkbox("Fullscreen", &state.images.enable_debug_fullscreen);
								ImGui::SliderInt(
									"Image Index",
									&state.images.debug_index,
									0, num_images - 1,
									"%d", ImGuiSliderFlags_ClampOnInput
								);

								GpuImage& image = state.images.items[state.images.debug_index];
								const ImVec2 size = ImVec2(256, 256);

								ImTextureID imtex_id = simgui_imtextureid(image.get_texture_view(0));
								ImGui::Image(imtex_id, size);
							}

							if (state.images.enable_debug_fullscreen)
							{
								image_to_copy_to_swapchain = state.images.items[state.images.debug_index];
							}
						}
					);

					sg_bindings bindings = (sg_bindings){
						.views = {
							[0] = image_to_copy_to_swapchain.get_texture_view(0),
							[1] = debug_text_pass.get_color_output(0).get_texture_view(0),
						},
						.samplers[0] = state.gpu.nearest_sampler,
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
	state.runtime.game_running = false;
	state.live_link.thread.join();

	jolt_shutdown();

	#if WITH_DEBUG_UI
	simgui_shutdown();
	#endif // WITH_DEBUG_UI

    sg_shutdown();
}

void event(const sapp_event* event)
{
	#if WITH_DEBUG_UI
	const bool imgui_wants_mouse_capture = g_show_imgui ? ImGui::GetIO().WantCaptureMouse : false;
	// Pass events to sokol_imgui
	simgui_handle_event(event);
	#else
	const bool imgui_wants_mouse_capture = false;
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
			app_state.mouse_position = HMM_V2(event->mouse_x, event->mouse_y);

			if (!imgui_wants_mouse_capture && event->mouse_button == SAPP_MOUSEBUTTON_LEFT && state.gi.probe_isolation_enable && state.gi.show_probes)
			{
				pick_isolated_gi_probe();
				break;
			}

			// Lock Mouse on left click into game space
			if (!imgui_wants_mouse_capture && event->mouse_button == SAPP_MOUSEBUTTON_LEFT)
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
			app_state.mouse_position.X = event->mouse_x;
			app_state.mouse_position.Y = event->mouse_y;
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
		state.runtime.init_file = result["f"].as<std::string>();
	}

    return (sapp_desc) {
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
		.event_cb = event,
        .width = 1920,
        .height = 1080,
        .sample_count = 1,
		.high_dpi = true,
        .window_title = "Blender Game",
        .icon = {
			.sokol_default = true,
		},
        .logger.func = slog_func,
		.win32.console_attach = true,
    };
}
