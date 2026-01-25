// Common Shader Code
#include "shader_common.h"

@vs vs

layout(binding=0) uniform vs_params {
    mat4 view;
	mat4 projection;
};

struct ObjectData
{
	mat4 model_matrix;
	mat4 rotation_matrix;
	int material_index;
};

layout(binding=0) readonly buffer ObjectDataBuffer {
	ObjectData object_data_array[];
};

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 normal;
layout(location = 2) in vec2 texcoord;

out vec4 world_position;
out vec4 world_normal;
out vec2 pixel_texcoord;
out flat int material_index;

void main() {
	world_position = object_data_array[0].model_matrix * position;
	world_normal = object_data_array[0].rotation_matrix * normal;
	pixel_texcoord = texcoord;
	material_index = object_data_array[0].material_index;

    gl_Position = projection * view * world_position;
}
@end

@fs fs
@include_block material

layout(binding=0) uniform sampler smp;

layout(binding=1) readonly buffer MaterialDataBuffer {
	Material material_data_array[];
};

layout(binding=2) uniform texture2D base_color_texture;
layout(binding=3) uniform texture2D metallic_texture;
layout(binding=4) uniform texture2D roughness_texture;
layout(binding=5) uniform texture2D emission_texture;

in vec4 world_position;
in vec4 world_normal;
in vec2 pixel_texcoord;
in flat int material_index;

out vec4 out_color;
out vec4 out_position;
out vec4 out_normal;
out vec4 out_roughness_metallic_emissive;

void main()
{
	if (material_index >= 0)
	{	
		Material material = material_data_array[material_index];

		// Base Color
		if (material.base_color_image_index >= 0)
		{
			out_color = texture(sampler2D(base_color_texture, smp), pixel_texcoord);
		}
		else
		{
			out_color = material.base_color;
		}

		// Metallic
		if (material.metallic_image_index >= 0)
		{
			out_roughness_metallic_emissive.g = texture(sampler2D(metallic_texture, smp), pixel_texcoord).r;
		}
		else
		{
			out_roughness_metallic_emissive.g = material.metallic;
		}

		// Roughness
		if (material.roughness_image_index >= 0)
		{
			out_roughness_metallic_emissive.r = texture(sampler2D(roughness_texture, smp), pixel_texcoord).r;
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
				out_color.rgb = texture(sampler2D(emission_texture, smp), pixel_texcoord).rgb;
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
	}
	else
	{
		out_color = vec4(1,0,1,1);
		out_roughness_metallic_emissive = vec4(1,0,0,0);
	}
	out_position = world_position; 
	out_normal = normalize(world_normal);
}
@end

@program geometry vs fs
