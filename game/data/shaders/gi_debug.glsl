// Common Shader Code
#include "shader_common.h"
#include "octahedral_helpers.h"
#include "gi_helpers.h"

@vs vs

@include_block gi_helpers

layout(binding=0) uniform vs_params {
    mat4 view;
	mat4 projection;
};

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 normal;
layout(location = 2) in vec2 texcoord;

out vec4 world_position;
out vec4 world_normal;
out vec2 pixel_texcoord;
out flat int probe_index;

mat4 mat4_translate(vec3 offset) {
    return mat4(
        1.0, 0.0, 0.0, 0.0, // Column 1
        0.0, 1.0, 0.0, 0.0, // Column 2
        0.0, 0.0, 1.0, 0.0, // Column 3
        offset.x, offset.y, offset.z, 1.0  // Column 4
    );
}

void main() {

	vec3 probe_position = gi_probe_position_from_index(gl_InstanceIndex);
	mat4 model = mat4_translate(probe_position);

	world_position = model * position;
	world_normal = normal;
	pixel_texcoord = texcoord;

	probe_index = gl_InstanceIndex;

    gl_Position = projection * view * world_position;
}
@end

@fs fs

@include_block material
@include_block octahedral_helpers
@include_block gi_helpers
@include_block remap
@include_block probe_radiance

layout(binding=1) uniform fs_params {
	int atlas_total_size;
	int atlas_entry_size;
	int probe_vis_mode;
};

layout(binding=0) uniform sampler linear_sampler;
layout(binding=1) uniform sampler nearest_sampler;

layout(binding=0) uniform texture2D octahedral_atlas_texture;
layout(binding=1) uniform texture2D octahedral_depth_texture;

layout(binding=2) readonly buffer ProbesBuffer {
	GI_Probe gi_probes[];
};

layout(binding=3) readonly buffer SH9CoefficientsBuffer {
	ProbeRadianceCoefficient sh9_coefficients[];
};

layout(binding=4) readonly buffer SG9LobesBuffer {
	ProbeSGLobe sg9_lobes[];
};

in vec4 world_position;
in vec4 world_normal;
in vec2 pixel_texcoord;
in flat int probe_index;

out vec4 out_color;
out vec4 out_position;
out vec4 out_normal;
out vec4 out_roughness_metallic_emissive;

const float depth_vis_dist = GI_MAX_RADIAL_DEPTH;
const float depth_vis_dist_squared = depth_vis_dist * depth_vis_dist;

vec3 sample_sh9_irradiance(int in_probe_index, vec3 normal)
{
	vec3 irradiance = vec3(0.0);
	int coefficient_offset = in_probe_index * 9;
	for (int i = 0; i < 9; ++i)
	{
		irradiance += sh9_coefficients[coefficient_offset + i].value.rgb * sh9_basis(i, normal) * sh9_diffuse_convolution_factor(i);
	}
	return max(irradiance, vec3(0.0));
}

vec3 sample_sg9_irradiance(int in_probe_index, vec3 normal)
{
	vec3 reconstructed_radiance = vec3(0.0);
	float weight_sum = 0.0;
	int coefficient_offset = in_probe_index * 9;
	for (int i = 0; i < 9; ++i)
	{
		vec4 lobe = sg9_lobes[coefficient_offset + i].params;
		float response = sg_lobe_diffuse_response(normalize(lobe.xyz), lobe.w, normal);
		reconstructed_radiance += sg9_lobes[coefficient_offset + i].amplitude.rgb * response;
		weight_sum += response;
	}
	return max(reconstructed_radiance / max(weight_sum, 0.00001), vec3(0.0));
}

void main()
{
	GI_Probe probe = gi_probes[probe_index];

	const vec3 sample_dir = normalize(world_normal).xyz;
	const vec2 octahedral_coords = padded_atlas_uv_from_normal(sample_dir, probe.atlas_idx, atlas_total_size, atlas_entry_size);

	switch (probe_vis_mode)
	{
		case 0:
		{
			out_color = texture(sampler2D(octahedral_atlas_texture, linear_sampler), octahedral_coords);
			break;
		}
		case 1:
		{
			out_color = vec4(sample_sh9_irradiance(probe_index, sample_dir), 1.0);
			break;
		}
		case 2:
		{
			out_color = vec4(sample_sg9_irradiance(probe_index, sample_dir), 1.0);
			break;
		}
		case 3:
		{
			const float radial_depth = texture(sampler2D(octahedral_depth_texture, linear_sampler), octahedral_coords).x;
			const float adjusted_depth = remap_clamped(radial_depth, 0.0, depth_vis_dist, 0.0, 1.0);
			out_color = vec4(vec3(adjusted_depth), 1.0);
			break;
		}
		case 4:
		{
			const float radial_depth_squared = texture(sampler2D(octahedral_depth_texture, linear_sampler), octahedral_coords).y;
			const float adjusted_depth_squared = remap_clamped(radial_depth_squared, 0.0, depth_vis_dist_squared, 0.0, 1.0);
			out_color = vec4(vec3(adjusted_depth_squared), 1.0);
			break;
		}
		case 5:
		{
			const float evrp_positive_moment = texture(sampler2D(octahedral_depth_texture, linear_sampler), octahedral_coords).x;
			const float adjusted_evrp_moment = remap_clamped(evrp_positive_moment, 1.0, exp(5.0), 0.0, 1.0);
			out_color = vec4(vec3(adjusted_evrp_moment), 1.0);
			break;
		}
		default:
		{
			out_color = vec4(0,0,0,0);
		}
	}

	out_roughness_metallic_emissive = vec4(1,0,1,0);
	out_position = world_position; 
	out_normal = normalize(world_normal);
}
@end

@program gi_debug vs fs
