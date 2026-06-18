// Common Shader Code
#include "shader_common.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.h"

@fs copy_fs

layout(binding=0) uniform texture2D source_color_texture;
layout(binding=0) uniform sampler source_sampler;

in vec2 uv;

out vec4 frag_color;

void main()
{
	frag_color = texture(sampler2D(source_color_texture, source_sampler), uv);
}
@end

@program copy vs copy_fs

@vs mesh_vs

struct WireOverlayVertex
{
	vec4 position;
	vec4 normal;
	vec2 texcoord;
	float padding0;
	float padding1;
};

struct WireOverlayIndex
{
	uint value;
};

layout(binding=0) uniform mesh_vs_params {
	mat4 view;
	mat4 projection;
	mat4 model;
};

layout(binding=0) readonly buffer WireOverlayVerticesBuffer {
	WireOverlayVertex vertices[];
};

layout(binding=1) readonly buffer WireOverlayIndicesBuffer {
	WireOverlayIndex indices[];
};

noperspective out vec3 wire_barycentric;
noperspective out vec2 wire_screen_uv;
out vec4 wire_world_position;

void main()
{
	uint corner = uint(gl_VertexIndex) % 3u;
	uint index = indices[uint(gl_VertexIndex)].value;
	WireOverlayVertex vertex = vertices[index];

	wire_barycentric = corner == 0u
		? vec3(1.0, 0.0, 0.0)
		: corner == 1u
			? vec3(0.0, 1.0, 0.0)
			: vec3(0.0, 0.0, 1.0);

	wire_world_position = model * vertex.position;
	gl_Position = projection * view * wire_world_position;

	float safe_w = abs(gl_Position.w) < 0.000001 ? 0.000001 : gl_Position.w;
	vec2 ndc = gl_Position.xy / safe_w;
	wire_screen_uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}
@end

@fs mesh_fs

layout(binding=1) uniform mesh_fs_params {
	vec4 color;
	vec4 camera_position;
	vec4 camera_forward;
	vec2 screen_size;
	float width;
	float softness;
	float opacity;
	float visibility_tolerance;
};

layout(binding=2) uniform texture2D geometry_position_texture;
layout(binding=0) uniform sampler geometry_sampler;

noperspective in vec3 wire_barycentric;
noperspective in vec2 wire_screen_uv;
in vec4 wire_world_position;

out vec4 frag_color;

void main()
{
	vec4 visible_position = texture(sampler2D(geometry_position_texture, geometry_sampler), wire_screen_uv);
	if (visible_position.w <= 0.0)
	{
		discard;
	}

	vec3 camera_to_wire = wire_world_position.xyz - camera_position.xyz;
	vec3 camera_to_visible = visible_position.xyz - camera_position.xyz;
	float wire_depth = dot(camera_to_wire, camera_forward.xyz);
	float visible_depth = dot(camera_to_visible, camera_forward.xyz);
	float depth_tolerance = max(visibility_tolerance, abs(wire_depth) * 0.001);
	if (abs(wire_depth - visible_depth) > depth_tolerance)
	{
		discard;
	}

	vec3 barycentric_distance = abs(wire_barycentric);
	vec3 barycentric_width = max(fwidth(barycentric_distance), vec3(0.000001));
	vec3 edge_distance_pixels = barycentric_distance / barycentric_width;
	float nearest_edge_pixels = min(edge_distance_pixels.x, min(edge_distance_pixels.y, edge_distance_pixels.z));
	float coverage = 1.0 - smoothstep(
		max(width - softness, 0.0),
		width + max(softness, 0.001),
		nearest_edge_pixels
	);
	frag_color = vec4(color.rgb, coverage * clamp(opacity, 0.0, 1.0));
}
@end

@program mesh_overlay mesh_vs mesh_fs
