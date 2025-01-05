
// Primitive Typedefs
#include <cstdint>
typedef int64_t 	i64;
typedef int32_t 	i32;
typedef int16_t		i16;
typedef int8_t		i8;
typedef uint64_t 	u64;
typedef uint32_t 	u32;
typedef uint16_t 	u16;
typedef uint8_t 	u8;
// C std lib I/O

#include <cstdio>

// C++ std lib Optional
#include <optional>
using std::optional;

// C++ std lib threading
#include <thread>
#include <atomic>
using std::atomic;

// Ankerl's Segmented Vector and Fast Unordered Hash Math
#include "ankerl/unordered_dense.h"
using ankerl::unordered_dense::map;

// Handmade Math
#define HANDMADE_MATH_IMPLEMENTATION
//#define HANDMADE_MATH_NO_SSE
#include "handmade_math/HandmadeMath.h"

// STB Dynamic Array
#define STB_DS_IMPLEMENTATION
#include "stb/stb_ds.h"

// Basic Template-wrapper around stb_ds array functionality
template<typename T>
struct StretchyBuffer
{	
public:
	StretchyBuffer() { _data = nullptr; }
	~StretchyBuffer() { reset(); }
	void add(const T& value) { arrput(_data, value); }
	void add_unitialized(const size_t num) { arraddn(_data, num); }
	void reset() { arrfree(_data); _data = nullptr; }
	size_t length() { return arrlen(_data); }
	T* data() { return _data; }
	T& operator[](i32 idx) { return _data[idx]; }
protected:
	T* _data = nullptr;
};

// Sokol Headers
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

// Flatbuffers generated file
#include "blender_live_link_generated.h"

// Generated Shader File
#include "basic_draw.compiled.h"

// Wrapper for sockets 
#include "network/socket_wrapper.h"

// Thread-Safe Channel
#include "network/channel.h"

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

// Helper primarily used to pass between live link and main threads. 
// We want to wait until our data is back on the main thread to create GPU resources.
template<typename T> 
struct GpuBufferDesc
{
	/* Data to initialize the GpuBuffer with */
	T* data;
	
	/* Size of data */
	u64 data_size;

	/* max size of this buffer. Only meaningful if is_dynamic is true */
	u64 max_size;
	
	/* type of the GpuBuffer */
	sg_buffer_type type;

	/* can the GpuBuffer be updated after creation? */
	bool is_dynamic = false;

	/* Debug Label */
	const char* label;
};

template<typename T>
struct GpuBuffer
{
public:
	GpuBuffer() = default;

	GpuBuffer(GpuBufferDesc<T> in_desc)
	: data(in_desc.data)
	, data_size(in_desc.data_size)
	, max_size(in_desc.max_size)
	, gpu_buffer_type(in_desc.type)
	, is_dynamic(in_desc.is_dynamic)
	{
		//Static Buffers max_size and data_size should be identical
		if (!is_dynamic && max_size != data_size)
		{
			max_size = data_size;
		}

		set_label(in_desc.label);
	}

	bool is_gpu_buffer_valid()
	{
		return gpu_buffer.has_value();
	}

	sg_buffer get_gpu_buffer()
	{
		if (!gpu_buffer.has_value())
		{
			sg_usage usage = is_dynamic ? SG_USAGE_DYNAMIC : SG_USAGE_IMMUTABLE;

			sg_buffer_desc buffer_desc = {
				.type = gpu_buffer_type,
				.usage = usage,
				.data = {	
					/* 
					 * Static Buffers Set up their data in sg_make_buffer
					 * Dynamic Buffers only need to init to their max size
					 */
					.ptr = is_dynamic ? nullptr : data,
					.size = is_dynamic ? max_size : data_size,
				},
				.label = label ? label->c_str() : "",
			};

			gpu_buffer = sg_make_buffer(buffer_desc);

			// Dynamic Buffer Update can happen now
			if (is_dynamic && data != nullptr && data_size > 0)
			{
				// If Dynamic Buffer has data, send via sg_update_buffer now
				const sg_range update_range = {
					.ptr = data,
					.size = data_size,
				};

				sg_update_buffer(
					*gpu_buffer, 
					update_range				
				);
			}
	
			if (!keep_data)
			{
				free(data);	
			}
		}

		return *gpu_buffer;
	}

	void update_gpu_buffer(const sg_range& in_range)
	{
		assert(is_dynamic);
		sg_update_buffer(get_gpu_buffer(), &in_range);
	}

	void destroy_gpu_buffer()
	{	
		if (gpu_buffer.has_value())
		{
			sg_destroy_buffer(*gpu_buffer);
			gpu_buffer.reset();
		}
	}

	void set_label(const char* in_label)
	{
		label = std::string(in_label);
	}

	void set_keep_data(bool in_keep_data)
	{
		keep_data = in_keep_data;
	}

protected:
	// Underlying Buffer Data
	T* data;

	// Data Size
	u64 data_size;

	// Max Possible Size
	u64 max_size;

	// Gpu Buffer Type
	sg_buffer_type gpu_buffer_type;

	// is this Gpu Buffer dynamic
	bool is_dynamic = false;

	// Buffer used for gpu operations
	optional<sg_buffer> gpu_buffer;

	// If true, data is kept alive after initialing gpu_buffer
	bool keep_data = false;

	// Optional Label
	optional<std::string> label;
};

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

struct Object 
{
	i32 unique_id;
	bool visibility;

	// Object's world location
	HMM_Vec4 location;
	HMM_Quat rotation;

	GpuBuffer<ObjectData_t> storage_buffer; 

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
	/* 	
		We allocate here because GpuBuffer will take ownership of this memory and 
		free it after it creates the actual gpu resource on the main thread 
	*/
	ObjectData_t* object_data = (ObjectData_t*) malloc(sizeof(ObjectData_t)); 
	HMM_Mat4 scale_matrix = HMM_Scale(HMM_V3(scale.X, scale.Y, scale.Z));
	HMM_Mat4 rotation_matrix = HMM_QToM4(rotation);
	HMM_Mat4 translation_matrix = HMM_Translate(HMM_V3(location.X, location.Y, location.Z));
	object_data->model_matrix = HMM_MulM4(translation_matrix, HMM_MulM4(rotation_matrix, scale_matrix));	

	return (Object) {
		.unique_id = unique_id,
		.visibility = visibility,
		.location = location,
		.rotation = rotation,
		.storage_buffer = GpuBuffer((GpuBufferDesc<ObjectData_t>){
			.data = object_data,
			.data_size = sizeof(*object_data),
			.max_size = sizeof(*object_data),
			.type = SG_BUFFERTYPE_STORAGEBUFFER,
			.is_dynamic = false,
			.label = "Object::storage_buffer",
		}),
		.has_mesh = false,
		.mesh = {},
	};
}

void cleanup_object(Object& in_object)
{
	in_object.storage_buffer.destroy_gpu_buffer();
	if (in_object.has_mesh)
	{
		in_object.mesh.index_buffer.destroy_gpu_buffer();
		in_object.mesh.vertex_buffer.destroy_gpu_buffer();
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

	// These channels are how we pass our data from our live-link thread to the main thread
	Channel<Object> updated_objects;
	Channel<i32> deleted_objects;

	// Fragment shader params
	fs_params_t fs_params;

	// Contains Lights Data packed up for gpu usage
	bool needs_light_data_update = true;	

	StretchyBuffer<PointLight_t> point_lights;
	GpuBuffer<PointLight_t> point_lights_buffer;

	StretchyBuffer<SpotLight_t> spot_lights;
	GpuBuffer<SpotLight_t> spot_lights_buffer;
	
	std::thread live_link_thread;

	SOCKET blender_socket;
	SOCKET connection_socket;

	Camera camera = {
		.position = HMM_V3(0.0f, -9.0f, 0.0f),
		.forward = HMM_V3(0.0f, 1.0f, 0.0f),
		.up = HMM_V3(0.0f, 0.0f, 1.0f),
	};
} state;

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
				flatbuffer_data.add_unitialized(current_bytes_read);
				memcpy(&flatbuffer_data[next_idx], buffer, current_bytes_read);
				++packets_read;	
			}
		}
		while (state.game_running && (current_bytes_read == 0 || (flatbuffer_size && total_bytes_read < flatbuffer_size.value())));

		if (flatbuffer_data.length() > 0)
		{
			printf("We've got some data! Data Length: %td Packets Read: %i\n", flatbuffer_data.length(), packets_read);

			// Interpret Flatbuffer data
			auto* update = Blender::LiveLink::GetSizePrefixedUpdate(flatbuffer_data.data());
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

					Object game_object = make_object(
						unique_id, 
						visibility, 
						location, 
						scale, 
						rotation
					);

					if (auto object_mesh = object->mesh())
					{
						u32 num_vertices = 0;
		 				Vertex* vertices = nullptr;
						if (auto flatbuffer_vertices = object_mesh->vertices())
						{
							num_vertices = flatbuffer_vertices->size();
							vertices = (Vertex*) malloc(sizeof(Vertex) * num_vertices);

							for (i32 vertex_idx = 0; vertex_idx < num_vertices; ++vertex_idx)
							{
								auto vertex = flatbuffer_vertices->Get(vertex_idx);
								auto vertex_position = vertex->position();
								auto vertex_normal = vertex->normal();

								vertices[vertex_idx] = {
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

						// Set Mesh Data on Game Object
						if (num_vertices > 0 && num_indices > 0)
						{
							game_object.has_mesh = true;
							game_object.mesh = make_mesh(vertices, num_vertices, indices, num_indices);
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
		}
	}

	printf("Shutting down sockets\n");

	socket_close(state.connection_socket);
	socket_close(state.blender_socket);

	socket_lib_quit();
}

void init(void)
{
    sg_setup((sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });

	// Check for Storage Buffer Support
	if (!sg_query_features().storage_buffer)
	{
		printf("Sokol Gfx Error: Storage Buffers are Required\n");
		exit(0);
	}

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

		// Receive Updated Objects
		while (optional<Object> received_updated_object = state.updated_objects.receive())
		{
			Object& updated_object = *received_updated_object;
			i32 updated_object_uid = updated_object.unique_id;

			printf("Updating Object. UID: %i\n", updated_object_uid);

			if (state.objects.contains(updated_object_uid))
			{	
				Object& existing_object = state.objects[updated_object_uid];
				cleanup_object(existing_object);
			}

			if (updated_object.has_light)
			{
				state.needs_light_data_update = true;
			}

			state.objects[updated_object_uid] = updated_object;
		}

		// Receive Deleted Objects
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

				cleanup_object(object_to_delete);
				state.objects.erase(deleted_object_uid);
			}
		}

		// Run initially, and then only if our updates encounter a light
		if (state.needs_light_data_update)
		{
			state.needs_light_data_update = false;

			const i32 MAX_LIGHTS_PER_TYPE = 1024;

			// Init point_lights_buffer if we haven't already
			if (!state.point_lights_buffer.is_gpu_buffer_valid())
			{
				state.point_lights_buffer = GpuBuffer((GpuBufferDesc<PointLight_t>){
					.data = nullptr,
					.data_size = 0,
					.max_size = sizeof(PointLight_t) * MAX_LIGHTS_PER_TYPE,
					.type = SG_BUFFERTYPE_STORAGEBUFFER,
					.is_dynamic = true,
					.label = "point_lights",
				});

				//FCS TODO: GpuBuffer shouldn't manage data memory
				state.point_lights_buffer.set_keep_data(true);
			}

			// Init spot_lights_buffer if we haven't already
			if (!state.spot_lights_buffer.is_gpu_buffer_valid())
			{
				state.spot_lights_buffer = GpuBuffer((GpuBufferDesc<SpotLight_t>){
					.data = nullptr,
					.data_size = 0,
					.max_size = sizeof(SpotLight_t) * MAX_LIGHTS_PER_TYPE,
					.type = SG_BUFFERTYPE_STORAGEBUFFER,
					.is_dynamic = true,
					.label = "spot_lights",
				});

				//FCS TODO: GpuBuffer shouldn't manage data memory
				state.spot_lights_buffer.set_keep_data(true);
			}

			state.point_lights.reset();
			state.spot_lights.reset();

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

							PointLight_t new_point_light = {};
							memcpy(new_point_light.location, &object.location, sizeof(float) * 4);
							memcpy(new_point_light.color, &object.light.color, sizeof(float) * 3);
							new_point_light.color[3] = 1.0;
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

							SpotLight_t new_spot_light = {};
							memcpy(new_spot_light.location, &object.location, sizeof(float) * 4);
							memcpy(new_spot_light.color, &object.light.color, sizeof(float) * 3);
							new_spot_light.color[3] = 1.0;

							HMM_Vec3 spot_light_dir = HMM_RotateV3Q(HMM_V3(0,0,-1), object.rotation);
							memcpy(new_spot_light.direction, &spot_light_dir, sizeof(float) * 3);

							new_spot_light.spot_angle_radians = object.light.spot.beam_angle / 2.0f;
							
							new_spot_light.power = object.light.spot.power;
							new_spot_light.edge_blend = object.light.spot.edge_blend;
							state.spot_lights.add(new_spot_light);
							break;
						}
						case LightType::Sun:
						{
							//FCS TODO:	
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

			state.fs_params.num_point_lights = state.point_lights.length();
			printf("Num Point Lights: %i\n", state.fs_params.num_point_lights);
			if (state.fs_params.num_point_lights > 0)
			{
				state.point_lights_buffer.update_gpu_buffer(
					(sg_range){
						.ptr = state.point_lights.data(),
						.size = sizeof(PointLight_t) * state.fs_params.num_point_lights,
					}
				);
			}

			state.fs_params.num_spot_lights = state.spot_lights.length();	
			printf("Num Spot Lights: %i\n", state.fs_params.num_spot_lights);
			if (state.fs_params.num_spot_lights > 0)
			{
				state.spot_lights_buffer.update_gpu_buffer(
					(sg_range){
						.ptr = state.spot_lights.data(),
						.size = sizeof(SpotLight_t) * state.fs_params.num_spot_lights,
					}
				);
			}
		}

		if (!state.blender_data_loaded)
		{	
			sdtx_canvas(sapp_width() * 0.5f, sapp_height() * 0.5f);
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

			// Apply Vertex Uniforms
			sg_apply_uniforms(0, SG_RANGE(vs_params));

			// Apply Fragment Uniforms
			sg_apply_uniforms(1, SG_RANGE(state.fs_params));

			for (auto& [unique_id, object] : state.objects)
			{
				if (!object.visibility)
				{
					continue;
				}

				if (object.has_mesh)
				{
					Mesh& mesh = object.mesh;
					sg_bindings bindings = {
						.vertex_buffers[0] = mesh.vertex_buffer.get_gpu_buffer(),
						.index_buffer = mesh.index_buffer.get_gpu_buffer(),
						.storage_buffers = {
							[0] = object.storage_buffer.get_gpu_buffer(),
							[1] = state.point_lights_buffer.get_gpu_buffer(),
							[2] = state.spot_lights_buffer.get_gpu_buffer(),
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
		.win32_console_attach = true,
    };
}
