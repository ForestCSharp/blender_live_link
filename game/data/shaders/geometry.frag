#version 450

#include "shader_common.h"

layout(location = 0) in vec4 in_world_position;
layout(location = 1) in vec4 in_world_normal;
layout(location = 2) in vec2 in_texcoord;
layout(location = 3) flat in int in_material_index;
layout(location = 4) in vec4 in_skin_debug_color;
layout(location = 5) flat in int in_is_skinned_mesh;

layout(push_constant) uniform PushConstants
{
	int object_index;
	int skin_matrix_offset;
	int skinning_debug_view;
	int _pad0;
} pc;

// G-buffer (game/ layout):
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
	if (in_material_index >= 0)
	{
		Material material = material_data_array[in_material_index];

		// Base Color
		if (material.base_color_image_index >= 0)
		{
			out_color = texture(sampler2D(SCENE_TEXTURE(material.base_color_image_index), scene_sampler), in_texcoord);
		}
		else
		{
			out_color = material.base_color;
		}

		// Metallic
		if (material.metallic_image_index >= 0)
		{
			out_roughness_metallic_emissive.g = texture(sampler2D(SCENE_TEXTURE(material.metallic_image_index), scene_sampler), in_texcoord).r;
		}
		else
		{
			out_roughness_metallic_emissive.g = material.metallic;
		}

		// Roughness
		if (material.roughness_image_index >= 0)
		{
			out_roughness_metallic_emissive.r = texture(sampler2D(SCENE_TEXTURE(material.roughness_image_index), scene_sampler), in_texcoord).r;
		}
		else
		{
			out_roughness_metallic_emissive.r = material.roughness;
		}

		// Emission Color and Strength
		if (material.emission_strength > 0.0)
		{
			out_roughness_metallic_emissive.b = material.emission_strength;
			if (material.emission_color_image_index >= 0)
			{
				out_color.rgb = texture(sampler2D(SCENE_TEXTURE(material.emission_color_image_index), scene_sampler), in_texcoord).rgb;
				out_color.a = 1.0;
			}
			else
			{
				out_color.rgb = material.emission_color.rgb;
				out_color.a = 1.0;
			}
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
		// Deviation from game/ (magenta + invalid-geometry sentinel):
		// material-less objects render as LIT grey so pre-material scenes
		// stay meaningful
		out_color = vec4(0.6, 0.6, 0.6, 1.0);
		out_roughness_metallic_emissive = vec4(0.5, 0.0, 0.0, 0.0);
		out_position = in_world_position;
		out_normal = normalize(in_world_normal);
	}
}
