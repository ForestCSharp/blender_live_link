#pragma once

#include <map>
#include <optional>
#include <string>
#include <thread>

#include "core/types.h"
#include "network/channel.h"
#include "network/socket_wrapper.h"
#include "game_object/camera.h"
#include "game_object/game_object.h"
#include "render/vulkan_context.h"

// Global game state, in the shape of game/'s State struct
struct State
{
	struct RuntimeState
	{
		bool game_running = true;
		bool blender_data_loaded = false;
		std::optional<std::string> init_file;
	} runtime;

	struct WindowState
	{
		GLFWwindow* handle = nullptr;
		i32 width = 1920;
		i32 height = 1080;
	} window;

	struct InputState
	{
		bool keycodes[GLFW_KEY_LAST + 1] = {};
		HMM_Vec2 mouse_delta = HMM_V2(0.0f, 0.0f);
		bool is_mouse_locked = false;
	} input;

	struct SceneState
	{
		std::map<i32, Object> objects;
		std::optional<i32> camera_control_id;
	} scene;

	struct LiveLinkState
	{
		std::string port = "65432";
		std::thread thread;

		SOCKET blender_socket = socket_invalid();
		SOCKET connection_socket = socket_invalid();

		Channel<Object> updated_objects;
		Channel<i32> deleted_objects;
		Channel<bool> reset;
	} live_link;

	struct DebugCameraState
	{
		bool active = true;
		Camera camera = {
			.location = HMM_V3(2.5f, -15.0f, 3.0f),
			.forward = HMM_NormV3(HMM_V3(0.0f, 1.0f, -0.5f)),
			.up = HMM_NormV3(HMM_V3(0.0f, 0.0f, 1.0f)),
		};
	} debug_camera;

	VulkanContext vk;
} state;

bool is_key_pressed(i32 in_keycode)
{
	return state.input.keycodes[in_keycode];
}

HMM_Vec2 get_mouse_delta()
{
	return state.input.mouse_delta;
}

void reset_mouse_delta()
{
	state.input.mouse_delta = HMM_V2(0.0f, 0.0f);
}

bool is_mouse_locked()
{
	return state.input.is_mouse_locked;
}

void set_mouse_locked(bool in_locked)
{
	state.input.is_mouse_locked = in_locked;
	glfwSetInputMode(
		state.window.handle,
		GLFW_CURSOR,
		in_locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL
	);
}
