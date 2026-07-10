#ifndef SHADER_COMMON_H
#define SHADER_COMMON_H

// Shared between C++ and GLSL (compiled by glslc with -I data/shaders).
// The C++ branch supplies GLSL-style typedefs; the GLSL branch declares the
// descriptor set 0 resource blocks.
//
// LAYOUT RULES: no vec3 members, ever (std140/430 vs C++ packing); scalars
// are allowed as long as each struct's total size is a 16-byte multiple that
// matches on both sides. C++ static_asserts live in src/render/frame_data.h.

#if defined(__cplusplus)
	typedef HMM_Vec2 vec2;
	typedef HMM_Vec4 vec4;
	typedef HMM_Mat4 mat4;
#endif

#define MAX_MATERIALS 1024

// Capped by MoltenVK's non-update-after-bind limits
// (maxPerStageDescriptorSampledImages = 256, maxPerStageResources = 287).
// Raising this beyond ~250 requires UPDATE_AFTER_BIND on the texture array
// binding + pool, which unlocks the much larger UpdateAfterBind limits.
#define MAX_BINDLESS_IMAGES 128

// Per-frame data: camera + primary sun. std140 (all members 16-byte aligned).
// sun_direction.xyz is the direction the light TRAVELS (game/ convention:
// (0,0,-1) rotated by the sun object's rotation); shaders negate it to get
// the surface-to-light vector.
struct PerFrameData
{
	mat4 view;
	mat4 projection;
	mat4 view_projection;
	vec4 camera_position;	// w unused
	vec4 camera_forward;	// w unused
	vec4 sun_direction;		// xyz = light travel direction; w unused
	vec4 sun_color;			// rgb = light.color * sun.power; w unused
};

// Per-object data. std430, 144-byte stride — matches game/'s
// geometry_ObjectData_t exactly (mat4 + mat4 + int + 12 bytes pad).
struct ObjectData
{
	mat4 model_matrix;
	mat4 rotation_matrix;
	int material_index;		// index into material_data_array, -1 = none
	int _pad0;
	int _pad1;
	int _pad2;
};

// Material. std430, 64-byte stride — field order matches game/'s
// geometry_Material_t exactly (don't "fix" the image-index ordering).
struct Material
{
	vec4 base_color;
	vec4 emission_color;
	float metallic;
	float roughness;
	float emission_strength;
	int base_color_image_index;
	int emission_color_image_index;
	int metallic_image_index;
	int roughness_image_index;
	int _pad0;
};

#if !defined(__cplusplus)
layout(set = 0, binding = 0, std140) uniform PerFrameBlock
{
	PerFrameData per_frame;
};

// Named object_data_array (game/ parity) — "object_data" is a reserved
// address-space keyword in Metal, which breaks MoltenVK's SPIRV->MSL pass
layout(set = 0, binding = 1, std430) readonly buffer ObjectDataBlock
{
	ObjectData object_data_array[];
};

layout(set = 0, binding = 2, std430) readonly buffer MaterialDataBlock
{
	Material material_data_array[];
};

// Per-frame skin matrix arena; each skinned draw indexes at its own offset
// (push constant)
layout(set = 0, binding = 3, std430) readonly buffer SkinMatrixBlock
{
	mat4 skin_matrix_array[];
};

// Bindless scene textures: Material image indices index this array directly.
// PARTIALLY_BOUND descriptor binding — only sample when image_index >= 0.
layout(set = 0, binding = 4) uniform texture2D scene_textures[MAX_BINDLESS_IMAGES];
layout(set = 0, binding = 5) uniform sampler scene_sampler;

// Weighted 4-bone skin matrix (port of game/'s get_skin_matrix,
// shader_common.h:88-101); identity when total weight is ~zero
mat4 get_skin_matrix(int in_base_offset, vec4 in_joint_indices, vec4 in_joint_weights)
{
	float total_weight = in_joint_weights.x + in_joint_weights.y + in_joint_weights.z + in_joint_weights.w;
	if (total_weight <= 0.00001)
	{
		return mat4(1.0);
	}

	return in_joint_weights.x * skin_matrix_array[in_base_offset + int(in_joint_indices.x)]
		 + in_joint_weights.y * skin_matrix_array[in_base_offset + int(in_joint_indices.y)]
		 + in_joint_weights.z * skin_matrix_array[in_base_offset + int(in_joint_indices.z)]
		 + in_joint_weights.w * skin_matrix_array[in_base_offset + int(in_joint_indices.w)];
}
#endif

#endif // SHADER_COMMON_H
