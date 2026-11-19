#version 450

// Projects a probe's capture cubes into its 16^2 slot of the moments atlas and
// the three geometry-cache atlases (one MRT draw; all share the slot grid).
// Moments are blurred as before; the cache is read nearest-neighbour so hit
// distances and normals never blend across silhouettes.

#include "octahedral_helpers.h"
#include "sampling.h"

layout(push_constant) uniform fs_params
{
	int cubemap_render_size;
	int atlas_entry_size;
	int compute_irradiance;
	int use_importance_sampling;
	int output_moments;
};

layout(set = 0, binding = 2) uniform samplerCube cubemap_depth_tex;
layout(set = 0, binding = 3) uniform samplerCube cache_surface_cube;
layout(set = 0, binding = 4) uniform samplerCube cache_normal_cube;
layout(set = 0, binding = 5) uniform samplerCube cache_hit_cube;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 out_moments;
layout(location = 1) out vec4 out_cache_surface;
layout(location = 2) out vec4 out_cache_normal;
layout(location = 3) out vec4 out_cache_hit;

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
		vec2 u = sampling_hammersley(i, sample_count);
		vec3 sample_dir = importance_sample_radial_depth(u, dir, CUBE_MOMENT_BLUR_POWER_COSINE_EXPONENT);
		sum_moments += texture(cubemap_depth_tex, sample_dir);
	}
	return sum_moments * (1.0 / float(sample_count));
}

void main()
{
	vec3 dir = octahedral_decode(make_padded_atlas_uv(uv, atlas_entry_size));
	out_moments = compute_cube_moments(dir);
	out_cache_surface = texture(cache_surface_cube, dir);
	out_cache_normal = texture(cache_normal_cube, dir);
	out_cache_hit = texture(cache_hit_cube, dir);
}
