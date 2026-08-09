#version 450

layout(set = 0, binding = 0) uniform sampler2D exposure_fine;
layout(set = 0, binding = 1) uniform sampler2D exposure_coarse;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 debug_laplacian;

void main()
{
	vec3 laplacian = texture(exposure_fine, uv).rgb - texture(exposure_coarse, uv).rgb;
	debug_laplacian = vec4(clamp(vec3(0.5) + 0.5 * laplacian, 0.0, 1.0), 1.0);
}
