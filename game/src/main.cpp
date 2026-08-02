//
// Golden-path Vulkan (MoltenVK) + Volk + VMA + GLFW live-link game.
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <limits>
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

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI
	#define IMGUI_IMPLEMENTATION
	#include "imgui/misc/single_file/imgui_single_file.h"
	#include "imgui/backends/imgui_impl_glfw.cpp"
	#include "imgui/backends/imgui_impl_vulkan.cpp"
#endif

// Generated flatbuffer schema (from ../compiled_schemas/cpp)
#include "blender_live_link_generated.h"

#include "core/types.h"
#include "core/timings.h"
#include "render/vulkan_context.h"
#include "render/render_types.h"
#include "render/gpu_buffer.h"
#include "game_object/game_object.h"
#include "state/state.h"
#include "game_object/attachment_point.h"
#include "game_object/mech.h"
#include "core/benchmark.h"
#include "core/runtime_state_overrides.h"
#include "render/geometry_pass.h"
#include "render/shadow_depth_pass.h"
#include "render/shadow_blur_pass.h"
#include "render/shadow_cascade_debug_pass.h"
#include "render/ssao_pass.h"
#include "render/blur_pass.h"
#include "render/screen_space_shadows_pass.h"
#include "render/fog_pass.h"
#include "render/dof_combine_pass.h"
#include "render/wire_overlay_pass.h"
#include "render/temporal_aa_pass.h"
#include "render/fxaa_pass.h"
#include "render/gpu_skinning.h"
#include "render/tessellation.h"
#include "render/lighting_capture.h"
#include "render/gi.h"
#include "render/gi_debug_pass.h"
#include "render/lighting_pass.h"
#include "render/tonemapping_pass.h"
#include "render/sky_pass.h"
#include "render/copy_to_swapchain_pass.h"
#include "automation/automated_screenshot.h"
#include "animation/animation_system.h"
#include "input/input_api.h"

#include "render/imgui_layer.h"
#include "input/input_system.h"
#include "live_link/live_link_system.h"
#include "render/render_system.h"

static AutomatedScreenshot automated_screenshot;

// Copies Jolt body transforms back into object transforms every frame.
// This is a no-op while paused because bodies do not move.
// The object transforms therefore remain unchanged while paused.
void update_physics_backed_object_transforms()
{
	JPH::BodyInterface& body_interface = jolt_state.physics_system.GetBodyInterface();
	for (auto& [unique_id, object] : state.scene.objects)
	{
		object_copy_physics_transform(object, body_interface);
	}
}

// Picks the active fog controller: the lowest-uid enabled and visible
// controller supplies the fog render pass parameters.
// Selection is refreshed after live-link updates.
void refresh_active_fog_controller()
{
	const std::optional<i32> previous_id = state.fog.active_fog_controller_id;

	state.fog.active_fog_controller_id.reset();
	state.fog.active = false;

	i32 selected_uid = std::numeric_limits<i32>::max();
	for (auto& [unique_id, object] : state.scene.objects)
	{
		if (!object.has_fog_controller || !object.fog_controller.enabled || !object.visibility)
		{
			continue;
		}

		if (unique_id < selected_uid)
		{
			selected_uid = unique_id;
			state.fog.active_fog_controller_id = unique_id;
			state.fog.active = true;
		}
	}

	if (state.fog.active_fog_controller_id != previous_id)
	{
		if (state.fog.active_fog_controller_id)
		{
			printf("Active fog controller: UID %i\n", *state.fog.active_fog_controller_id);
		}
		else
		{
			printf("Active fog controller: none\n");
		}
	}
}

// Keeps state.scene.primary_sun_id pointing at a valid sun object, rescanning
// the light index when the cached id goes stale.
void refresh_primary_sun_id()
{
	if (state.scene.primary_sun_id)
	{
		auto found = state.scene.objects.find(*state.scene.primary_sun_id);
		if (found != state.scene.objects.end() && object_is_sun_light(found->second))
		{
			return;
		}
		state.scene.primary_sun_id.reset();
	}

	scene_ensure_indexes(state);
	i32 selected_uid = std::numeric_limits<i32>::max();
	for (i32 light_object_id : state.scene.indexes.light_object_ids)
	{
		auto found = state.scene.objects.find(light_object_id);
		if (found != state.scene.objects.end() && object_is_sun_light(found->second))
		{
			selected_uid = MIN(selected_uid, light_object_id);
		}
	}
	if (selected_uid != std::numeric_limits<i32>::max())
	{
		state.scene.primary_sun_id = selected_uid;
	}
}

void frame(f32 in_delta_time)
{
	CPU_TIMING_FRAME("Frame");
	data_oriented_begin_frame(state);

	state.debug_ui.immediate_frame_time_ms = in_delta_time * 1000.0f;
	state.debug_ui.immediate_fps = in_delta_time > 0.0f ? 1.0f / in_delta_time : 0.0f;

	f64 immediate_cpu_time_ms = 0.0;
	state.debug_ui.immediate_cpu_time_valid = cpu_timings_get_latest_frame_total_ms(immediate_cpu_time_ms);
	if (state.debug_ui.immediate_cpu_time_valid)
	{
		state.debug_ui.immediate_cpu_time_ms = (f32) immediate_cpu_time_ms;
		state.debug_ui.cpu_time_sample_sum_ms += immediate_cpu_time_ms;
		state.debug_ui.cpu_time_sample_count += 1;
		if (!state.debug_ui.cpu_time_valid)
		{
			state.debug_ui.cpu_time_ms = (f32) immediate_cpu_time_ms;
			state.debug_ui.cpu_time_valid = true;
		}
	}

	f64 immediate_gpu_time_ms = 0.0;
	bool immediate_gpu_time_pending = false;
	state.debug_ui.immediate_gpu_time_valid = gpu_timings_get_latest_completed_frame_total_ms(immediate_gpu_time_ms, immediate_gpu_time_pending);
	state.debug_ui.immediate_gpu_time_pending = !state.debug_ui.immediate_gpu_time_valid && immediate_gpu_time_pending;
	if (state.debug_ui.immediate_gpu_time_valid)
	{
		state.debug_ui.immediate_gpu_time_ms = (f32) immediate_gpu_time_ms;
		state.debug_ui.gpu_time_sample_sum_ms += immediate_gpu_time_ms;
		state.debug_ui.gpu_time_sample_count += 1;
		if (!state.debug_ui.gpu_time_valid)
		{
			state.debug_ui.gpu_time_ms = (f32) immediate_gpu_time_ms;
			state.debug_ui.gpu_time_valid = true;
			state.debug_ui.gpu_time_pending = false;
		}
	}
	else if (state.debug_ui.immediate_gpu_time_pending)
	{
		state.debug_ui.gpu_time_pending = !state.debug_ui.gpu_time_valid;
	}
	else
	{
		state.debug_ui.gpu_time_valid = false;
		state.debug_ui.gpu_time_pending = false;
	}

	state.debug_ui.stats_sample_elapsed += in_delta_time;
	state.debug_ui.stats_sample_count += 1;
	if (state.debug_ui.fps == 0.0f && in_delta_time > 0.0f)
	{
		state.debug_ui.frame_time_ms = in_delta_time * 1000.0f;
		state.debug_ui.fps = 1.0f / in_delta_time;
	}
	if (state.debug_ui.stats_sample_elapsed >= 0.25)
	{
		const f64 average_delta_time = state.debug_ui.stats_sample_elapsed / state.debug_ui.stats_sample_count;
		state.debug_ui.frame_time_ms = (f32) (average_delta_time * 1000.0);
		state.debug_ui.fps = (f32) (state.debug_ui.stats_sample_count / state.debug_ui.stats_sample_elapsed);
		if (state.debug_ui.cpu_time_sample_count > 0)
		{
			state.debug_ui.cpu_time_ms = (f32) (state.debug_ui.cpu_time_sample_sum_ms / state.debug_ui.cpu_time_sample_count);
			state.debug_ui.cpu_time_valid = true;
		}
		if (state.debug_ui.gpu_time_sample_count > 0)
		{
			state.debug_ui.gpu_time_ms = (f32) (state.debug_ui.gpu_time_sample_sum_ms / state.debug_ui.gpu_time_sample_count);
			state.debug_ui.gpu_time_valid = true;
			state.debug_ui.gpu_time_pending = false;
		}
		else if (!state.debug_ui.gpu_time_valid)
		{
			state.debug_ui.gpu_time_pending = state.debug_ui.immediate_gpu_time_pending;
		}
		state.debug_ui.stats_sample_elapsed = 0.0;
		state.debug_ui.stats_sample_count = 0;
		state.debug_ui.cpu_time_sample_sum_ms = 0.0;
		state.debug_ui.cpu_time_sample_count = 0;
		state.debug_ui.gpu_time_sample_sum_ms = 0.0;
		state.debug_ui.gpu_time_sample_count = 0;
	}

	if (!RenderSystem::begin_frame(state))
	{
		InputSystem::reset_mouse_delta(state);
		return;
	}

	{
		CPU_TIMING_SCOPE("Live Link");
		LiveLinkSystem::drain(state);
	}
	automated_screenshot.begin_frame(state, RenderSystem::gi_scene());

	{
		CPU_TIMING_SCOPE("Camera + Controls");
		InputSystem::update_controls(state, in_delta_time, automated_screenshot.enabled());
		if (InputSystem::consume_gi_probe_pick_request(state))
		{
			RenderSystem::pick_isolated_gi_probe(state);
		}
	}

	{
		CPU_TIMING_SCOPE("Simulation");
		if (state.runtime.is_simulating)
		{
			InputSystem::update_player_character(state, in_delta_time);
			jolt_update(in_delta_time);
		}
	}

	{
		CPU_TIMING_SCOPE("Skinned Animation Advance");
		AnimationSystem::advance(state, in_delta_time);
	}

	{
		CPU_TIMING_SCOPE("Object Transforms");
		update_physics_backed_object_transforms();
		update_mech_transforms();
		scene_ensure_indexes(state);
		refresh_primary_sun_id();
		refresh_active_fog_controller();
		build_render_object_snapshot(state);
		pack_lights(state);
		upload_lights(state);
	}

	{
		CPU_TIMING_SCOPE("Skinned Animation Pack");
		AnimationSystem::pack_skin_matrices(state);
	}

	RenderSystem::render(state);
	automated_screenshot.queue_if_ready(state);

	RenderSystem::end_frame(state);
	automated_screenshot.after_frame(state);

	InputSystem::reset_mouse_delta(state);
}

int main(int argc, char** argv)
{
	// Unbuffered stdout so logs survive crashes and external kills
	setvbuf(stdout, nullptr, _IONBF, 0);

	cxxopts::Options options("Game", "Game that uses Blender as its tooling (Vulkan)");

	options.add_options()
		("f,file", "File name", cxxopts::value<std::string>())
		("p,port", "Live link TCP port", cxxopts::value<std::string>()->default_value("65432"))
		("no-live-link", "Do not start the live-link server", cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
		("warmup-frames", "Benchmark warmup frame count", cxxopts::value<u64>()->default_value("300"))
		("benchmark-frames", "Measured frame count; providing this enables benchmark mode", cxxopts::value<u64>())
		("benchmark-output", "Benchmark JSON output path", cxxopts::value<std::string>()->default_value("benchmark.json"))
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
	const bool no_live_link = args["no-live-link"].as<bool>();
	BenchmarkState benchmark;
	if (args.count("benchmark-frames") > 0)
	{
		benchmark.configure(
			args["warmup-frames"].as<u64>(),
			args["benchmark-frames"].as<u64>(),
			args["benchmark-output"].as<std::string>()
		);
		state.debug_ui.visible = false;
	}

	InputSystem::install_error_callback();
	if (!glfwInit())
	{
		printf("Failed to initialize GLFW\n");
		return 1;
	}
	if (!automated_screenshot.configure(state))
	{
		glfwTerminate();
		return 1;
	}

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWwindow* window = glfwCreateWindow(state.window.width, state.window.height, "Blender Game", nullptr, nullptr);
	if (!window)
	{
		printf("Failed to create GLFW window\n");
		return 1;
	}
	state.window.handle = window;

	InputSystem::install_callbacks(window);

	jolt_init();

	RuntimeStateOverrides::apply(state);

	RenderSystem::initialize(state, window);

	// If an init file was provided, load it as a FlatBuffer Update on startup.
	if (state.runtime.init_file)
	{
		LiveLinkSystem::load_initial_file(state, *state.runtime.init_file);
	}

	// Start Live Link Server (Blender connects to 127.0.0.1:<port>). Offline
	// benchmark runs use a captured --file update and skip the socket thread.
	if (!no_live_link)
	{
		LiveLinkSystem::start(state);
	}

	f64 last_frame_time = glfwGetTime();
	benchmark.begin(&state.vk);

	while (!glfwWindowShouldClose(window))
	{
		glfwPollEvents();

		const f64 current_time = glfwGetTime();
		const f32 delta_time = (f32)(current_time - last_frame_time);
		last_frame_time = current_time;

		// Debug: exercise swapchain recreation without manual window dragging
		if (RuntimeConfig::get().test_resize && state.vk.frame_number == 30)
		{
			glfwSetWindowSize(window, 1280, 720);
		}

		const f64 frame_start_time = glfwGetTime();
		frame(delta_time);
		const f64 frame_end_time = glfwGetTime();
		benchmark.after_frame((frame_end_time - frame_start_time) * 1000.0, &state.vk);
		if (benchmark.should_exit())
		{
			glfwSetWindowShouldClose(window, GLFW_TRUE);
		}
		if (automated_screenshot.finished())
		{
			glfwSetWindowShouldClose(window, GLFW_TRUE);
		}
	}
	if (automated_screenshot.enabled() && !automated_screenshot.finished())
	{
		automated_screenshot.fail("window closed before capture completed");
	}
	benchmark_finalize(benchmark, &state.vk);

	// Tell the Live Link thread we're done and wait for it to complete.
	if (!no_live_link)
	{
		LiveLinkSystem::stop(state);
	}
	else
	{
		state.runtime.game_running = false;
	}

	VK_CHECK(vulkan_device_wait_idle(&state.vk));
	mech_reset_all();
	scene_clear_objects(state);
	LiveLinkSystem::cleanup_imported_resources(state);
	jolt_shutdown();
	RenderSystem::shutdown(state);

	glfwDestroyWindow(window);
	glfwTerminate();
	return automated_screenshot.failed() ? 1 : 0;
}
