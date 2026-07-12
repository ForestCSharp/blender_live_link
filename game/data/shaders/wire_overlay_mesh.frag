#version 450

// Wireframe edge shading (port of game/data/shaders/wire_overlay.glsl
// mesh_fs): barycentric distance-to-edge in pixels, visibility test against
// the G-buffer world position (only wires on visible surfaces draw).

layout(set = 1, binding = 0) uniform mesh_fs_params
{
	vec4 color;
	vec4 camera_position;
	vec4 camera_forward;
	vec2 screen_size;
	float width;
	float softness;
	float opacity;
	float visibility_tolerance;
};

layout(set = 1, binding = 1) uniform sampler2D geometry_position_tex;

layout(location = 0) noperspective in vec3 wire_barycentric;
layout(location = 1) noperspective in vec2 wire_screen_uv;
layout(location = 2) in vec4 wire_world_position;

layout(location = 0) out vec4 frag_color;

void main()
{
	vec4 visible_position = texture(geometry_position_tex, wire_screen_uv);
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
