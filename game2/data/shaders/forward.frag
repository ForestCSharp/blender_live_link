#version 450

layout(location = 0) in vec3 in_world_normal;

layout(location = 0) out vec4 out_color;

// Hardcoded sun for the scaffold. First upgrade slot: feed the scene's
// SunLight direction/color through a per-frame UBO.
const vec3 SUN_DIRECTION = normalize(vec3(0.3, 0.5, 0.8));
const vec3 BASE_COLOR = vec3(0.6, 0.6, 0.6);

void main()
{
	vec3 normal = normalize(in_world_normal);
	float n_dot_l = max(dot(normal, SUN_DIRECTION), 0.0);
	vec3 color = BASE_COLOR * (0.15 + 0.85 * n_dot_l);
	out_color = vec4(color, 1.0);
}
