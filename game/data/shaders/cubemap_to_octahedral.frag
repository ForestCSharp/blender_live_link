#version 450

// Projects a probe's lighting + radial-depth cubemaps into one padded
// octahedral atlas entry (port of game/data/shaders/cubemap_to_octahedral.glsl
// fs). MRT 0 = irradiance/lighting atlas, MRT 1 = depth-moment atlas.

#include "octahedral_helpers.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

layout(push_constant) uniform fs_params
{
	int cubemap_render_size;
	int atlas_entry_size;
	int compute_irradiance;
	int use_importance_sampling;
};

layout(set = 0, binding = 1) uniform samplerCube cubemap_lighting_tex;
layout(set = 0, binding = 2) uniform samplerCube cubemap_depth_tex;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec4 radial_depth;

float radical_inverse_vdc(uint bits)
{
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

vec2 hammersley(uint i, uint N)
{
	return vec2(float(i)/float(N), radical_inverse_vdc(i));
}

vec3 importance_sample_diffuse(vec2 u, vec3 N)
{
	float phi = 2.0 * M_PI * u.x;
	float cos_theta = sqrt(1.0 - u.y);
	float sin_theta = sqrt(u.y);

	// spherical to cartesian
	vec3 H;
	H.x = cos(phi) * sin_theta;
	H.y = sin(phi) * sin_theta;
	H.z = cos_theta;

	// tangent space to world
	vec3 up        = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent   = normalize(cross(up, N));
	vec3 bitangent = cross(N, tangent);

	vec3 sample_vec = tangent * H.x + bitangent * H.y + N * H.z;
	return normalize(sample_vec);
}

#define CUBE_MOMENT_BLUR_IMPORTANCE_SAMPLE_COUNT 64u
#define CUBE_MOMENT_BLUR_POWER_COSINE_EXPONENT 32.0

void build_orthonormal_basis(vec3 n, out vec3 tangent, out vec3 bitangent)
{
	// Branchless orthonormal basis (Frisvad-style) to avoid direction seam artifacts.
	float s = signNotZero(n.z);
	float a = -1.0 / (s + n.z);
	float b = n.x * n.y * a;
	tangent = normalize(vec3(1.0 + s * n.x * n.x * a, s * b, -s * n.x));
	bitangent = normalize(vec3(b, s + n.y * n.y * a, -n.y));
}

vec3 importance_sample_radial_depth(vec2 u, vec3 N, float power_exponent)
{
	// Power-cosine hemisphere sampling around N.
	float phi = 2.0 * M_PI * u.x;
	float cos_theta = pow(1.0 - u.y, 1.0 / (power_exponent + 1.0));
	float sin_theta = sqrt(max(0.0, 1.0 - (cos_theta * cos_theta)));

	vec3 tangent;
	vec3 bitangent;
	build_orthonormal_basis(N, tangent, bitangent);

	vec3 local = vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
	return normalize(tangent * local.x + bitangent * local.y + N * local.z);
}

vec4 compute_cube_moments(vec3 dir)
{
	vec4 sum_moments = vec4(0.0);
	const uint sample_count = CUBE_MOMENT_BLUR_IMPORTANCE_SAMPLE_COUNT;

	for (uint i = 0u; i < sample_count; ++i)
	{
		vec2 u = hammersley(i, sample_count);
		vec3 sample_dir = importance_sample_radial_depth(u, dir, CUBE_MOMENT_BLUR_POWER_COSINE_EXPONENT);
		sum_moments += texture(cubemap_depth_tex, sample_dir);
	}

	return sum_moments * (1.0 / float(sample_count));
}

void main()
{
	// convert 0 to 1 uv to -1 to 1 octahedral coords
	vec2 octahedral_coords = make_padded_atlas_uv(uv, atlas_entry_size);

	// Convert octahedral coords to 3d cubemap direction vector
	vec3 cubemap_dir = octahedral_decode(octahedral_coords);

	if (compute_irradiance != 0)
	{
		if (use_importance_sampling != 0)
		{
			vec3 normal = cubemap_dir;
			vec3 irradiance = vec3(0.0);

			const uint SAMPLE_COUNT = 1024u;
			for (uint i = 0u; i < SAMPLE_COUNT; ++i)
			{
				vec2 u = hammersley(i, SAMPLE_COUNT);
				vec3 sample_vec = importance_sample_diffuse(u, normal);

				irradiance += texture(cubemap_lighting_tex, sample_vec).rgb * max(dot(normal, sample_vec), 0.0);
			}
			irradiance = M_PI * irradiance * (1.0 / float(SAMPLE_COUNT));
			frag_color = vec4(irradiance, 1.0);
		}
		else // uniform sampling over hemisphere
		{
			vec3 normal = cubemap_dir;
			vec3 irradiance = vec3(0.0);

			vec3 up    = vec3(0.0, 1.0, 0.0);
			vec3 right = normalize(cross(up, normal));
			up         = normalize(cross(normal, right));

			float sample_delta = 0.025;
			float nr_samples = 0.0;
			for (float phi = 0.0; phi < 2.0 * M_PI; phi += sample_delta)
			{
				for (float theta = 0.0; theta < 0.5 * M_PI; theta += sample_delta)
				{
					// spherical to cartesian (in tangent space)
					vec3 tangent_sample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
					// tangent space to world
					vec3 sample_vec = tangent_sample.x * right + tangent_sample.y * up + tangent_sample.z * normal;

					irradiance += texture(cubemap_lighting_tex, sample_vec).rgb * cos(theta) * sin(theta);
					nr_samples++;
				}
			}
			irradiance = M_PI * irradiance * (1.0 / float(nr_samples));
			frag_color = vec4(irradiance, 1.0);
		}
	}
	else
	{
		// Sample our cubemap
		vec3 sampled_color = texture(cubemap_lighting_tex, cubemap_dir).xyz;

		// Write final color output
		frag_color = vec4(sampled_color, 1.0);
	}

	radial_depth = compute_cube_moments(cubemap_dir);
}
