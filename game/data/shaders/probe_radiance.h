#ifndef PROBE_RADIANCE_H
#define PROBE_RADIANCE_H

// SH9/SG9 probe radiance basis helpers (port of game/data/shaders/
// shader_common.h @block probe_radiance). GLSL-only.

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct ProbeRadianceCoefficient
{
	vec4 value;
};

struct ProbeSGLobe
{
	vec4 params;
	vec4 amplitude;
};

const float SG9_SHARPNESS = 1.0;
const float SG9_MIN_SHARPNESS = 1.0;
const float SG9_MAX_SHARPNESS = 32.0;
const float SG9_DIFFUSE_SHARPNESS_SCALE = 0.5;

float sh9_basis(int index, vec3 dir)
{
	float x = dir.x;
	float y = dir.y;
	float z = dir.z;

	switch (index)
	{
		case 0: return 0.282095;
		case 1: return 0.488603 * y;
		case 2: return 0.488603 * z;
		case 3: return 0.488603 * x;
		case 4: return 1.092548 * x * y;
		case 5: return 1.092548 * y * z;
		case 6: return 0.315392 * (3.0 * z * z - 1.0);
		case 7: return 1.092548 * x * z;
		case 8: return 0.546274 * (x * x - y * y);
		default: return 0.0;
	}
}

float sh9_diffuse_convolution_factor(int index)
{
	if (index == 0) { return M_PI; }
	if (index <= 3) { return (2.0 * M_PI) / 3.0; }
	return M_PI / 4.0;
}

vec3 sg9_initial_axis(int index)
{
	switch (index)
	{
		case 0: return vec3(1.0, 0.0, 0.0);
		case 1: return vec3(-1.0, 0.0, 0.0);
		case 2: return vec3(0.0, 1.0, 0.0);
		case 3: return vec3(0.0, -1.0, 0.0);
		case 4: return vec3(0.0, 0.0, 1.0);
		case 5: return vec3(0.0, 0.0, -1.0);
		case 6: return normalize(vec3(1.0, 1.0, 1.0));
		case 7: return normalize(vec3(-1.0, 1.0, -1.0));
		case 8: return normalize(vec3(1.0, -1.0, -1.0));
		default: return vec3(0.0, 0.0, 1.0);
	}
}

float sg_lobe_response(vec3 axis, float sharpness, vec3 dir)
{
	return exp(sharpness * (dot(axis, dir) - 1.0));
}

float sg_lobe_diffuse_response(vec3 axis, float sharpness, vec3 dir)
{
	return sg_lobe_response(axis, sharpness * SG9_DIFFUSE_SHARPNESS_SCALE, dir);
}

#endif // PROBE_RADIANCE_H
