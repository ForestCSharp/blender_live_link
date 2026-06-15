// Common Shader Code
#include "shader_common.h"

@vs vs

layout(binding=0) uniform vs_params {
    mat4 view;
	mat4 projection;
};

@include_block object_data

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 normal;
layout(location = 2) in vec2 texcoord;

out vec4 world_position;
out vec4 world_normal;
out vec2 pixel_texcoord;
out vec4 skin_debug_color;
out flat int is_skinned_mesh;
out flat int material_index;

void main() {
	world_position = object_data_array[0].model_matrix * position;
	world_normal = object_data_array[0].rotation_matrix * normal;
	pixel_texcoord = texcoord;
	skin_debug_color = vec4(0.0);
	is_skinned_mesh = 0;
	material_index = object_data_array[0].material_index;

    gl_Position = projection * view * world_position;
}
@end

@vs skinned_vs

layout(binding=0) uniform vs_params {
    mat4 view;
	mat4 projection;
};

@include_block object_data
@include_block skinning_data

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 normal;
layout(location = 2) in vec2 texcoord;
layout(location = 3) in vec4 joint_indices;
layout(location = 4) in vec4 joint_weights;

out vec4 world_position;
out vec4 world_normal;
out vec2 pixel_texcoord;
out vec4 skin_debug_color;
out flat int is_skinned_mesh;
out flat int material_index;

void main() {
	mat4 skin_matrix = get_skin_matrix(joint_indices, joint_weights);
	vec4 skinned_position = skin_matrix * position;
	vec4 skinned_normal = vec4(normalize((skin_matrix * vec4(normal.xyz, 0.0)).xyz), 0.0);

	world_position = object_data_array[0].model_matrix * skinned_position;
	world_normal = object_data_array[0].rotation_matrix * skinned_normal;
	pixel_texcoord = texcoord;
	skin_debug_color = get_skin_debug_color(joint_indices, joint_weights);
	is_skinned_mesh = 1;
	material_index = object_data_array[0].material_index;

    gl_Position = projection * view * world_position;
}
@end

@fs fs
@include_block material

layout(binding=1) uniform fs_params {
	int skinning_debug_view;
};

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
in vec4 skin_debug_color;
in flat int is_skinned_mesh;
in flat int material_index;

out vec4 out_color;
out vec4 out_position;
out vec4 out_normal;
out vec4 out_roughness_metallic_emissive;

void main()
{
	if (skinning_debug_view != 0 && is_skinned_mesh != 0)
	{
		out_color = skin_debug_color;
		out_roughness_metallic_emissive = vec4(1.0, 0.0, 1.0, 0.0);
		out_position = world_position;
		out_normal = normalize(world_normal);
		return;
	}

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

		out_position = world_position; 
		out_normal = normalize(world_normal);
	}
	else
	{
		out_color = vec4(1,0,1,1);
		out_roughness_metallic_emissive = vec4(1,0,0,0);
		out_position = vec4(0,0,0,0);
		out_normal = vec4(0,0,0,0);
	}
}
@end

// Wireframe lives in the geometry shader module because the overlay uses the same
// vertex/object data and writes into the same G-buffer attachments as shaded geometry.
@vs wire_vs

layout(binding=0) uniform vs_params {
    mat4 view;
	mat4 projection;
};

@include_block object_data

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 normal;
layout(location = 2) in vec2 texcoord;

out vec4 wire_world_position;
out vec4 wire_world_normal;

void main() {
	wire_world_position = object_data_array[0].model_matrix * position;
	wire_world_normal = object_data_array[0].rotation_matrix * normal;
	gl_Position = projection * view * wire_world_position;
}
@end

@fs wire_fs

in vec4 wire_world_position;
in vec4 wire_world_normal;

out vec4 out_color;
out vec4 out_position;
out vec4 out_normal;
out vec4 out_roughness_metallic_emissive;

void main()
{
	out_color = vec4(0.01, 0.01, 0.01, 1.0);
	out_position = wire_world_position;
	out_normal = normalize(wire_world_normal);
	out_roughness_metallic_emissive = vec4(0.85, 0.0, 0.35, 0.0);
}
@end

@program geometry vs fs
@program skinned_geometry skinned_vs fs
@program wireframe wire_vs wire_fs
