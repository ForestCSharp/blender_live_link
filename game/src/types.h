#pragma once

#include <cstdint>
#include "handmade_math/HandmadeMath.h"

// Primitive Typedefs
typedef int64_t 	i64;
typedef int32_t 	i32;
typedef int16_t		i16;
typedef int8_t		i8;
typedef uint64_t 	u64;
typedef uint32_t 	u32;
typedef uint16_t 	u16;
typedef uint8_t 	u8;


struct Transform
{
	HMM_Vec4 location;
	HMM_Quat rotation;
	HMM_Vec3 scale;
};

namespace Constants
{
	constexpr double Pi = 3.1415926535897932384626433832795;
}

namespace UnitVectors
{
	static const HMM_Vec3 Right		= HMM_NormV3(HMM_V3(1,0,0));
	static const HMM_Vec3 Forward	= HMM_NormV3(HMM_V3(0,1,0));
	static const HMM_Vec3 Up		= HMM_NormV3(HMM_V3(0,0,1));	
}

HMM_Vec3 quat_forward(HMM_Quat q) {
    HMM_Vec3 v = UnitVectors::Forward; 
    HMM_Vec3 u = HMM_V3(q.X, q.Y, q.Z);
    float s = q.W;

    HMM_Vec3 t = HMM_MulV3F(HMM_Cross(u, v), 2.0f);
    HMM_Vec3 result = HMM_AddV3(v,
                      HMM_AddV3(HMM_MulV3F(t, s),
                      HMM_Cross(u, t)));

    return result;
}

HMM_Vec3 rotate_vector(HMM_Vec3 in_vector_to_rotate, HMM_Vec3 in_axis, float in_magnitude)
{
    in_axis = HMM_NormV3(in_axis);
    HMM_Quat quat = HMM_QFromAxisAngle_RH(in_axis, in_magnitude);
    HMM_Mat4 rotation_matrix = HMM_QToM4(quat);
	HMM_Vec4 rotated = rotation_matrix * HMM_V4(in_vector_to_rotate.X, in_vector_to_rotate.Y, in_vector_to_rotate.Z, 0.f);
    return HMM_V3(rotated.X, rotated.Y, rotated.Z);
}

HMM_Vec3 vec3_projection(const HMM_Vec3 v, const HMM_Vec3 dir)
{
    float dir_length_squared = HMM_LenSqrV3(dir);
	return dir * HMM_DotV3(v,dir) / dir_length_squared;
}

HMM_Vec3 vec3_plane_projection(const HMM_Vec3 v, const HMM_Vec3 plane_normal)
{
	return v - vec3_projection(v, plane_normal);
}
