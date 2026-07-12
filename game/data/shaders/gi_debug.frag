#version 450

#include "gi_helpers.h"
#include "octahedral_helpers.h"
#include "probe_radiance.h"

layout(push_constant) uniform DebugParams
{
	mat4 view_projection;
	int debug_probe_start_index;
	float probe_debug_radius;
	int atlas_total_size;
	int atlas_entry_size;
	int probe_vis_mode;
	int isolated_probe_index;
	int padding0;
	int padding1;
};

layout(set = 0, binding = 1) uniform sampler2D lighting_atlas;
layout(set = 0, binding = 2) uniform sampler2D depth_atlas;
layout(set = 0, binding = 3, std430) readonly buffer ProbeFragmentBlock { GI_Probe probes[]; };
layout(set = 0, binding = 4, std430) readonly buffer SH9Block { ProbeRadianceCoefficient sh9_coefficients[]; };
layout(set = 0, binding = 5, std430) readonly buffer SG9Block { ProbeSGLobe sg9_lobes[]; };

layout(location = 0) in vec4 world_position;
layout(location = 1) in vec4 world_normal;
layout(location = 2) flat in int probe_index;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_position;
layout(location = 2) out vec4 out_normal;
layout(location = 3) out vec4 out_rme;

vec3 sample_sh9(int index, vec3 normal)
{
	vec3 result = vec3(0.0);
	for (int i = 0; i < 9; ++i)
		result += sh9_coefficients[index * 9 + i].value.rgb * sh9_basis(i, normal) * sh9_diffuse_convolution_factor(i);
	return max(result, vec3(0.0));
}

vec3 sample_sg9(int index, vec3 normal)
{
	vec3 result = vec3(0.0); float weight = 0.0;
	for (int i = 0; i < 9; ++i)
	{
		ProbeSGLobe lobe = sg9_lobes[index * 9 + i];
		float response = sg_lobe_diffuse_response(normalize(lobe.params.xyz), lobe.params.w, normal);
		result += lobe.amplitude.rgb * response; weight += response;
	}
	return max(result / max(weight, 0.00001), vec3(0.0));
}

void main()
{
	GI_Probe probe = probes[probe_index];
	vec3 normal = normalize(world_normal.xyz);
	vec4 color = vec4(0.05, 0.05, 0.05, 1.0);
	if (isolated_probe_index >= 0 && probe_index != isolated_probe_index)
		color = vec4(1.0, 0.0, 0.0, 1.0);
	else if (probe.atlas_idx >= 0)
	{
		vec2 atlas_uv = padded_atlas_uv_from_normal(normal, probe.atlas_idx, atlas_total_size, atlas_entry_size);
		if (probe_vis_mode == 0) color = texture(lighting_atlas, atlas_uv);
		else if (probe_vis_mode == 1) color = vec4(sample_sh9(probe_index, normal), 1.0);
		else if (probe_vis_mode == 2) color = vec4(sample_sg9(probe_index, normal), 1.0);
		else if (probe_vis_mode == 3) color = vec4(vec3(clamp(texture(depth_atlas, atlas_uv).x / max(probe.max_radial_depth, 0.00001), 0.0, 1.0)), 1.0);
		else if (probe_vis_mode == 4) color = vec4(vec3(clamp(texture(depth_atlas, atlas_uv).y / max(probe.max_radial_depth * probe.max_radial_depth, 0.00001), 0.0, 1.0)), 1.0);
		else color = vec4(vec3(clamp((texture(depth_atlas, atlas_uv).x - 1.0) / (exp(5.0) - 1.0), 0.0, 1.0)), 1.0);
	}
	out_color = color;
	out_position = world_position;
	out_normal = vec4(normal, 0.0);
	out_rme = vec4(1.0, 0.0, 1.0, 0.0);
}
