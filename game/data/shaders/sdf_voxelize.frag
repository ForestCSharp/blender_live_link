#version 450

// Writes each fragment's material into the voxel holding its world position.
// The viewport is 2x the level resolution, so a triangle seen along its
// dominant axis covers every voxel it passes through. Last writer wins.

#include "geometry_common.h"
#include "sdf_common.h"

layout(location = 0) in vec4 in_world_position;
layout(location = 1) in vec4 in_world_normal;
layout(location = 2) in vec2 in_texcoord;
layout(location = 3) flat in int in_material_index;

layout(push_constant) uniform PushConstants
{
	mat4 view_projection;
	vec4 level_origin_voxel;
	ivec4 level;
} pc;

layout(set = 1, binding = 0, rgba8) uniform writeonly image3D albedo_image;
layout(set = 1, binding = 1, rgba8) uniform writeonly image3D normal_emission_image;

layout(location = 0) out vec4 out_unused;	// dummy target; color writes are masked off

void main()
{
	out_unused = vec4(0.0);
	ivec3 voxel = ivec3(floor((in_world_position.xyz - pc.level_origin_voxel.xyz) / pc.level_origin_voxel.w));
	if (any(lessThan(voxel, ivec3(0))) || any(greaterThanEqual(voxel, ivec3(SDF_LEVEL_RES))))
	{
		return;
	}
	GeometryMaterialSample material_sample = geometry_sample_material(in_material_index, in_texcoord);
	ivec3 texel = ivec3(voxel.xy, voxel.z + pc.level.x * SDF_LEVEL_RES);
	vec3 n = normalize(in_world_normal.xyz);
	imageStore(albedo_image, texel, vec4(clamp(material_sample.color.rgb, 0.0, 1.0), 1.0));
	imageStore(normal_emission_image, texel,
		vec4(n * 0.5 + 0.5, sdf_encode_emission(material_sample.roughness_metallic_emissive.b)));
}
