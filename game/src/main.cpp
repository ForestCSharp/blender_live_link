//
// Golden-path Vulkan (MoltenVK) + Volk + VMA + GLFW live-link game.
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
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

#include "core/dynamic_array.h"

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
#include "render/core/vulkan_context.h"
#include "render/core/render_types.h"
#include "render/core/gpu_buffer.h"
#include "game_object/game_object.h"
#include "state/state.h"
#include "game_object/attachment_point.h"
#include "game_object/mech.h"
#include "core/benchmark.h"
#include "core/runtime_state_overrides.h"
#include "render/passes/geometry/geometry_pass.h"
#include "render/passes/shadows/shadow_depth_pass.h"
#include "render/passes/shadows/shadow_blur_pass.h"
#include "render/passes/shadows/shadow_cascade_debug_pass.h"
#include "render/passes/ssao/ssao_pass.h"
#include "render/passes/blur/blur_pass.h"
#include "render/passes/shadows/screen_space_shadows_pass.h"
#include "render/passes/fog/fog_pass.h"
#include "render/passes/dof/dof_combine_pass.h"
#include "render/passes/wire_overlay/wire_overlay_pass.h"
#include "render/passes/temporal_aa/temporal_aa_pass.h"
#include "render/passes/fxaa/fxaa_pass.h"
#include "render/passes/gpu_skinning/gpu_skinning.h"
#include "render/passes/tessellation/tessellation.h"
#include "render/passes/gi/lighting_capture.h"
#include "render/passes/gi/gi.h"
#include "render/passes/gi/gi_debug_pass.h"
#include "render/passes/lighting/lighting_pass.h"
#include "render/passes/bloom/bloom_pass.h"
#include "render/passes/tonemapping/tonemapping_pass.h"
#include "render/passes/sky/sky_pass.h"
#include "render/passes/copy_to_swapchain/copy_to_swapchain_pass.h"
#include "automation/automated_screenshot.h"
#include "animation/animation_system.h"

#include "render/passes/imgui/imgui_layer.h"
#include "input/input_system.h"
#include "live_link/live_link_system.h"
#include "render/render_system.h"
#include "scene/scene_system.h"
#include "ui/debug_ui_system.h"

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

void frame(f32 in_delta_time)
{
	CPU_TIMING_FRAME("Frame");
	data_oriented_begin_frame(state);
	DebugUiSystem::update_frame_stats(state, in_delta_time);

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
		SceneSystem::refresh_derived_state(state);
	}

	// Must precede build_render_object_snapshot: ObjectData now carries
	// skin_matrix_arena_offset, which this fills in. Still after
	// update_mech_transforms so newly spawned runtime parts are indexed.
	{
		CPU_TIMING_SCOPE("Skinned Animation Pack");
		AnimationSystem::pack_skin_matrices(state);
	}

	{
		CPU_TIMING_SCOPE("Render Object Snapshot");
		build_render_object_snapshot(state);
		pack_lights(state);
		upload_lights(state);
		geometry_arena_sync(state);
	}

	RenderSystem::render(state, in_delta_time);
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
		("p,port", "Live link TCP port (default: $BLENDER_LIVE_LINK_PORT, else 65432)", cxxopts::value<std::string>())
		("no-live-link", "Do not start the live-link server", cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
		("warmup-frames", "Benchmark warmup frame count", cxxopts::value<u64>()->default_value("300"))
		("benchmark-frames", "Measured frame count; providing this enables benchmark mode", cxxopts::value<u64>())
		("benchmark-output", "Benchmark JSON output path", cxxopts::value<std::string>()->default_value("benchmark.json"))
		("fullscreen", "Use the primary monitor in fullscreen mode", cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
	;

	// First positional arg can be file to load
	options.parse_positional({"file"});

	auto args = options.parse(argc, argv);

	// If we passed an init file, load it on startup
	if (args.count("file") > 0)
	{
		state.runtime.init_file = args["f"].as<std::string>();
	}
	// --port beats $BLENDER_LIVE_LINK_PORT beats the built-in default. Resolved
	// on the main thread before anything starts, so a bad value fails the
	// process outright instead of only taking down the live link thread.
	{
		const char* environment_port = getenv("BLENDER_LIVE_LINK_PORT");
		std::string port = "65432";
		if (args.count("port") > 0)
		{
			port = args["port"].as<std::string>();
		}
		else if (environment_port != nullptr && environment_port[0] != '\0')
		{
			port = environment_port;
		}
		char* parse_end = nullptr;
		const long port_value = strtol(port.c_str(), &parse_end, 10);
		if (parse_end == port.c_str() || *parse_end != '\0' || port_value < 1 || port_value > 65535)
		{
			printf("Live link port must be between 1 and 65535, got \"%s\"%s\n", port.c_str(),
				args.count("port") > 0 ? "" : " (from $BLENDER_LIVE_LINK_PORT)");
			return 1;
		}
		state.live_link.port = port;
	}
	const bool no_live_link = args["no-live-link"].as<bool>();
	const bool fullscreen = args["fullscreen"].as<bool>();
	BenchmarkState benchmark;
	if (args.count("benchmark-frames") > 0)
	{
		benchmark.configure(
			args["warmup-frames"].as<u64>(),
			args["benchmark-frames"].as<u64>(),
			args["benchmark-output"].as<std::string>()
		);
		state.debug_ui.visible = false;
		state.runtime.benchmark_active = true;
	}

	InputSystem::install_error_callback();
	if (!glfwInit())
	{
		printf("Failed to initialize GLFW\n");
		return 1;
	}
	if (!automated_screenshot.configure(state, benchmark.enabled))
	{
		glfwTerminate();
		return 1;
	}

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	GLFWmonitor* window_monitor = fullscreen ? glfwGetPrimaryMonitor() : nullptr;
	const GLFWvidmode* fullscreen_mode = window_monitor
		? glfwGetVideoMode(window_monitor) : nullptr;
	GLFWwindow* window = glfwCreateWindow(
		fullscreen_mode ? fullscreen_mode->width : state.window.width,
		fullscreen_mode ? fullscreen_mode->height : state.window.height,
		"Blender Game", window_monitor, nullptr);
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

        // Time and run one frame update
		const f64 frame_start_time = glfwGetTime();
		frame(delta_time);
		const f64 frame_end_time = glfwGetTime();

		benchmark.after_frame((frame_end_time - frame_start_time) * 1000.0, &state.vk);
		if (benchmark.should_exit())
		{
			glfwSetWindowShouldClose(window, GLFW_TRUE);
		}
		if (automated_screenshot.wants_exit())
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
