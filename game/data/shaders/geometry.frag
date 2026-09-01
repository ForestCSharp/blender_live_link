#version 450

#include "geometry_common.h"

layout(location = 0) in vec4 in_world_position;
layout(location = 1) in vec4 in_world_normal;
layout(location = 2) in vec2 in_texcoord;
layout(location = 3) flat in int in_material_index;
layout(location = 4) in vec4 in_skin_debug_color;
layout(location = 5) flat in int in_is_skinned_mesh;

layout(push_constant) uniform PushConstants
{
	int skinning_debug_view;
} pc;

// G-buffer attachment layout:
//  0: base color, or emission color when emission_strength > 0
//  1: world position (w = 1 marks valid geometry)
//  2: world normal   (vec4(0) = sky/no-geometry sentinel for lighting)
//  3: r = roughness, g = metallic, b = emission_strength
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_position;
layout(location = 2) out vec4 out_normal;
layout(location = 3) out vec4 out_roughness_metallic_emissive;

void main()
{
	if (pc.skinning_debug_view != 0 && in_is_skinned_mesh != 0)
	{
		out_color = in_skin_debug_color;
		out_position = in_world_position;
		out_normal = normalize(in_world_normal);
		out_roughness_metallic_emissive = vec4(1.0, 0.0, 1.0, 0.0);
		return;
	}
	GeometryMaterialSample material_sample = geometry_sample_material(
		in_material_index, in_texcoord);
	out_color = material_sample.color;
	out_roughness_metallic_emissive = material_sample.roughness_metallic_emissive;
	out_position = in_world_position;
	out_normal = normalize(in_world_normal);
}
