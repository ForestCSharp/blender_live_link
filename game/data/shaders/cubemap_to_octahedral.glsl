
// Common Shader Code
#include "shader_common.h"

// octahedral helpers
#include "octahedral_helpers.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.h"

@fs fs

@include_block octahedral_helpers

layout(binding=0) uniform fs_params
{
	int atlas_entry_size;
	int compute_irradiance;
	int use_importance_sampling;
};

layout(binding=0) uniform textureCube cubemap_lighting_texture;
layout(binding=1) uniform textureCube cubemap_depth_texture;

layout(binding=0) uniform sampler smp;

in vec2 uv;

out vec4 frag_color;
out float radial_depth;

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
	float cosTheta = sqrt(1.0 - u.y);
	float sinTheta = sqrt(u.y);

	// spherical to cartesian
	vec3 H;
	H.x = cos(phi) * sinTheta;
	H.y = sin(phi) * sinTheta;
	H.z = cosTheta;

	// tangent space to world
	vec3 up        = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent   = normalize(cross(up, N));
	vec3 bitangent = cross(N, tangent);

	vec3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
	return normalize(sampleVec);
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
			for(uint i = 0u; i < SAMPLE_COUNT; ++i)
			{
				vec2 u = hammersley(i, SAMPLE_COUNT);
				vec3 sampleVec = importance_sample_diffuse(u, normal); 

				irradiance += texture(samplerCube(cubemap_lighting_texture, smp), sampleVec).rgb * max(dot(normal, sampleVec), 0.0);
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

			float sampleDelta = 0.025;
			float nrSamples = 0.0; 
			for(float phi = 0.0; phi < 2.0 * M_PI; phi += sampleDelta)
			{
				for(float theta = 0.0; theta < 0.5 * M_PI; theta += sampleDelta)
				{
					// spherical to cartesian (in tangent space)
					vec3 tangentSample = vec3(sin(theta) * cos(phi),  sin(theta) * sin(phi), cos(theta));
					// tangent space to world
					vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * normal; 

					irradiance += texture(samplerCube(cubemap_lighting_texture, smp), sampleVec).rgb * cos(theta) * sin(theta);
					nrSamples++;
				}
			}
			irradiance = M_PI * irradiance * (1.0 / float(nrSamples));
			frag_color = vec4(irradiance, 1.0);
		}
	}
	else
	{
		// Sample our cubemap
		vec3 sampled_color = texture(samplerCube(cubemap_lighting_texture, smp), cubemap_dir).xyz;

		// Write final color output
		frag_color = vec4(sampled_color, 1.0);
	}

	radial_depth = texture(samplerCube(cubemap_depth_texture, smp), cubemap_dir).x;
}

@end

@program cubemap_to_octahedral vs fs
