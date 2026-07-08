//
// game2: Vulkan (MoltenVK) + Volk + VMA + GLFW port of the live-link game.
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

using std::optional;

#ifndef WITH_DEBUG_UI
#define WITH_DEBUG_UI 1
#endif

#if defined(__APPLE__)
	#define VK_USE_PLATFORM_METAL_EXT
#endif

#define VK_NO_PROTOTYPES
#define VOLK_IMPLEMENTATION
#include "volk/volk.h"

#include "vma/vk_mem_alloc.h"

#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"

#define HANDMADE_MATH_IMPLEMENTATION
#include "handmade_math/HandmadeMath.h"

// Simple Template Wrapper around stb_ds array
#define STB_DS_IMPLEMENTATION
#include "core/stretchy_buffer.h"

// Command line argument parsing
#include "cxxopts/cxxopts.hpp"

// Generated flatbuffer schema (from ../compiled_schemas/cpp)
#include "blender_live_link_generated.h"

#include "core/types.h"
#include "core/timings.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/gpu_buffer.h"
#include "game_object/game_object.h"
#include "state/state.h"
#include "render/forward_pass.h"

// Key with no binding, used as the "no modifier" sentinel in the event macros
// (GLFW key 0 is unassigned; is_key_pressed(0) is always false, so we special
// case it below)
#define KEYCODE_NONE 0

// Macro to define an event and run code in __VA_ARGS__ when it triggers
#define DEFINE_EVENT_TWO_KEYS(key_1, key_2, ...)\
	{\
		static bool was_key_pressed = false;\
		const bool is_key_1_pressed = is_key_pressed(key_1);\
		const bool is_key_2_pressed = (key_2 == KEYCODE_NONE) ? true : is_key_pressed(key_2);\
		if (is_key_1_pressed && is_key_2_pressed && !was_key_pressed)\
		{\
			{__VA_ARGS__}\
		}\
		was_key_pressed = is_key_1_pressed && is_key_2_pressed;\
	}

// Same as above but no mod key
#define DEFINE_EVENT_ONE_KEY(key, ...) DEFINE_EVENT_TWO_KEYS(key, KEYCODE_NONE, __VA_ARGS__)

// Macro to handle toggleable inputs. uses event macro above but just sets variable instead of passing in __VA_ARGS__
#define DEFINE_TOGGLE_TWO_KEYS(var_name, key_1, key_2)\
	DEFINE_EVENT_TWO_KEYS(key_1, key_2,\
		var_name = !var_name;\
	);

// Same as above but no mod key
#define DEFINE_TOGGLE_ONE_KEY(var_name, key)\
	DEFINE_TOGGLE_TWO_KEYS(var_name, key, KEYCODE_NONE)

Camera& get_active_camera()
{
	if (state.scene.camera_control_id && !state.debug_camera.active)
	{
		Object& camera_control_object = state.scene.objects[*state.scene.camera_control_id];
		return camera_control_object.camera_control.camera;
	}
	return state.debug_camera.camera;
}

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

char* copy_flatbuffer_string(const flatbuffers::String* in_string)
{
	if (!in_string)
	{
		return nullptr;
	}

	const char* source = in_string->c_str();
	const size_t length = strlen(source);
	char* result = (char*) malloc(length + 1);
	memcpy(result, source, length + 1);
	return result;
}

// Parses a size-prefixed flatbuffer Update and sends results to the main
// thread via channels. Runs on the live link thread — GPU resources are only
// described here (lazy GpuBuffer), never created.
// game2 scope: objects (transform + mesh + light + camera control), deletes,
// reset. Images, materials, armatures, rigid bodies, characters, and fog are
// skipped until their systems are ported.
void parse_flatbuffer_data(StretchyBuffer<u8>& flatbuffer_data)
{
	if (flatbuffer_data.length() == 0)
	{
		return;
	}

	// Interpret Flatbuffer data
	auto* update = Blender::LiveLink::GetSizePrefixedUpdate(flatbuffer_data.data());
	assert(update);

	// process objects from update
	if (auto objects = update->objects())
	{
		for (u32 idx = 0; idx < objects->size(); ++idx)
		{
			auto object = objects->Get(idx);
			if (auto object_name = object->name())
			{
				printf("\tObject Name: %s\n", object_name->c_str());
			}

			int unique_id = object->unique_id();
			bool visibility = object->visibility();

			auto object_location = object->location();
			auto object_scale = object->scale();
			auto object_rotation = object->rotation();
			if (!object_location || !object_scale || !object_rotation)
			{
				printf("\tDropping malformed object UID: %i\n", unique_id);
				continue;
			}

			HMM_Vec4 location	= flatbuffer_helpers::to_hmm_vec4(object_location, 1.0f);
			HMM_Vec3 scale		= flatbuffer_helpers::to_hmm_vec3(object_scale);
			HMM_Quat rotation	= flatbuffer_helpers::to_hmm_quat(object_rotation);

			Object game_object = object_create(
				unique_id,
				copy_flatbuffer_string(object->name()),
				visibility,
				location,
				rotation,
				scale
			);

			if (auto object_mesh = object->mesh())
			{
				u32 num_vertices = 0;
				Vertex* vertices = nullptr;

				auto flatbuffer_positions = object_mesh->positions();
				auto flatbuffer_normals = object_mesh->normals();
				auto flatbuffer_texcoords = object_mesh->texcoords();
				const bool has_valid_vertex_streams =
					flatbuffer_positions &&
					flatbuffer_normals &&
					flatbuffer_texcoords &&
					(flatbuffer_positions->size() % 3) == 0 &&
					flatbuffer_normals->size() >= flatbuffer_positions->size() &&
					flatbuffer_texcoords->size() >= (flatbuffer_positions->size() / 3) * 2;
				if (has_valid_vertex_streams)
				{
					num_vertices = flatbuffer_positions->size() / 3;
					vertices = (Vertex*) malloc(sizeof(Vertex) * num_vertices);
					for (u32 vertex_idx = 0; vertex_idx < num_vertices; ++vertex_idx)
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
				else
				{
					printf("\tDropping malformed mesh vertex streams on object UID: %i\n", unique_id);
				}

				u32 num_indices = 0;
				u32* indices = nullptr;
				if (auto flatbuffer_indices = object_mesh->indices())
				{
					if ((flatbuffer_indices->size() % 3) != 0)
					{
						printf("\tDropping malformed triangle index stream on object UID: %i\n", unique_id);
					}
					else
					{
						num_indices = flatbuffer_indices->size();
						indices = (u32*) malloc(sizeof(u32) * num_indices);
						for (u32 indices_idx = 0; indices_idx < num_indices; ++indices_idx)
						{
							indices[indices_idx] = flatbuffer_indices->Get(indices_idx);
						}
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
					};
					game_object.mesh = make_mesh(mesh_init_data);
					game_object.has_mesh = true;
				}
				else
				{
					free(indices);
					free(vertices);
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
						break;
					}
					default:
						printf("\t\tUnsupported Light Type\n");
						exit(0);
				}
			}

			// Custom gameplay components we've specified on our blender objects
			if (auto object_components = object->components())
			{
				u32 num_components = object_components->size();
				for (u32 component_idx = 0; component_idx < num_components; ++component_idx)
				{
					auto component_container = object_components->Get(component_idx);
					if (!component_container) { continue; }
					auto component = component_container->value();
					if (!component) { continue; }

					auto component_type = component_container->value_type();
					switch (component_type)
					{
						case Blender::LiveLink::GameplayComponent_GameplayComponentCameraControl:
						{
							using Blender::LiveLink::GameplayComponentCameraControl;
							const GameplayComponentCameraControl* cam_control_component = reinterpret_cast<const GameplayComponentCameraControl*>(component);

							const f32 cam_control_follow_distance = cam_control_component->follow_distance();
							const f32 cam_control_follow_speed = cam_control_component->follow_speed();

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
							// Character, fog, etc. return with their systems
							break;
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

// Live Link Function. Runs on its own thread
void live_link_thread_function()
{
	socket_lib_init();

	// Init socket we'll use to talk to blender
	struct addrinfo hints, *res;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	const char* HOST = "127.0.0.1";
	getaddrinfo(HOST, state.live_link.port.c_str(), &hints, &res);

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
	while (!socket_is_valid(state.live_link.connection_socket) && state.runtime.game_running);

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
					||	last_error == socket_error_would_block()
					||	last_error == socket_error_timed_out())
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

// Drain live link channels on the main thread. GPU buffer destruction for
// replaced/deleted objects routes through the deletion queue, so this is safe
// while frames are in flight.
void live_link_drain_channels()
{
	// Receive Any Updated Objects
	while (optional<Object> received_updated_object = state.live_link.updated_objects.receive())
	{
		Object& updated_object = *received_updated_object;
		i32 updated_object_uid = updated_object.unique_id;

		printf("Updating Object. UID: %i\n", updated_object_uid);

		// Cleanup old object
		if (state.scene.objects.contains(updated_object_uid))
		{
			object_cleanup(state.scene.objects[updated_object_uid]);
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
			object_cleanup(state.scene.objects[deleted_object_uid]);
			state.scene.objects.erase(deleted_object_uid);

			if (state.scene.camera_control_id == deleted_object_uid)
			{
				state.scene.camera_control_id.reset();
			}
		}
	}

	// Receive any Reset Messages
	while (optional<bool> received_reset = state.live_link.reset.receive())
	{
		state.runtime.blender_data_loaded = false;

		for (auto& [unique_id, object] : state.scene.objects)
		{
			object_cleanup(object);
		}

		state.scene.objects.clear();
		state.scene.camera_control_id.reset();
	}
}

void key_callback(GLFWwindow* in_window, i32 in_key, i32 in_scancode, i32 in_action, i32 in_mods)
{
	if (in_key < 0 || in_key > GLFW_KEY_LAST)
	{
		return;
	}

	if (in_action == GLFW_PRESS)
	{
		if (in_key == GLFW_KEY_ESCAPE)
		{
			if (in_mods & GLFW_MOD_SHIFT)
			{
				set_mouse_locked(false);
			}
			else
			{
				glfwSetWindowShouldClose(in_window, GLFW_TRUE);
			}
		}

		state.input.keycodes[in_key] = true;
	}
	else if (in_action == GLFW_RELEASE)
	{
		state.input.keycodes[in_key] = false;
	}
}

void mouse_button_callback(GLFWwindow* in_window, i32 in_button, i32 in_action, i32 in_mods)
{
	// Lock Mouse on left click into game space
	if (in_button == GLFW_MOUSE_BUTTON_LEFT && in_action == GLFW_PRESS)
	{
		if (!is_mouse_locked())
		{
			set_mouse_locked(true);
		}
	}
}

void cursor_position_callback(GLFWwindow* in_window, f64 in_x, f64 in_y)
{
	static f64 last_x = 0.0;
	static f64 last_y = 0.0;
	static bool has_last_position = false;

	if (has_last_position)
	{
		state.input.mouse_delta.X += (f32)(in_x - last_x);
		state.input.mouse_delta.Y += (f32)(in_y - last_y);
	}

	last_x = in_x;
	last_y = in_y;
	has_last_position = true;
}

void framebuffer_size_callback(GLFWwindow* in_window, i32 in_width, i32 in_height)
{
	state.window.width = in_width;
	state.window.height = in_height;
	state.vk.needs_resize = true;
}

void update_debug_camera(f32 in_delta_time)
{
	if (!state.debug_camera.active || is_key_pressed(GLFW_KEY_LEFT_CONTROL))
	{
		return;
	}

	Camera& camera = state.debug_camera.camera;
	const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));

	f32 move_speed = 10.0f * in_delta_time;
	if (is_key_pressed(GLFW_KEY_LEFT_SHIFT))
	{
		move_speed *= 5.0f;
	}

	if (is_key_pressed(GLFW_KEY_W) || is_key_pressed(GLFW_KEY_UP))
	{
		camera.location += camera.forward * move_speed;
	}
	if (is_key_pressed(GLFW_KEY_S) || is_key_pressed(GLFW_KEY_DOWN))
	{
		camera.location -= camera.forward * move_speed;
	}
	if (is_key_pressed(GLFW_KEY_D) || is_key_pressed(GLFW_KEY_RIGHT))
	{
		camera.location += camera_right * move_speed;
	}
	if (is_key_pressed(GLFW_KEY_A) || is_key_pressed(GLFW_KEY_LEFT))
	{
		camera.location -= camera_right * move_speed;
	}
	if (is_key_pressed(GLFW_KEY_E))
	{
		camera.location += camera.up * move_speed;
	}
	if (is_key_pressed(GLFW_KEY_Q))
	{
		camera.location -= camera.up * move_speed;
	}

	if (is_mouse_locked())
	{
		const f32 look_speed = 1.0f * in_delta_time;
		const HMM_Vec2 mouse_delta = get_mouse_delta();

		camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera.up, -mouse_delta.X * look_speed));
		camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera_right, -mouse_delta.Y * look_speed));
	}
}

// Player camera control: mouse-orbit + follow the controlled object
// (port of game/src/main.cpp:2725-2763)
void update_camera_control(f32 in_delta_time)
{
	if (!state.scene.camera_control_id || state.debug_camera.active)
	{
		return;
	}

	Object& camera_control_object = state.scene.objects[*state.scene.camera_control_id];
	CameraControl& camera_control = camera_control_object.camera_control;
	Camera& camera = camera_control.camera;

	if (is_mouse_locked())
	{
		//FCS TODO: Add max angle property (angle above XY plane) that we can rotate camera
		//FCS TODO: Add rotation speed property to camera control component
		const f32 look_speed = 1.0f * in_delta_time;
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
		HMM_Clamp(0.0f, camera_control.follow_speed * in_delta_time, 1.0f),
		desired_location
	);
}

void frame(f32 in_delta_time)
{
	CPU_TIMING_FRAME("Frame");

	{
		CPU_TIMING_SCOPE("Live Link");
		live_link_drain_channels();
	}

	{
		CPU_TIMING_SCOPE("Camera + Controls");

		// D + Left Control toggle debug camera
		DEFINE_EVENT_TWO_KEYS(GLFW_KEY_D, GLFW_KEY_LEFT_CONTROL,
			if (!state.debug_camera.active) state.debug_camera.camera = get_active_camera();
			state.debug_camera.active = !state.debug_camera.active;
		);

		update_debug_camera(in_delta_time);
		update_camera_control(in_delta_time);
	}

	CPU_TIMING_SCOPE("Rendering");

	if (!vulkan_context_begin_frame(&state.vk))
	{
		reset_mouse_delta();
		return;
	}

	// View + Projection matrix setup
	const Camera& camera = get_active_camera();
	const f32 fov = HMM_AngleDeg(60.0f);
	const f32 aspect_ratio = (f32) state.vk.swapchain_extent.width / (f32) state.vk.swapchain_extent.height;
	const HMM_Mat4 projection_matrix = mat4_perspective(fov, aspect_ratio);

	const HMM_Vec3 target = camera.location + camera.forward * 10;
	const HMM_Mat4 view_matrix = HMM_LookAt_RH(camera.location, target, camera.up);
	const HMM_Mat4 view_projection_matrix = HMM_MulM4(projection_matrix, view_matrix);

	forward_pass_begin(&state.vk);
	for (auto& [unique_id, object] : state.scene.objects)
	{
		if (object.has_mesh && object.visibility)
		{
			forward_pass_draw_mesh(&state.vk, object.mesh, view_projection_matrix, object_get_model_matrix(object));
		}
	}
	forward_pass_end(&state.vk);

	// Debug frame dump for automated visual verification
	static const char* screenshot_path = getenv("GAME2_SCREENSHOT");
	static const char* screenshot_frame_env = getenv("GAME2_SCREENSHOT_FRAME");
	static const u64 screenshot_frame = screenshot_frame_env ? strtoull(screenshot_frame_env, nullptr, 10) : 60;
	if (screenshot_path && state.vk.frame_number == screenshot_frame)
	{
		state.vk.pending_frame_dump = screenshot_path;
	}

	vulkan_context_end_frame(&state.vk);

	reset_mouse_delta();
}

int main(int argc, char** argv)
{
	// Unbuffered stdout so logs survive crashes and external kills
	setvbuf(stdout, nullptr, _IONBF, 0);

	cxxopts::Options options("Game2", "Game that uses blender as its tooling (Vulkan)");

	options.add_options()
		("f,file", "File name", cxxopts::value<std::string>())
		("p,port", "Live link TCP port", cxxopts::value<std::string>()->default_value("65432"))
	;

	// First positional arg can be file to load
	options.parse_positional({"file"});

	auto args = options.parse(argc, argv);

	// If we passed an init file, load it on startup
	if (args.count("file") > 0)
	{
		state.runtime.init_file = args["f"].as<std::string>();
	}
	state.live_link.port = args["port"].as<std::string>();

	if (!glfwInit())
	{
		printf("Failed to initialize GLFW\n");
		return 1;
	}

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWwindow* window = glfwCreateWindow(state.window.width, state.window.height, "Blender Game 2", nullptr, nullptr);
	if (!window)
	{
		printf("Failed to create GLFW window\n");
		return 1;
	}
	state.window.handle = window;

	glfwSetKeyCallback(window, key_callback);
	glfwSetMouseButtonCallback(window, mouse_button_callback);
	glfwSetCursorPosCallback(window, cursor_position_callback);
	glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

	vulkan_context_init(&state.vk, window);
	forward_pass_init(&state.vk);

	// If we passed an init file, load it as a flatbuffer Update on startup
	// (port of game/src/main.cpp:1666-1684)
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
			fclose(file);

			parse_flatbuffer_data(flatbuffer_data);
		}
		else
		{
			printf("Failed to open init file: %s\n", state.runtime.init_file->c_str());
		}
	}

	// Start Live Link Server (Blender connects to 127.0.0.1:<port>)
	state.live_link.thread = std::thread(live_link_thread_function);

	f64 last_frame_time = glfwGetTime();

	while (!glfwWindowShouldClose(window))
	{
		glfwPollEvents();

		const f64 current_time = glfwGetTime();
		const f32 delta_time = (f32)(current_time - last_frame_time);
		last_frame_time = current_time;

		// Debug: exercise swapchain recreation without manual window dragging
		static const bool test_resize = getenv("GAME2_TEST_RESIZE") != nullptr;
		if (test_resize && state.vk.frame_number == 30)
		{
			glfwSetWindowSize(window, 1280, 720);
		}

		frame(delta_time);
	}

	// Tell live_link_thread we're done running and wait for it to complete.
	// Note: like game/, the thread blocks in accept() until Blender's first
	// connection, so quitting before any connection waits on that accept.
	state.runtime.game_running = false;
	state.live_link.thread.join();

	vkDeviceWaitIdle(state.vk.device);

	for (auto& [unique_id, object] : state.scene.objects)
	{
		object_cleanup(object);
	}
	state.scene.objects.clear();

	forward_pass_shutdown(&state.vk);
	vulkan_context_shutdown(&state.vk);

	glfwDestroyWindow(window);
	glfwTerminate();
	return 0;
}
