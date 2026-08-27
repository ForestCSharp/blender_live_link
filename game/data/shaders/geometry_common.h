#ifndef GEOMETRY_COMMON_H
#define GEOMETRY_COMMON_H

#include "shader_common.h"

struct GeometryVertexSample
{
	vec4 local_position;
	vec4 local_normal;
	vec4 skin_debug_color;
	int is_skinned;
};

struct GeometryWorldVertex
{
	vec4 position;
	vec4 normal;
};

struct GeometryMaterialSample
{
	vec4 color;
	vec4 roughness_metallic_emissive;
};

GeometryVertexSample geometry_static_vertex(vec4 position, vec4 normal)
{
	GeometryVertexSample result;
	result.local_position = position;
	result.local_normal = normal;
	result.skin_debug_color = vec4(0.0);
	result.is_skinned = 0;
	return result;
}

GeometryVertexSample geometry_skinned_vertex(
	vec4 position,
	vec4 normal,
	int skin_matrix_offset,
	vec4 joint_indices,
	vec4 joint_weights)
{
	mat4 skin_matrix = get_skin_matrix(
		skin_matrix_offset, joint_indices, joint_weights);

	GeometryVertexSample result;
	result.local_position = skin_matrix * position;
	result.local_normal = vec4(
		normalize((skin_matrix * vec4(normal.xyz, 0.0)).xyz), 0.0);
	result.skin_debug_color = get_skin_debug_color(joint_indices, joint_weights);
	result.is_skinned = 1;
	return result;
}

vec4 geometry_skinned_position(
	vec4 position,
	int skin_matrix_offset,
	vec4 joint_indices,
	vec4 joint_weights)
{
	return get_skin_matrix(skin_matrix_offset, joint_indices, joint_weights)
		* position;
}

GeometryWorldVertex geometry_world_vertex(
	mat4 model_matrix,
	mat4 rotation_matrix,
	vec4 local_position,
	vec4 local_normal)
{
	GeometryWorldVertex result;
	result.position = model_matrix * local_position;
	result.normal = rotation_matrix * local_normal;
	return result;
}

GeometryMaterialSample geometry_sample_material(
	int material_index,
	vec2 texcoord)
{
	GeometryMaterialSample result;
	if (material_index < 0)
	{
		result.color = vec4(0.6, 0.6, 0.6, 1.0);
		result.roughness_metallic_emissive = vec4(0.5, 0.0, 0.0, 0.0);
		return result;
	}

	Material material = material_data_array[material_index];
	result.color = material.base_color_image_index >= 0
		? texture(sampler2D(
			SCENE_TEXTURE(material.base_color_image_index), scene_sampler), texcoord)
		: material.base_color;
	result.roughness_metallic_emissive.g = material.metallic_image_index >= 0
		? texture(sampler2D(
			SCENE_TEXTURE(material.metallic_image_index), scene_sampler), texcoord).r
		: material.metallic;
	result.roughness_metallic_emissive.r = material.roughness_image_index >= 0
		? texture(sampler2D(
			SCENE_TEXTURE(material.roughness_image_index), scene_sampler), texcoord).r
		: material.roughness;

	if (material.emission_strength > 0.0)
	{
		result.roughness_metallic_emissive.b = material.emission_strength;
		result.color.rgb = material.emission_color_image_index >= 0
			? texture(sampler2D(
				SCENE_TEXTURE(material.emission_color_image_index), scene_sampler), texcoord).rgb
			: material.emission_color.rgb;
		result.color.a = 1.0;
	}
	else
	{
		result.roughness_metallic_emissive.b = 0.0;
	}
	result.roughness_metallic_emissive.a = 0.0;
	return result;
}

#endif // GEOMETRY_COMMON_H
