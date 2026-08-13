#pragma once

#include <cstdio>

#include "animation/animation_system.h"
#include "input/input_api.h"
#include "render/imgui_layer.h"
#include "state/state.h"

namespace InputSystem
{
	enum class Action : i32
	{
		ToggleUi = 0,
		ToggleSimulation,
		ToggleDebugCamera,
		Reset,
	};

	inline bool is_key_pressed(const State& in_state, i32 in_keycode)
	{
		return in_state.input.keycodes[in_keycode];
	}

	inline HMM_Vec2 get_mouse_delta(const State& in_state)
	{
		return in_state.input.mouse_delta;
	}

	inline void reset_mouse_delta(State& in_state)
	{
		in_state.input.mouse_delta = HMM_V2(0.0f, 0.0f);
	}

	inline bool is_mouse_locked(const State& in_state)
	{
		return in_state.input.is_mouse_locked;
	}

	inline void set_mouse_locked(State& in_state, bool in_locked)
	{
		in_state.input.is_mouse_locked = in_locked;
		glfwSetInputMode(
			in_state.window.handle,
			GLFW_CURSOR,
			in_locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL
		);
	}

	inline bool chord_pressed_once(State& in_state, Action in_action, i32 in_key, i32 in_modifier)
	{
		const bool key_pressed = is_key_pressed(in_state, in_key);
		const bool modifier_pressed = in_modifier == 0 || is_key_pressed(in_state, in_modifier);
		bool& was_pressed = in_state.input.action_latches[(i32) in_action];
		const bool pressed = key_pressed && modifier_pressed;
		const bool triggered = pressed && !was_pressed;
		was_pressed = pressed;
		return triggered;
	}

	Camera& active_camera(State& in_state)
	{
		if (in_state.scene.camera_control_id && !in_state.debug_camera.active)
		{
			Object& camera_control_object = in_state.scene.objects[*in_state.scene.camera_control_id];
			return camera_control_object.camera_control.camera;
		}
		return in_state.debug_camera.camera;
	}
	

	// Camera-relative WASD player movement, Shift sprint, Space jump
	// is disabled while the debug camera is active or when no player-controlled
	// character exists.
	void update_player_character(State& in_state, f32 in_delta_time)
	{
		if (!in_state.scene.player_character_id || in_state.debug_camera.active)
		{
			return;
		}
	
		if (!in_state.scene.objects.contains(*in_state.scene.player_character_id))
		{
			return;
		}
	
		const Camera& camera = active_camera(in_state);
		const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));
	
		Object& player_character_object = in_state.scene.objects[*in_state.scene.player_character_id];
		Character& player_character_state = player_character_object.character;
	
		HMM_Vec3 projected_cam_forward = vec3_plane_projection(camera.forward, UnitVectors::Up);
		HMM_Vec3 projected_cam_right = vec3_plane_projection(camera_right, UnitVectors::Up);
	
		if (HMM_LenSqrV3(projected_cam_forward) > 1.0e-6f)
		{
			projected_cam_forward = HMM_NormV3(projected_cam_forward);
		}
		else
		{
			projected_cam_forward = UnitVectors::Forward;
		}
	
		if (HMM_LenSqrV3(projected_cam_right) > 1.0e-6f)
		{
			projected_cam_right = HMM_NormV3(projected_cam_right);
		}
		else
		{
			projected_cam_right = HMM_NormV3(HMM_Cross(projected_cam_forward, UnitVectors::Up));
		}
	
		HMM_Vec3 move_direction = HMM_V3(0, 0, 0);
		if (is_key_pressed(in_state, GLFW_KEY_W))
		{
			move_direction += projected_cam_forward;
		}
		if (is_key_pressed(in_state, GLFW_KEY_S))
		{
			move_direction -= projected_cam_forward;
		}
		if (is_key_pressed(in_state, GLFW_KEY_D))
		{
			move_direction += projected_cam_right;
		}
		if (is_key_pressed(in_state, GLFW_KEY_A))
		{
			move_direction -= projected_cam_right;
		}
	
		if (HMM_LenSqrV3(move_direction) > 1.0f)
		{
			move_direction = HMM_NormV3(move_direction);
		}
	
		if (is_key_pressed(in_state, GLFW_KEY_LEFT_SHIFT))
		{
			move_direction *= 3.0f;
		}
	
		const bool jump = is_key_pressed(in_state, GLFW_KEY_SPACE);
		character_move(player_character_state, move_direction, jump, in_delta_time);
	}

	void key_callback(GLFWwindow* in_window, i32 in_key, i32 in_scancode, i32 in_action, i32 in_mods)
	{
		#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
		if (ImGuiLayer::initialized)
		{
			ImGui_ImplGlfw_KeyCallback(in_window, in_key, in_scancode, in_action, in_mods);
		}
		#endif
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
					set_mouse_locked(state, false);
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
		#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
		if (ImGuiLayer::initialized)
		{
			ImGui_ImplGlfw_MouseButtonCallback(in_window, in_button, in_action, in_mods);
			if (ImGui::GetIO().WantCaptureMouse) { return; }
		}
		#endif
		// Lock Mouse on left click into game space
		if (in_button == GLFW_MOUSE_BUTTON_LEFT && in_action == GLFW_PRESS)
		{
			if (state.gi.probe_isolation_enable && state.gi.show_probes)
			{
				state.input.gi_probe_pick_requested = true;
				return;
			}
			if (!is_mouse_locked(state))
			{
				set_mouse_locked(state, true);
			}
		}
	}
	
	void cursor_position_callback(GLFWwindow* in_window, f64 in_x, f64 in_y)
	{
		#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
		if (ImGuiLayer::initialized)
		{
			ImGui_ImplGlfw_CursorPosCallback(in_window, in_x, in_y);
		}
		#endif
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
		state.input.mouse_position = HMM_V2((f32) in_x, (f32) in_y);
		has_last_position = true;
	}
	
	void char_callback(GLFWwindow* in_window, u32 in_codepoint)
	{
		#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
		if (ImGuiLayer::initialized) { ImGui_ImplGlfw_CharCallback(in_window, in_codepoint); }
		#endif
	}
	
	void scroll_callback(GLFWwindow* in_window, f64 in_x_offset, f64 in_y_offset)
	{
		#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
		if (ImGuiLayer::initialized) { ImGui_ImplGlfw_ScrollCallback(in_window, in_x_offset, in_y_offset); }
		#endif
	}
	
	void cursor_enter_callback(GLFWwindow* in_window, i32 in_entered)
	{
		#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
		if (ImGuiLayer::initialized) { ImGui_ImplGlfw_CursorEnterCallback(in_window, in_entered); }
		#endif
	}
	
	void window_focus_callback(GLFWwindow* in_window, i32 in_focused)
	{
		#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
		if (ImGuiLayer::initialized) { ImGui_ImplGlfw_WindowFocusCallback(in_window, in_focused); }
		#endif
	}
	
	void framebuffer_size_callback(GLFWwindow* in_window, i32 in_width, i32 in_height)
	{
		// The swapchain owns the authoritative framebuffer extent. Keep the old
		// values here so handle_resize() can detect the change after recreation
		// and resize all scaled offscreen targets exactly once.
		state.vk.needs_resize = true;
	}
	
	void glfw_error_callback(i32 in_error, const char* in_description)
	{
		fprintf(stderr, "GLFW error %i: %s\n", in_error, in_description ? in_description : "(no description)");
	}
	
	

	void update_debug_camera(State& in_state, f32 in_delta_time)
	{
		if (!in_state.debug_camera.active || is_key_pressed(in_state, GLFW_KEY_LEFT_CONTROL))
		{
			return;
		}
	
		Camera& camera = in_state.debug_camera.camera;
		const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));
	
		f32 move_speed = in_state.debug_camera.move_speed * in_delta_time;
		if (is_key_pressed(in_state, GLFW_KEY_LEFT_SHIFT))
		{
			move_speed *= 5.0f;
		}
	
		if (is_key_pressed(in_state, GLFW_KEY_W) || is_key_pressed(in_state, GLFW_KEY_UP))
		{
			camera.location += camera.forward * move_speed;
		}
		if (is_key_pressed(in_state, GLFW_KEY_S) || is_key_pressed(in_state, GLFW_KEY_DOWN))
		{
			camera.location -= camera.forward * move_speed;
		}
		if (is_key_pressed(in_state, GLFW_KEY_D) || is_key_pressed(in_state, GLFW_KEY_RIGHT))
		{
			camera.location += camera_right * move_speed;
		}
		if (is_key_pressed(in_state, GLFW_KEY_A) || is_key_pressed(in_state, GLFW_KEY_LEFT))
		{
			camera.location -= camera_right * move_speed;
		}
		if (is_key_pressed(in_state, GLFW_KEY_E))
		{
			camera.location += camera.up * move_speed;
		}
		if (is_key_pressed(in_state, GLFW_KEY_Q))
		{
			camera.location -= camera.up * move_speed;
		}
	
		if (is_mouse_locked(in_state))
		{
			const f32 look_speed = 1.0f * in_delta_time;
			const HMM_Vec2 mouse_delta = get_mouse_delta(in_state);
	
			camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera.up, -mouse_delta.X * look_speed));
			camera.forward = HMM_NormV3(rotate_vector(camera.forward, camera_right, -mouse_delta.Y * look_speed));
		}
	}
	
	// Player camera control orbits around and follows the controlled object.
	// The debug camera takes precedence when active.
	void update_camera_control(State& in_state, f32 in_delta_time)
	{
		if (!in_state.scene.camera_control_id || in_state.debug_camera.active)
		{
			return;
		}
	
		Object& camera_control_object = in_state.scene.objects[*in_state.scene.camera_control_id];
		CameraControl& camera_control = camera_control_object.camera_control;
		Camera& camera = camera_control.camera;
	
		if (is_mouse_locked(in_state))
		{
			//FCS TODO: Add max angle property (angle above XY plane) that we can rotate camera
			//FCS TODO: Add rotation speed property to camera control component
			const f32 look_speed = 1.0f * in_delta_time;
			const HMM_Vec2 mouse_delta = get_mouse_delta(in_state);
	
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

	inline void install_error_callback()
	{
		glfwSetErrorCallback(glfw_error_callback);
	}

	inline void install_callbacks(GLFWwindow* in_window)
	{
		glfwSetKeyCallback(in_window, key_callback);
		glfwSetMouseButtonCallback(in_window, mouse_button_callback);
		glfwSetCursorPosCallback(in_window, cursor_position_callback);
		glfwSetCharCallback(in_window, char_callback);
		glfwSetScrollCallback(in_window, scroll_callback);
		glfwSetCursorEnterCallback(in_window, cursor_enter_callback);
		glfwSetWindowFocusCallback(in_window, window_focus_callback);
		glfwSetFramebufferSizeCallback(in_window, framebuffer_size_callback);
	}

	inline bool consume_gi_probe_pick_request(State& in_state)
	{
		const bool requested = in_state.input.gi_probe_pick_requested;
		in_state.input.gi_probe_pick_requested = false;
		return requested;
	}

	inline void update_controls(State& in_state, f32 in_delta_time, bool in_automation_enabled)
	{
		if (chord_pressed_once(in_state, Action::ToggleUi, GLFW_KEY_I, GLFW_KEY_LEFT_CONTROL))
		{
			in_state.debug_ui.visible = !in_state.debug_ui.visible;
		}

		#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
		const bool ui_captures_keyboard = ImGui::GetIO().WantCaptureKeyboard;
		const bool ui_captures_mouse = ImGui::GetIO().WantCaptureMouse;
		#else
		const bool ui_captures_keyboard = false;
		const bool ui_captures_mouse = false;
		#endif

		if (in_automation_enabled)
		{
			return;
		}

		if (!ui_captures_keyboard)
		{
			if (chord_pressed_once(in_state, Action::ToggleSimulation, GLFW_KEY_SPACE, GLFW_KEY_LEFT_CONTROL))
			{
				in_state.runtime.is_simulating = !in_state.runtime.is_simulating;
			}
			if (chord_pressed_once(in_state, Action::ToggleDebugCamera, GLFW_KEY_D, GLFW_KEY_LEFT_CONTROL))
			{
				if (!in_state.debug_camera.active)
				{
					in_state.debug_camera.camera = active_camera(in_state);
					in_state.debug_camera.initial_location = in_state.debug_camera.camera.location;
				}
				in_state.debug_camera.active = !in_state.debug_camera.active;
			}
			if (chord_pressed_once(in_state, Action::Reset, GLFW_KEY_R, GLFW_KEY_LEFT_CONTROL))
			{
				for (auto& [reset_uid, reset_object] : in_state.scene.objects)
				{
					reset_object.current_transform = reset_object.initial_transform;
					if (reset_object.has_rigid_body && reset_object.rigid_body.jolt_body != nullptr)
					{
						object_reset_jolt_body(reset_object);
					}
				}
				AnimationSystem::rewind(in_state);
			}
		}

		if (is_mouse_locked(in_state) || (!ui_captures_mouse && !ui_captures_keyboard))
		{
			update_debug_camera(in_state, in_delta_time);
			update_camera_control(in_state, in_delta_time);
		}
	}
}
