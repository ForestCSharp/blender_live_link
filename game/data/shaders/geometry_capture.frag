#version 450

#include "geometry_common.h"

layout(location = 0) in vec4 in_world_position;
layout(location = 1) in vec4 in_world_normal;
layout(location = 2) in vec2 in_texcoord;
layout(location = 3) flat in int in_material_index;

layout(push_constant) uniform PushConstants
{
	mat4 view_projection;
	vec4 capture_position_and_radius;
} pc;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_position;
layout(location = 2) out vec4 out_normal;
layout(location = 3) out vec4 out_roughness_metallic_emissive;

void main()
{
	if (pc.capture_position_and_radius.w > 0.0)
	{
		vec3 probe_to_fragment =
			in_world_position.xyz - pc.capture_position_and_radius.xyz;
		float radius_squared = pc.capture_position_and_radius.w
			* pc.capture_position_and_radius.w;
		if (dot(probe_to_fragment, probe_to_fragment) > radius_squared)
		{
			discard;
		}
	}

	GeometryMaterialSample material_sample = geometry_sample_material(
		in_material_index, in_texcoord);
	out_color = material_sample.color;
	out_roughness_metallic_emissive = material_sample.roughness_metallic_emissive;
	out_position = in_world_position;
	out_normal = normalize(in_world_normal);
}
