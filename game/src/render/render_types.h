#pragma once

#include "core/types.h"
#include "handmade_math/HandmadeMath.h"
#include "shaders/shader_common.h"

struct Vertex
{
	HMM_Vec4 position;
	HMM_Vec4 normal;
	HMM_Vec2 texcoord;
	f32 _padding[2];
};

struct SkinnedVertex
{
	HMM_Vec4 joint_indices;
	HMM_Vec4 joint_weights;
};

static constexpr i32 NUM_CUBE_FACES = 6;

#if defined(SOKOL_D3D11) || defined(SOKOL_WGPU) || defined(SOKOL_METAL)
    /* These backends use [0, 1] depth range */
    #define PERSPECTIVE_FUNCTION HMM_Perspective_RH_ZO
#elif defined(SOKOL_GLCORE33) || defined(SOKOL_GLES2) || defined(SOKOL_GLES3) || defined(SOKOL_VULKAN)
    /* These backends use [-1, 1] depth range */
    #define PERSPECTIVE_FUNCTION HMM_Perspective_RH_NO
#else
    /* Fallback to the generic version or NO if unknown */
    #define PERSPECTIVE_FUNCTION HMM_Perspective_RH_NO
#endif

HMM_Mat4 mat4_perspective(f32 in_fov, f32 in_aspect_ratio)
{	
	const f32 projection_near = 0.01f;
	const f32 projection_far = 10000.0f;

#if USE_INVERSE_DEPTH
	return PERSPECTIVE_FUNCTION(in_fov, in_aspect_ratio, projection_far, projection_near);
#else
	return PERSPECTIVE_FUNCTION(in_fov, in_aspect_ratio, projection_near, projection_far);
#endif
}

namespace Render
{
#if USE_INVERSE_DEPTH
	constexpr sg_compare_func DEPTH_COMPARE_FUNC = SG_COMPAREFUNC_GREATER_EQUAL;
	constexpr sg_color DEPTH_CLEAR_VALUE = {0.0, 0.0, 0.0, 0.0};
#else
	constexpr sg_compare_func DEPTH_COMPARE_FUNC = SG_COMPAREFUNC_LESS_EQUAL;
	constexpr sg_color DEPTH_CLEAR_VALUE = {1.0, 1.0, 1.0, 1.0};
#endif
}
