// Common Shader Code
#include "shader_common.h"

@cs cs

@include_block probe_radiance

layout(binding=0) uniform cs_params {
	int probe_index;
	int radiance_mode;
	int sample_count;
	int _padding0;
};

layout(binding=2) uniform textureCube cubemap_lighting_texture;
layout(binding=0) uniform sampler smp;

layout(binding=0) buffer SH9CoefficientsBuffer {
	ProbeRadianceCoefficient sh9_coefficients[];
};

layout(binding=1) buffer SG9CoefficientsBuffer {
	ProbeRadianceCoefficient sg9_coefficients[];
};

layout(local_size_x=32, local_size_y=1, local_size_z=1) in;

const int PROBE_RADIANCE_MODE_SH9 = 1;
const int PROBE_RADIANCE_MODE_SG9 = 2;

float radical_inverse_vdc(uint bits)
{
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint i, uint N)
{
	return vec2(float(i) / float(N), radical_inverse_vdc(i));
}

vec3 sample_sphere(vec2 u)
{
	float z = 1.0 - 2.0 * u.x;
	float r = sqrt(max(0.0, 1.0 - z * z));
	float phi = 2.0 * M_PI * u.y;
	return vec3(r * cos(phi), r * sin(phi), z);
}

void main()
{
	uint coefficient_index = gl_GlobalInvocationID.x;
	if (coefficient_index >= 9u)
	{
		return;
	}

	int coefficient_offset = probe_index * 9 + int(coefficient_index);
	uint num_samples = uint(max(sample_count, 1));

	if (radiance_mode == PROBE_RADIANCE_MODE_SH9)
	{
		vec3 accumulated = vec3(0.0);
		for (uint i = 0u; i < num_samples; ++i)
		{
			vec3 dir = sample_sphere(hammersley(i, num_samples));
			vec3 radiance = texture(samplerCube(cubemap_lighting_texture, smp), dir).rgb;
			accumulated += radiance * sh9_basis(int(coefficient_index), dir);
		}

		float integral_weight = (4.0 * M_PI) / float(num_samples);
		sh9_coefficients[coefficient_offset].value = vec4(accumulated * integral_weight, 0.0);
	}
	else if (radiance_mode == PROBE_RADIANCE_MODE_SG9)
	{
		vec3 accumulated = vec3(0.0);
		float accumulated_weight = 0.0;
		for (uint i = 0u; i < num_samples; ++i)
		{
			vec3 dir = sample_sphere(hammersley(i, num_samples));
			vec3 radiance = texture(samplerCube(cubemap_lighting_texture, smp), dir).rgb;
			float weight = sg9_basis(int(coefficient_index), dir);
			accumulated += radiance * weight;
			accumulated_weight += weight;
		}

		sg9_coefficients[coefficient_offset].value = vec4(accumulated / max(accumulated_weight, 0.00001), 0.0);
	}
}

@end

@program probe_radiance_projection cs
