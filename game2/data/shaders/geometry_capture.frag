#version 450

#include "shader_common.h"

layout(location = 0) in vec4 in_world_position;
layout(location = 1) in vec4 in_world_normal;
layout(location = 2) in vec2 in_texcoord;
layout(location = 3) flat in int in_material_index;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_position;
layout(location = 2) out vec4 out_normal;
layout(location = 3) out vec4 out_roughness_metallic_emissive;

void main()
{
	if (in_material_index >= 0)
	{
		Material material = material_data_array[in_material_index];
		out_color = material.base_color_image_index >= 0
			? texture(sampler2D(scene_textures[material.base_color_image_index], scene_sampler), in_texcoord)
			: material.base_color;
		out_roughness_metallic_emissive.g = material.metallic_image_index >= 0
			? texture(sampler2D(scene_textures[material.metallic_image_index], scene_sampler), in_texcoord).r
			: material.metallic;
		out_roughness_metallic_emissive.r = material.roughness_image_index >= 0
			? texture(sampler2D(scene_textures[material.roughness_image_index], scene_sampler), in_texcoord).r
			: material.roughness;
		if (material.emission_strength > 0.0)
		{
			out_roughness_metallic_emissive.b = material.emission_strength;
			out_color.rgb = material.emission_color_image_index >= 0
				? texture(sampler2D(scene_textures[material.emission_color_image_index], scene_sampler), in_texcoord).rgb
				: material.emission_color.rgb;
			out_color.a = 1.0;
		}
		else
		{
			out_roughness_metallic_emissive.b = 0.0;
		}
		out_roughness_metallic_emissive.a = 0.0;
		out_position = in_world_position;
		out_normal = normalize(in_world_normal);
	}
	else
	{
		out_color = vec4(0.6, 0.6, 0.6, 1.0);
		out_roughness_metallic_emissive = vec4(0.5, 0.0, 0.0, 0.0);
		out_position = in_world_position;
		out_normal = normalize(in_world_normal);
	}
}
