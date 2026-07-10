#pragma once

#include "core/types.h"
#include "handmade_math/HandmadeMath.h"

// Identical 48-byte layout to game/'s Vertex so flatbuffer parse code copies untouched
struct Vertex
{
	HMM_Vec4 position;
	HMM_Vec4 normal;
	HMM_Vec2 texcoord;
	f32 _padding[2];
};
static_assert(sizeof(Vertex) == 48, "Vertex must stay 48 bytes (matches game/ + shader vertex input layout)");

// Per-vertex skinning data (second vertex buffer for skinned draws)
struct SkinnedVertex
{
	HMM_Vec4 joint_indices;
	HMM_Vec4 joint_weights;
};
static_assert(sizeof(SkinnedVertex) == 32, "SkinnedVertex must match the skinned vertex input layout");

// Vulkan uses [0, 1] clip-space depth
#define PERSPECTIVE_FUNCTION HMM_Perspective_RH_ZO

HMM_Mat4 mat4_perspective(f32 in_fov, f32 in_aspect_ratio)
{
	const f32 projection_near = 0.01f;
	const f32 projection_far = 10000.0f;
	return PERSPECTIVE_FUNCTION(in_fov, in_aspect_ratio, projection_near, projection_far);
}

namespace Render
{
	constexpr VkCompareOp DEPTH_COMPARE_OP = VK_COMPARE_OP_LESS_OR_EQUAL;
	constexpr f32 DEPTH_CLEAR_VALUE = 1.0f;

	// Internal scene target: HDR-capable for future lighting; the copy pass
	// samples linear values and the sRGB swapchain view encodes on write
	constexpr VkFormat SCENE_COLOR_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
	constexpr VkFormat SCENE_DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
}
