
#include <atomic>
using std::atomic;

#include <cerrno>
#include <cstdio>

#include <optional>
using std::optional;

#include <thread>

#include "ankerl/unordered_dense.h"
using ankerl::unordered_dense::map;

#define HANDMADE_MATH_IMPLEMENTATION
#define HANDMADE_MATH_NO_SSE
#include "handmade_math/HandmadeMath.h"

#define STB_DS_IMPLEMENTATION
#include "stb/stb_ds.h"
#define sbuffer(type) type*

#include "sokol/sokol_gfx.h"
#include "sokol/sokol_app.h"
#include "sokol/sokol_log.h"
#include "sokol/sokol_glue.h"

#include "sokol/util/sokol_debugtext.h"
#define FONT_KC853 (0)
#define FONT_KC854 (1)
#define FONT_Z1013 (2)
#define FONT_CPC   (3)
#define FONT_C64   (4)
#define FONT_ORIC  (5)

//FCS TODO: Make crossplat
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>

// Flatbuffers generated file
#include "blender_live_link_generated.h"

// Flatbuffer helper functions
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

#include "cube-sapp.glsl.h"

// Primitive Typedefs
#include <cstdint>
typedef int32_t 	i32;
typedef int16_t		i16;
typedef int8_t		i8;
typedef uint32_t 	u32;
typedef uint16_t 	u16;
typedef uint8_t 	u8;

struct Mesh {
	u32 idx_count;
	sg_buffer index_buffer; 
	sg_buffer vertex_buffer; 
};

struct Vertex
{
	HMM_Vec4 position;
	HMM_Vec4 normal;
};

Mesh make_mesh(
	Vertex* vertices, 
	u32 vertices_len, 
	u32* indices, 
	u32 indices_len
)
{
	sg_buffer index_buffer = sg_make_buffer((sg_buffer_desc){
		.type = SG_BUFFERTYPE_INDEXBUFFER,
		.data = {
			.ptr = indices,
			.size = indices_len * sizeof(u32)
		},
		.label = "mesh-indices"
	});

	sg_buffer vertex_buffer = sg_make_buffer((sg_buffer_desc){
		.data = {
			.ptr = vertices,
			.size = vertices_len * sizeof(Vertex)
		},
		.label = "mesh-vertices"
	});

	return (Mesh) {
		.idx_count = indices_len,
		.index_buffer = index_buffer,
		.vertex_buffer = vertex_buffer,
	};
}

enum class LightType : u8
{
	Point	= 0,
	Sun 	= 1,
	Spot 	= 2,
	Area	= 3,
};

// Make sure this stays in sync with value in cube-sapp.glsl
#define MAX_POINT_LIGHTS 100

struct PointLight {
	float energy;
};

struct Light 
{
	LightType type;
	HMM_Vec3 color;	
	union
	{
		PointLight point;
	};
};

struct Object 
{
	i32 unique_id;
	bool visibility;

	// Object's world location
	HMM_Vec4 location;

	sg_buffer storage_buffer; 

	// Mesh Data, stored inline
	bool has_mesh;
	Mesh mesh;

	// Light Data, stored inline
	bool has_light;
	Light light;
	
};

Object make_object(
	i32 unique_id,	
	bool visibility,
	HMM_Vec4 location, 
	HMM_Vec4 scale, 
	HMM_Quat rotation
)
{
	// From shader header
	ObjectData_t object_data = {};
	HMM_Mat4 scale_matrix = HMM_Scale(HMM_V3(scale.X, scale.Y, scale.Z));
	HMM_Mat4 rotation_matrix = HMM_QToM4(rotation);
	HMM_Mat4 translation_matrix = HMM_Translate(HMM_V3(location.X, location.Y, location.Z));
	object_data.model_matrix = HMM_MulM4(translation_matrix, HMM_MulM4(rotation_matrix, scale_matrix));	

	sg_buffer storage_buffer = sg_make_buffer((sg_buffer_desc){
		.type = SG_BUFFERTYPE_STORAGEBUFFER,
		.data = SG_RANGE(object_data),
		.label = "object-storage-buffer"
	});

	return (Object) {
		.unique_id = unique_id,
		.visibility = visibility,
		.location = location,
		.storage_buffer = storage_buffer,
		.has_mesh = false,
		.mesh = {},
	};
}

void cleanup_object(Object& in_object)
{
	sg_destroy_buffer(in_object.storage_buffer);
	if (in_object.has_mesh)
	{
		sg_destroy_buffer(in_object.mesh.index_buffer);
		sg_destroy_buffer(in_object.mesh.vertex_buffer);
	}
}

struct Camera {
	HMM_Vec3 position;
	HMM_Vec3 forward;
	HMM_Vec3 up;
};

struct {
    sg_pipeline pipeline;

	atomic<bool> game_running = true;
	atomic<bool> blender_data_loaded = false;

	map<i32, Object> objects;
	atomic<i32> updating_object_uid;

	optional<sg_buffer> lights_buffer;
	atomic<bool> updating_lights_buffer = false;
	
	std::thread live_link_thread;

	int blender_socket;
	int connection_socket;

	Camera camera = {
		.position = HMM_V3(0.0f, -9.0f, 0.0f),
		.forward = HMM_V3(0.0f, 1.0f, 0.0f),
		.up = HMM_V3(0.0f, 0.0f, 1.0f),
	};
} state;

#define SOCKET_OP(f) \
{\
	int result = (f);\
	if (result != 0)\
	{\
		printf("Line %i error on %s: %i\n", __LINE__, #f, errno);\
		exit(0);\
	}\
}

#if defined(__WIN32__)
#define PLATFORM_WINDOWS 
#endif

#if !defined(PLATFORM_WINDOWS)
#define SOCKET int
#endif

void socket_set_recv_timeout(SOCKET in_socket, const struct timeval& in_timeval)
{
	#if defined(PLATFORM_WINDOWS)
		DWORD timeout = in_timeval.tv_sec * 1000 + in_timeval.tv_usec / 1000;
		setsockopt(in_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout);
	#else
		// MAC & LINUX
		setsockopt(in_socket, SOL_SOCKET, SO_RCVTIMEO, (const void*)&in_timeval, sizeof(in_timeval));
	#endif
}

void socket_set_reuse_addr_and_port(SOCKET in_socket, bool in_enable)
{
	int optval = in_enable ? 1 : 0;
	SOCKET_OP(setsockopt(in_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)));
	SOCKET_OP(setsockopt(in_socket, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)));
}

// Live Link Function. Runs on its own thread
void live_link_thread_function()
{
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
	state.blender_socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

	// Allow us to reuse address and port
	socket_set_reuse_addr_and_port(state.blender_socket, true);

	// bind our socket
	SOCKET_OP(bind(state.blender_socket, res->ai_addr, res->ai_addrlen));

	const i32 backlog = 1;
	SOCKET_OP(listen(state.blender_socket, backlog));

	// accept connection from blender
	struct sockaddr_storage their_addr;
	socklen_t addr_size = sizeof their_addr;
	state.connection_socket = accept(state.blender_socket, (struct sockaddr *) &their_addr, &addr_size);


	// set recv timeout
	struct timeval recv_timeout = {
		.tv_sec = 1,
		.tv_usec = 0
	};
	socket_set_recv_timeout(state.connection_socket, recv_timeout);

	// infinite recv loop
	while (state.game_running)
	{
		sbuffer(u8) flatbuffer_data = nullptr;

		int current_bytes_read = 0;
		int total_bytes_read = 0;
		int packets_read = 0;
		optional<flatbuffers::uoffset_t> flatbuffer_size;
		do 
		{
			const size_t buffer_len = 4096; 
			u8 buffer[buffer_len];
			const int flags = 0;
			current_bytes_read = recv(state.connection_socket, buffer, buffer_len, flags);

			// Less than zero is an error
			if (current_bytes_read < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK)
				{
					current_bytes_read = 0;
					continue;
				}
				else
				{
					printf("recv_error: %s\n", strerror(errno));
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
				i32 next_idx = arrlen(flatbuffer_data);
				arraddn(flatbuffer_data, current_bytes_read);
				memcpy(&flatbuffer_data[next_idx], buffer, current_bytes_read);
				++packets_read;	
			}
		}
		while (state.game_running && (current_bytes_read == 0 || (flatbuffer_size && total_bytes_read < flatbuffer_size.value())));

		if (arrlen(flatbuffer_data) > 0)
		{
			printf("We've got some data! Data Length: %td Packets Read: %i\n", arrlen(flatbuffer_data), packets_read);

			// Interpret Flatbuffer data
			auto* update = Blender::LiveLink::GetSizePrefixedUpdate(flatbuffer_data);
			assert(update);
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

					HMM_Vec4 location = flatbuffer_helpers::to_hmm_vec4(object_location, 1.0f);
					HMM_Vec4 scale = flatbuffer_helpers::to_hmm_vec4(object_scale, 0.0f);
					HMM_Quat rotation = flatbuffer_helpers::to_hmm_quat(object_rotation);

					// set atomic updating object uid
					// used to ensure we aren't reading from this object's data on the main thread 
					state.updating_object_uid = unique_id;

					if (state.objects.contains(unique_id))
					{	
						Object& existing_object = state.objects[unique_id];
						cleanup_object(existing_object);
					}

					Object game_object = make_object(
						unique_id, 
						visibility, 
						location, 
						scale, 
						rotation
					);

					if (auto object_mesh = object->mesh())
					{
		 				sbuffer(Vertex) vertices = nullptr;
						if (auto flatbuffer_vertices = object_mesh->vertices())
						{
							for (i32 vertex_idx = 0; vertex_idx < flatbuffer_vertices->size(); ++vertex_idx)
							{
								auto vertex = flatbuffer_vertices->Get(vertex_idx);
								auto vertex_position = vertex->position();
								auto vertex_normal = vertex->normal();

								Vertex new_vertex = {
									.position = {
										.X = vertex_position.x(),
										.Y = vertex_position.y(),
										.Z = vertex_position.z(),
										.W = vertex_position.w(),
									},
									.normal = {
										.X = vertex_normal.x(),
										.Y = vertex_normal.y(),
										.Z = vertex_normal.z(),
										.W = vertex_normal.w(),
									},
								};

								arrput(vertices, new_vertex);
							}
						}

		 				sbuffer(u32) indices = nullptr;
						if (auto flatbuffer_indices = object_mesh->indices())
						{
							for (i32 indices_idx = 0; indices_idx < flatbuffer_indices->size(); ++indices_idx)
							{
								u32 new_index = flatbuffer_indices->Get(indices_idx);
								arrput(indices, new_index);
							}
						}

						// Set Mesh Data on Game Object
						if (arrlen(vertices) > 0 && arrlen(indices) > 0)
						{
							game_object.has_mesh = true;
							game_object.mesh = make_mesh(vertices, arrlen(vertices), indices, arrlen(indices));
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
									.energy = point_light->energy(),
								};
		 						break;
		 					}
							case LightType::Sun:
							{
								//FCS TODO:
								break;
							}
							case LightType::Spot:
							{
								//FCS TODO:
								break;
							}
							case LightType::Area:
							{
								//FCS TODO:
								break;
							}
							default:
								printf("Unsupported Light Type\n");
								exit(0);
						}

						if (game_object.visibility)
						{
							//Add to our buffer that contains all light data that we'll pass to the GPU when rendering meshes
						}
					}

					state.objects[unique_id] = game_object;
					// clear atomic updating object uid
					state.updating_object_uid = -1;
				}

				state.blender_data_loaded = true;
			}

			if (auto deleted_object_uids = update->deleted_object_uids())
			{
				for (i32 deleted_object_uid : *deleted_object_uids)
				{
					state.updating_object_uid = deleted_object_uid;
					if (state.objects.contains(deleted_object_uid))
					{
						printf("Removing object. UID: %i\n", deleted_object_uid);
						cleanup_object(state.objects[deleted_object_uid]);
						state.objects.erase(deleted_object_uid);
					}
					state.updating_object_uid = -1;
				}
			}

			// Update Lights List
			{
				//FCS TODO: replace this atomic bool. keep old buffer around until we're done with it
				state.updating_lights_buffer = true;
				if (state.lights_buffer)
				{
					sg_destroy_buffer(*state.lights_buffer);
					state.lights_buffer.reset();
				}

				LightsData_t lights_data = {}; 

				i32 current_point_light_index = 0;
				// FCS TODO: other light types

				for (auto const& [unique_id, object] : state.objects)
				{
					if (object.has_light)
					{
						const Light& light = object.light;
						switch(light.type)
						{
							case LightType::Point:
							{
								if (current_point_light_index >= MAX_POINT_LIGHTS)
								{
									printf("Max Number of Point Lights Added\n");
									continue;
								}

								auto& current_point_light = lights_data.point_lights[current_point_light_index];
								memcpy(current_point_light.location, &object.location, sizeof(float) * 4);
								memcpy(current_point_light.color, &object.light.color, sizeof(float) * 3);
								current_point_light.color[3] = 1.0;
								current_point_light.power = object.light.point.energy;

								printf(
									"Point Light color: {%f,%f,%f}\n", 
									current_point_light.color[0],
									current_point_light.color[1], 
									current_point_light.color[2]
								);

								++current_point_light_index;
								break;
							}
							default: break;
						}
					}
				}

				lights_data.num_point_lights = current_point_light_index;
				printf("Num Point Lights: %i\n", lights_data.num_point_lights);
	
				state.lights_buffer = sg_make_buffer((sg_buffer_desc){
					.type = SG_BUFFERTYPE_STORAGEBUFFER,
					.data = {
						.ptr = &lights_data,
						.size = sizeof(LightsData_t), 
					},
					.label = "lights-data"
				});
				state.updating_lights_buffer = false;
			}
		}
	}

	printf("Shutting down sockets\n");

	shutdown(state.connection_socket,2);
	close(state.connection_socket);

	shutdown(state.blender_socket,2);
	close(state.blender_socket);
}

void init(void)
{
    sg_setup((sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });

	// Spin up a thread that blocks until we receive our init event, and then listens for updates
	state.live_link_thread = std::thread(live_link_thread_function);
	
    /* create shader */
    sg_shader shd = sg_make_shader(cube_shader_desc(sg_query_backend()));

    /* create pipeline object */
    state.pipeline = sg_make_pipeline((sg_pipeline_desc){
        .layout = {
            /* test to provide buffer stride, but no attr offsets */
            .buffers[0].stride = 32,
            .attrs = {
                [ATTR_cube_position].format = SG_VERTEXFORMAT_FLOAT4,
                [ATTR_cube_normal].format   = SG_VERTEXFORMAT_FLOAT4
            }
        },
        .shader = shd,
        .index_type = SG_INDEXTYPE_UINT32,
        .cull_mode = SG_CULLMODE_NONE,
        .depth = {
            .write_enabled = true,
            .compare = SG_COMPAREFUNC_LESS_EQUAL,
        },
        .label = "cube-pipeline"
    });

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
}

bool keycodes[SAPP_MAX_KEYCODES];
bool is_key_pressed(sapp_keycode keycode)
{
	assert((i32)keycode < SAPP_MAX_KEYCODES);
	return keycodes[keycode];
}

void frame(void)
{
	double delta_time = sapp_frame_duration();
	
	// Camera Control
	{
		Camera& camera = state.camera;	
		const HMM_Vec3 camera_right = HMM_NormV3(HMM_Cross(camera.forward, camera.up));

		float move_speed = 10.0f * delta_time;
		if (is_key_pressed(SAPP_KEYCODE_LEFT_SHIFT))
		{
			move_speed *= 5.0f;
		}

		if (is_key_pressed(SAPP_KEYCODE_W))
		{
			camera.position += camera.forward * move_speed;	
		}
		if (is_key_pressed(SAPP_KEYCODE_S))
		{
			camera.position -= camera.forward * move_speed;	
		}	
		if (is_key_pressed(SAPP_KEYCODE_D))
		{
			camera.position += camera_right * move_speed;	
		}
		if (is_key_pressed(SAPP_KEYCODE_A))
		{
			camera.position -= camera_right * move_speed;	
		}
		if (is_key_pressed(SAPP_KEYCODE_E))
		{
			camera.position += camera.up * move_speed;	
		}
		if (is_key_pressed(SAPP_KEYCODE_Q))
		{
			camera.position -= camera.up * move_speed;	
		}
	}

	// Rendering
	{
		// Begin Render Pass
		sg_begin_pass((sg_pass)
			{
			.action = {
				.colors[0] = {
					.load_action = SG_LOADACTION_CLEAR,
					.clear_value = { 0.25f, 0.25f, 0.25f, 1.0f }
				},
			},
			.swapchain = sglue_swapchain()
		});

		if (!state.blender_data_loaded)
		{	
			sdtx_canvas(sapp_width()*0.5f, sapp_height()*0.5f);
			sdtx_origin(1.0f, 2.0f);
			sdtx_home();
			draw_debug_text(FONT_C64, "Waiting on data from Blender\n", 255,255,255);
			sdtx_draw();
		}
		else
		{
			/* NOTE: struct vs_params_t has been generated by the shader-code-gen */
			vs_params_t vs_params;
			const float w = sapp_widthf();
			const float h = sapp_heightf();
			const float t = (float)(sapp_frame_duration() * 60.0);
			HMM_Mat4 proj = HMM_Perspective_RH_NO(HMM_AngleDeg(60.0f), w/h, 0.01f, 10000.0f);

			Camera& camera = state.camera;
			HMM_Vec3 target = camera.position + camera.forward * 10;
			HMM_Mat4 view = HMM_LookAt_RH(camera.position, target, camera.up);
			HMM_Mat4 view_proj = HMM_MulM4(proj, view);
			vs_params.vp = view_proj;

			sg_apply_pipeline(state.pipeline);
			sg_apply_uniforms(0, SG_RANGE(vs_params));

			if (!state.updating_lights_buffer && state.lights_buffer)
			{
				for (auto const& [unique_id, object] : state.objects)
				{
					if (unique_id == state.updating_object_uid)
					{
						continue;	
					}

					if (!object.visibility)
					{
						continue;
					}

					if (object.has_mesh)
					{
						const Mesh& mesh = object.mesh;
						sg_bindings bindings = {
							.vertex_buffers[0] = mesh.vertex_buffer,
							.index_buffer = mesh.index_buffer,
							.storage_buffers = {
								[0] = object.storage_buffer,
								[1] = *state.lights_buffer,
							},
						};
						sg_apply_bindings(&bindings);
						sg_draw(0, mesh.idx_count, 1);
					}

					if (object.has_light)
					{
						//FCS TODO: Draw Light Icon
					}
				}
			}
		}

		sg_end_pass();
		sg_commit();
	}
}

void cleanup(void)
{
	// Tell live_link_thread we're done running and wait for it to complete
	state.game_running = false;
	state.live_link_thread.join();

	// shutdown game
    sg_shutdown();
}

void event(const sapp_event* event)
{
	switch(event->type)
	{
		case SAPP_EVENTTYPE_KEY_DOWN:
		{
			// stop execution on escape key 
			if (event->key_code == SAPP_KEYCODE_ESCAPE)
			{
				sapp_quit();
			}

			keycodes[event->key_code] = true;
			break;
		}
		case SAPP_EVENTTYPE_KEY_UP:
		{
			keycodes[event->key_code] = false;
			break;
		}
		default: break;
	}
}

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    return (sapp_desc) {
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
		.event_cb = event,
        .width = 800,
        .height = 600,
        .sample_count = 4,
        .window_title = "Blender Game",
        .icon.sokol_default = true,
        .logger.func = slog_func,
    };
}
