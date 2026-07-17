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

// Vulkan uses [0, 1] clip-space depth. Reverse-Z (game/ USE_INVERSE_DEPTH
// parity): far/near swapped in the projection, GREATER compare, clear 0.
// Better depth precision and required by the ported pass-chain math
// (sky draws at z=0, shadow receiver depth = 1 - ndc.z, ...).
#define PERSPECTIVE_FUNCTION HMM_Perspective_RH_ZO

HMM_Mat4 mat4_perspective(f32 in_fov, f32 in_aspect_ratio)
{
	const f32 projection_near = 0.01f;
	const f32 projection_far = 10000.0f;
	return PERSPECTIVE_FUNCTION(in_fov, in_aspect_ratio, projection_far, projection_near);
}

namespace Render
{
	constexpr VkCompareOp DEPTH_COMPARE_OP = VK_COMPARE_OP_GREATER_OR_EQUAL;
	constexpr f32 DEPTH_CLEAR_VALUE = 0.0f;

	// Internal scene target: HDR-capable for future lighting; the copy pass
	// samples linear values and the sRGB swapchain view encodes on write
	inline VkFormat SCENE_COLOR_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
	inline VkFormat SCENE_DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

	// G-buffer attachments prefer game/ parity (4x RGBA32F); capability
	// selection falls back to RGBA16F when RGBA32F cannot render/filter.
	inline VkFormat GBUFFER_FORMAT = VK_FORMAT_R32G32B32A32_SFLOAT;
	constexpr i32 GBUFFER_OUTPUT_COUNT = 4;

	// EVSM4 moments (warped depths + second moments)
	inline VkFormat SHADOW_MOMENTS_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

	// Single-channel occlusion prefers R8 and falls back to R16F.
	inline VkFormat SSAO_FORMAT = VK_FORMAT_R8_UNORM;

	inline void configure_formats(const VulkanContext& in_context)
	{
		SCENE_COLOR_FORMAT = in_context.capabilities.scene_color_format;
		SCENE_DEPTH_FORMAT = in_context.capabilities.scene_depth_format;
		GBUFFER_FORMAT = in_context.capabilities.gbuffer_format;
		SHADOW_MOMENTS_FORMAT = in_context.capabilities.shadow_moments_format;
		SSAO_FORMAT = in_context.capabilities.ssao_format;
		printf("Render formats: scene=%i depth=%i gbuffer=%i shadow=%i ssao=%i\n",
			(i32)SCENE_COLOR_FORMAT, (i32)SCENE_DEPTH_FORMAT, (i32)GBUFFER_FORMAT,
			(i32)SHADOW_MOMENTS_FORMAT, (i32)SSAO_FORMAT);
	}
}
