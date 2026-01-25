#pragma once

#include "core/types.h"
#include "handmade_math/HandmadeMath.h"

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
