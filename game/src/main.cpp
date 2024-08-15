
#include <cerrno>
#include <cstdio>

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
#include <sys/socket.h>
#include <netdb.h>

// Flatbuffers generated file
#include "blender_live_link_generated.h"

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


float cube_vertices[] = {
	-1.0, -1.0, -1.0,   1.0, 0.0, 0.0, 1.0,
	1.0, -1.0, -1.0,   1.0, 0.0, 0.0, 1.0,
	1.0,  1.0, -1.0,   1.0, 0.0, 0.0, 1.0,
	-1.0,  1.0, -1.0,   1.0, 0.0, 0.0, 1.0,

	-1.0, -1.0,  1.0,   0.0, 1.0, 0.0, 1.0,
	1.0, -1.0,  1.0,   0.0, 1.0, 0.0, 1.0,
	1.0,  1.0,  1.0,   0.0, 1.0, 0.0, 1.0,
	-1.0,  1.0,  1.0,   0.0, 1.0, 0.0, 1.0,

	-1.0, -1.0, -1.0,   0.0, 0.0, 1.0, 1.0,
	-1.0,  1.0, -1.0,   0.0, 0.0, 1.0, 1.0,
	-1.0,  1.0,  1.0,   0.0, 0.0, 1.0, 1.0,
	-1.0, -1.0,  1.0,   0.0, 0.0, 1.0, 1.0,

	1.0, -1.0, -1.0,    1.0, 0.5, 0.0, 1.0,
	1.0,  1.0, -1.0,    1.0, 0.5, 0.0, 1.0,
	1.0,  1.0,  1.0,    1.0, 0.5, 0.0, 1.0,
	1.0, -1.0,  1.0,    1.0, 0.5, 0.0, 1.0,

	-1.0, -1.0, -1.0,   0.0, 0.5, 1.0, 1.0,
	-1.0, -1.0,  1.0,   0.0, 0.5, 1.0, 1.0,
	1.0, -1.0,  1.0,   0.0, 0.5, 1.0, 1.0,
	1.0, -1.0, -1.0,   0.0, 0.5, 1.0, 1.0,

	-1.0,  1.0, -1.0,   1.0, 0.0, 0.5, 1.0,
	-1.0,  1.0,  1.0,   1.0, 0.0, 0.5, 1.0,
	1.0,  1.0,  1.0,   1.0, 0.0, 0.5, 1.0,
	1.0,  1.0, -1.0,   1.0, 0.0, 0.5, 1.0
};

uint16_t cube_indices[] = {
	0, 1, 2,  0, 2, 3,
	6, 5, 4,  7, 6, 4,
	8, 9, 10,  8, 10, 11,
	14, 13, 12,  15, 14, 12,
	16, 17, 18,  16, 18, 19,
	22, 21, 20,  23, 22, 20
};

struct Mesh {
	hmm_vec4 location;
	i32 idx_count;
	sg_buffer vbuf;
	sg_buffer ibuf;
	sg_buffer sbuf;
};

Mesh make_cube(hmm_vec4 location)
{
	sg_buffer vbuf = sg_make_buffer((sg_buffer_desc){
		.data = SG_RANGE(cube_vertices),
		.label = "mesh-vertices"
	});

	sg_buffer ibuf = sg_make_buffer((sg_buffer_desc){
		.type = SG_BUFFERTYPE_INDEXBUFFER,
		.data = SG_RANGE(cube_indices),
		.label = "mesh-indices"
	});

	sg_buffer sbuf = sg_make_buffer((sg_buffer_desc){
		.type = SG_BUFFERTYPE_STORAGEBUFFER,
		.data = SG_RANGE(location),
		.label = "mesh-storage-buffer"
	});

	return (Mesh) {
		.location = location,
		.idx_count = sizeof(cube_indices) / sizeof(cube_indices[0]),
		.vbuf = vbuf,
		.ibuf = ibuf,
		.sbuf = sbuf,
	};
}

struct {
    sg_pipeline pip;
	sbuffer(Mesh) meshes;
	int blender_socket;
} state;

// FCS TODO: Do this in thread so its non-blocking? callback when finished...
void init_blender_socket(void)
{
	// Init socket we'll use to talk to blender
	struct addrinfo hints, *res;
	// first, load up address structs with getaddrinfo():

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	//FCS TODO: Store magic IP and Port numbers in some shared file
	getaddrinfo("127.0.0.1", "65432", &hints, &res);

	// make a socket
	state.blender_socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	bind(state.blender_socket, res->ai_addr, res->ai_addrlen);
	const i32 backlog = 1;
	listen(state.blender_socket, backlog);

	// accept connection from blender
	struct sockaddr_storage their_addr;
	socklen_t addr_size = sizeof their_addr;
	int connection_socket = accept(state.blender_socket, (struct sockaddr *) &their_addr, &addr_size);

	sbuffer(u8) flatbuffer_data = nullptr;

	int bytes_read = 0;
	int packets_read = 0;
	do 
	{
		const size_t buffer_len = 4096; 
		u8 buffer[buffer_len];
		const int flags = 0;
		bytes_read = recv(connection_socket, buffer, buffer_len, flags);

		if (bytes_read < 0)
		{
			printf("recv_error: %s\n", strerror(errno));
			exit(0);
		}
		
		if (bytes_read > 0)
		{
			i32 next_idx = arrlen(flatbuffer_data);
			arraddn(flatbuffer_data, bytes_read);
			memcpy(&flatbuffer_data[next_idx], buffer, bytes_read);
			++packets_read;
		}
	}
	while (bytes_read > 0);

	if (arrlen(flatbuffer_data) > 0)
	{
		printf("We've got some data! Data Length: %td Packets Read: %i\n", arrlen(flatbuffer_data), packets_read);
	}

	shutdown(connection_socket,2);
	shutdown(state.blender_socket,2);

	// Interpret Flatbuffer data
	auto* scene = Blender::LiveLink::GetScene(flatbuffer_data);
	assert(scene);
	auto scene_name = scene->name(); 
	assert(scene_name);
	printf("Scene Name: %s\n", scene_name->c_str());
	if (auto objects = scene->objects())
	{
		for (i32 idx = 0; idx < objects->size(); ++idx)
	   	{
			auto object = objects->Get(idx);
			if (auto object_name = object->name())
			{
				printf("\tObject Name: %s\n", object_name->c_str());
			}
			if (auto object_location = object->location())
			{
				printf("\tObject Location: %f, %f, %f\n", object_location->x(), object_location->y(), object_location->z());
				arrput(state.meshes, make_cube(HMM_Vec4(object_location->x(), object_location->y(), object_location->z(), 1)));
			}
		}
	}
}

void init(void) {

    sg_setup((sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });

	state.meshes = nullptr;
	init_blender_socket();
	
	//arrput(state.meshes, make_cube(HMM_Vec4(0,0,0,1)));
	//arrput(state.meshes, make_cube(HMM_Vec4(2.5,1,0,1)));
	//arrput(state.meshes, make_cube(HMM_Vec4(-2.5,1,0,1)));

    /* create shader */
    sg_shader shd = sg_make_shader(cube_shader_desc(sg_query_backend()));

    /* create pipeline object */
    state.pip = sg_make_pipeline((sg_pipeline_desc){
        .layout = {
            /* test to provide buffer stride, but no attr offsets */
            .buffers[0].stride = 28,
            .attrs = {
                [ATTR_vs_position].format = SG_VERTEXFORMAT_FLOAT3,
                [ATTR_vs_color0].format   = SG_VERTEXFORMAT_FLOAT4
            }
        },
        .shader = shd,
        .index_type = SG_INDEXTYPE_UINT16,
        .cull_mode = SG_CULLMODE_BACK,
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

void frame(void) {

	// Begin Render Pass
    sg_begin_pass((sg_pass)
		{
        .action = {
            .colors[0] = {
                .load_action = SG_LOADACTION_CLEAR,
                .clear_value = { 0.25f, 0.5f, 0.75f, 1.0f }
            },
        },
        .swapchain = sglue_swapchain()
    });

	bool blender_data_loaded = true;
	if (!blender_data_loaded)
	{	
		sdtx_canvas(sapp_width()*0.5f, sapp_height()*0.5f);
		sdtx_origin(1.0f, 2.0f);
    	sdtx_home();
		draw_debug_text(FONT_C64, "Waiting on data from Blender\n", 255,255,255);
		sdtx_draw();
	}
	else
	{
		/* NOTE: the vs_params_t struct has been code-generated by the shader-code-gen */
		vs_params_t vs_params;
		const float w = sapp_widthf();
		const float h = sapp_heightf();
		const float t = (float)(sapp_frame_duration() * 60.0);
		hmm_mat4 proj = HMM_Perspective(60.0f, w/h, 0.01f, 10.0f);
		hmm_mat4 view = HMM_LookAt(HMM_Vec3(0.0f, -9.0, 0.0f), HMM_Vec3(0.0f, 0.0f, 0.0f), HMM_Vec3(0.0f, 0.0f, 1.0f));
		hmm_mat4 view_proj = HMM_MultiplyMat4(proj, view);
		vs_params.vp = view_proj;

		sg_apply_pipeline(state.pip);
		sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_vs_params, SG_RANGE(vs_params));

		for (i32 mesh_idx = 0; mesh_idx < arrlen(state.meshes); ++mesh_idx)
		{
			Mesh& mesh = state.meshes[mesh_idx];
			sg_bindings bindings = {
				.vertex_buffers[0] = mesh.vbuf,
				.index_buffer = mesh.ibuf,
				.vs = {
					.storage_buffers[SLOT_ObjectDataBuffer] = mesh.sbuf,
				},
			};
			sg_apply_bindings(&bindings);
			sg_draw(0, mesh.idx_count, 1);
		}
	}

	sg_end_pass();
	sg_commit();
}

void cleanup(void)
{
    sg_shutdown();
}

void event(const sapp_event* event)
{
	switch(event->type)
	{
		case SAPP_EVENTTYPE_KEY_DOWN:
		{
			if (event->key_code == SAPP_KEYCODE_ESCAPE)
			{
				exit(0);
			}

			break;
		}
		default: break;
	}
}

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    return (sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
		.event_cb = event,
        .width = 800,
        .height = 600,
        .sample_count = 4,
        .window_title = "Cube (sokol-app)",
        .icon.sokol_default = true,
        .logger.func = slog_func,
    };
}
