#pragma once

#include <atomic>
#include <optional>

#include "ankerl/unordered_dense.h"
#include "core/stretchy_buffer.h"
#include "game_object/game_object.h"
#include "network/channel.h"
#include "network/socket_wrapper.h"
#include "render/gpu_image.h"
#include "render/render_pass.h"

#include "sokol/sokol_gfx.h"
#include "render/shader_files.h"

using ankerl::unordered_dense::map;
using std::atomic;
using std::optional;

enum class ERenderPass : int
{
	Geometry,
	SSAO,
	SSAO_Blur,
	Lighting,
	DOF_Blur,
	DOF_Combine,
	Tonemapping,
	DebugText,
	CopyToSwapchain,
	COUNT,
};


enum class EProbeVisMode : i32
{
	Irradiance = 0,
	RadialDepth = 1,
	MAX,
};

const char* EProbeVisModeNames[(i32)EProbeVisMode::MAX] = {
	"Irradiance",
	"RadialDepth",
};

//FCS TODO: Make this not a global. Just init it early in main() but then pass it around

struct State 
{
	optional<std::string> init_file;

	int width;
	int height;

	// Render Passes
	RenderPass render_passes[(int)ERenderPass::COUNT];

	// Rendering Feature Flags
	bool ssao_enable = true;
	bool sky_rendering_enable = true;
	bool direct_lighting_enable = true;
	bool gi_enable = true;
	bool gi_probe_occlusion = true;
	float gi_intensity = 1.0f;
	bool dof_enable = true;
	bool show_probes = true;
	EProbeVisMode probe_vis_mode = EProbeVisMode::Irradiance;

	// This is true when we are updating probes, and is set to false after all probes have been re-updated
	bool gi_is_updating = true;

	// SSAO Data 
	GpuImage ssao_noise_texture;
	ssao_fs_params_t ssao_fs_params;

	// Texture Sampler
	sg_sampler linear_sampler;
	sg_sampler nearest_sampler;

	atomic<bool> game_running = true;
	atomic<bool> blender_data_loaded = false;

	// Active game objects
	map<i32, Object> objects;

	// ID of player character
	optional<i32> player_character_id;

	// ID of camera controller
	optional<i32> camera_control_id;

	// ID of primary sun light
	optional<i32> primary_sun_id;

	// Are we currently simulating
	bool is_simulating = true;

	// These channels are how we pass our data from our live-link thread to the main thread
	Channel<Object> updated_objects;
	Channel<i32> deleted_objects;
	Channel<bool> reset;

	// Lighting fragment shader params
	lighting_fs_params_t lighting_fs_params;
	tonemapping_fs_params_t tonemapping_fs_params = {
		.exposure_bias = 1.5,
	};

	// Contains Lights Data packed up for gpu usage
	bool needs_light_data_update = true;	

	// Point Lights
	StretchyBuffer<lighting_PointLight_t> point_lights;
	GpuBuffer<lighting_PointLight_t> point_lights_buffer;

	// Spot Lights
	StretchyBuffer<lighting_SpotLight_t> spot_lights;
	GpuBuffer<lighting_SpotLight_t> spot_lights_buffer;

	// Sun (Directional) Lights
	StretchyBuffer<lighting_SunLight_t> sun_lights;
	GpuBuffer<lighting_SunLight_t> sun_lights_buffer;

	// Materials
	map<i32,i32> material_id_to_index;
	StretchyBuffer<geometry_Material_t> materials;
	GpuBuffer<geometry_Material_t> materials_buffer;

	// Images
	map<i32,i32> image_id_to_index;
	StretchyBuffer<GpuImage> images;
	GpuImage default_image;
	GpuImage default_image_cube;
	GpuBuffer<u8> default_buffer;

	bool enable_debug_image_fullscreen = false;
	i32 debug_image_index = 0;
	
	// Thread that listens for updates from Blender
	std::thread live_link_thread;

	SOCKET blender_socket = socket_invalid();
	SOCKET connection_socket = socket_invalid();

	bool debug_camera_active = true;
	Camera debug_camera = {
		.location = HMM_V3(2.5f, -15.0f, 3.0f),
		.forward = HMM_NormV3(HMM_V3(0.0f, 1.0f, -0.5f)),
		.up = HMM_NormV3(HMM_V3(0.0f, 0.0f, 1.0f)),
	};
} state;

void state_init()
{
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

	HMM_Vec4 default_image_cube_data[6] =
	{
		HMM_V4(0,0,0,1),
		HMM_V4(0,0,0,1),
		HMM_V4(0,0,0,1),
		HMM_V4(0,0,0,1),
		HMM_V4(0,0,0,1),
		HMM_V4(0,0,0,1),
	};
	GpuImageDesc default_image_cube_desc = {
		.type = SG_IMAGETYPE_CUBE,
		.width = 1,
		.height = 1,
		.num_slices = 6,
		.pixel_format = SG_PIXELFORMAT_RGBA32F,
		.data = (u8*) &default_image_cube_data,
	};
	state.default_image_cube = GpuImage(default_image_cube_desc);

	u8 default_buffer_data[4] = { 0,0,0,0 };
	GpuBufferDesc<u8> default_buffer_desc = {
		.data = default_buffer_data,
		.size = 4, 
		.usage = {
			.storage_buffer = true,
		},
		.label = "default_buffer",
	};
	state.default_buffer = GpuBuffer<u8>(default_buffer_desc);

	state.linear_sampler = sg_make_sampler((sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });

	state.nearest_sampler = sg_make_sampler((sg_sampler_desc){
        .min_filter = SG_FILTER_NEAREST,
        .mag_filter = SG_FILTER_NEAREST,
		.mipmap_filter = SG_FILTER_NEAREST,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
}

//FCS TODO: member methods on state once it's not a global
const i32 MAX_MATERIALS = 1024;
void init_materials_buffer()
{
	if (!state.materials_buffer.is_gpu_buffer_valid())
	{
		state.materials_buffer = GpuBuffer((GpuBufferDesc<geometry_Material_t>){
			.data = nullptr,
			.size = sizeof(geometry_Material_t) * MAX_MATERIALS,
			.usage = {
				.storage_buffer = true,
				.stream_update = true,
			},
			.label = "materials",
		});
	}
}

GpuBuffer<geometry_Material_t>& get_materials_buffer()
{
	init_materials_buffer();
	return state.materials_buffer;
}

void update_materials_buffer()
{
	// Write update to materials buffer
	get_materials_buffer().update_gpu_buffer(
		(sg_range){
			.ptr = state.materials.data(),
			.size = sizeof(geometry_Material_t) * state.materials.length(),
		}
	);	
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
