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

layout(binding=1) buffer SG9LobesBuffer {
	ProbeSGLobe sg9_lobes[];
};

layout(local_size_x=32, local_size_y=1, local_size_z=1) in;

const int PROBE_RADIANCE_MODE_SH9 = 1;
const int PROBE_RADIANCE_MODE_SG9 = 2;
const int SG9_NUM_LOBES = 9;
const int SG9_FIT_ITERATIONS = 4;

shared vec3 shared_sg9_axes[SG9_NUM_LOBES];
shared float shared_sg9_sharpness[SG9_NUM_LOBES];

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

float sg9_basis_sum(vec3 dir)
{
	float sum_basis = 0.0;
	for (int i = 0; i < SG9_NUM_LOBES; ++i)
	{
		sum_basis += sg_lobe_diffuse_response(shared_sg9_axes[i], shared_sg9_sharpness[i], dir);
	}
	return max(sum_basis, 0.00001);
}

void main()
{
	uint coefficient_index = gl_LocalInvocationID.x;
	bool has_lobe = coefficient_index < 9u;

	int coefficient_offset = probe_index * 9 + int(coefficient_index);
	uint num_samples = uint(max(sample_count, 1));

	if (radiance_mode == PROBE_RADIANCE_MODE_SH9)
	{
		if (!has_lobe)
		{
			return;
		}

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
		if (has_lobe)
		{
			shared_sg9_axes[coefficient_index] = sg9_initial_axis(int(coefficient_index));
			shared_sg9_sharpness[coefficient_index] = SG9_SHARPNESS;
		}
		barrier();

		for (int iteration = 0; iteration < SG9_FIT_ITERATIONS; ++iteration)
		{
			if (has_lobe)
			{
				vec3 current_axis = shared_sg9_axes[coefficient_index];
				float current_sharpness = shared_sg9_sharpness[coefficient_index];

				vec3 accumulated_direction = vec3(0.0);
				float accumulated_weight = 0.0;
				float accumulated_cosine = 0.0;

				for (uint i = 0u; i < num_samples; ++i)
				{
					vec3 dir = sample_sphere(hammersley(i, num_samples));
					float basis = sg_lobe_diffuse_response(current_axis, current_sharpness, dir);
					float responsibility = basis / sg9_basis_sum(dir);
					float weight = responsibility;

					accumulated_direction += dir * weight;
					accumulated_cosine += dot(current_axis, dir) * weight;
					accumulated_weight += weight;
				}

				if (accumulated_weight > 0.00001)
				{
					vec3 fitted_axis = normalize(accumulated_direction);
					float average_cosine = clamp(accumulated_cosine / accumulated_weight, 0.0, 0.999);
					float fitted_sharpness = clamp(1.0 / max(1.0 - average_cosine, 0.00001), SG9_MIN_SHARPNESS, SG9_MAX_SHARPNESS);

					shared_sg9_axes[coefficient_index] = fitted_axis;
					shared_sg9_sharpness[coefficient_index] = fitted_sharpness;
				}
			}

			barrier();
		}

		if (!has_lobe)
		{
			return;
		}

		vec3 final_axis = shared_sg9_axes[coefficient_index];
		float final_sharpness = shared_sg9_sharpness[coefficient_index];
		vec3 accumulated_radiance = vec3(0.0);
		float accumulated_weight = 0.0;
		for (uint i = 0u; i < num_samples; ++i)
		{
			vec3 dir = sample_sphere(hammersley(i, num_samples));
			vec3 radiance = texture(samplerCube(cubemap_lighting_texture, smp), dir).rgb;
			float basis = sg_lobe_diffuse_response(final_axis, final_sharpness, dir);
			float responsibility = basis / sg9_basis_sum(dir);

			accumulated_radiance += radiance * responsibility;
			accumulated_weight += responsibility;
		}

		sg9_lobes[coefficient_offset].params = vec4(final_axis, final_sharpness);
		sg9_lobes[coefficient_offset].amplitude = vec4(accumulated_radiance / max(accumulated_weight, 0.00001), 0.0);
	}
}

@end

@program probe_radiance_projection cs
