#version 450

layout(set = 0, binding = 0) uniform sampler2D exposure_fine;
layout(set = 0, binding = 1) uniform sampler2D exposure_coarse;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 debug_laplacian;

void main()
{
	vec4 packed_fine = texture(exposure_fine, uv);
	vec4 packed_coarse = texture(exposure_coarse, uv);
	vec3 fine = packed_fine.a > 1.0e-5 ? packed_fine.rgb / packed_fine.a : vec3(0.0);
	vec3 coarse = packed_coarse.a > 1.0e-5 ? packed_coarse.rgb / packed_coarse.a : fine;
	vec3 laplacian = fine - coarse;
	debug_laplacian = vec4(
		clamp(vec3(0.5) + 0.5 * laplacian, 0.0, 1.0), packed_fine.a);
}
