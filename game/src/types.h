#pragma once

#include <cstdint>

// std::numeric_limits
#include <limits>
static constexpr float LOWEST_FLOAT = std::numeric_limits<float>::lowest(); 
static constexpr float HIGHEST_FLOAT = std::numeric_limits<float>::max(); 

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

HMM_Vec3 HMM_MinV3(HMM_Vec3 a, HMM_Vec3 b)
{
    HMM_Vec3 r;
    r.X = (a.X < b.X) ? a.X : b.X;
    r.Y = (a.Y < b.Y) ? a.Y : b.Y;
    r.Z = (a.Z < b.Z) ? a.Z : b.Z;
    return r;
}

HMM_Vec3 HMM_MaxV3(HMM_Vec3 a, HMM_Vec3 b)
{
    HMM_Vec3 r;
    r.X = (a.X > b.X) ? a.X : b.X;
    r.Y = (a.Y > b.Y) ? a.Y : b.Y;
    r.Z = (a.Z > b.Z) ? a.Z : b.Z;
    return r;
}

struct BoundingBox
{
	HMM_Vec3 min;
	HMM_Vec3 max;
};

BoundingBox bounding_box_init()
{
	return (BoundingBox) {
		.min = HMM_V3(HIGHEST_FLOAT, HIGHEST_FLOAT, HIGHEST_FLOAT),
		.max = HMM_V3(LOWEST_FLOAT,LOWEST_FLOAT,LOWEST_FLOAT)
	};
}

void bounding_box_print(const BoundingBox& in_bounding_box, const char* in_label)
{
	printf(
		"%s min: (%f, %f, %f) max: (%f, %f, %f)\n", 
		in_label,
		in_bounding_box.min.X, in_bounding_box.min.Y, in_bounding_box.min.Z, 
		in_bounding_box.max.X, in_bounding_box.max.Y, in_bounding_box.max.Z
	);
}

BoundingBox bounding_box_transform(const BoundingBox& in_bounding_box, const Transform& in_transform)
{
	HMM_Vec3 corners[8] =
	{
		HMM_V3(in_bounding_box.min.X, in_bounding_box.min.Y, in_bounding_box.min.Z),
		HMM_V3(in_bounding_box.max.X, in_bounding_box.min.Y, in_bounding_box.min.Z),
		HMM_V3(in_bounding_box.min.X, in_bounding_box.max.Y, in_bounding_box.min.Z),
		HMM_V3(in_bounding_box.max.X, in_bounding_box.max.Y, in_bounding_box.min.Z),
		HMM_V3(in_bounding_box.min.X, in_bounding_box.min.Y, in_bounding_box.max.Z),
		HMM_V3(in_bounding_box.max.X, in_bounding_box.min.Y, in_bounding_box.max.Z),
		HMM_V3(in_bounding_box.min.X, in_bounding_box.max.Y, in_bounding_box.max.Z),
		HMM_V3(in_bounding_box.max.X, in_bounding_box.max.Y, in_bounding_box.max.Z),
	};

	for (int i = 0; i < 8; ++i)
	{
		// Scale Corners
		corners[i].X *= in_transform.scale.X;
		corners[i].Y *= in_transform.scale.Y;
		corners[i].Z *= in_transform.scale.Z;

		// Rotate Corners
		corners[i] = HMM_RotateV3Q(corners[i], in_transform.rotation);

		// Translate Corners
		corners[i].X += in_transform.location.X;
		corners[i].Y += in_transform.location.Y;
		corners[i].Z += in_transform.location.Z;
	}

	// Generate new bounding box after transform corners
	BoundingBox out_bounding_box = bounding_box_init();
	for (int i = 0; i < 8; ++i)
	{
		out_bounding_box.min = HMM_MinV3(out_bounding_box.min, corners[i]);
		out_bounding_box.max = HMM_MaxV3(out_bounding_box.max, corners[i]);
	}
	return out_bounding_box;
}

struct Frustum
{
	HMM_Vec4 planes[6];
};

Frustum frustum_create(const HMM_Mat4& in_view_proj)
{
	Frustum out_frustum = {};

	HMM_Vec4 (&frustum_planes)[6] = out_frustum.planes;

	// Extract using Gribb-Hartmann method

	// Left
	frustum_planes[0].X = in_view_proj.Elements[0][3] + in_view_proj.Elements[0][0];
	frustum_planes[0].Y = in_view_proj.Elements[1][3] + in_view_proj.Elements[1][0];
	frustum_planes[0].Z = in_view_proj.Elements[2][3] + in_view_proj.Elements[2][0];
	frustum_planes[0].W = in_view_proj.Elements[3][3] + in_view_proj.Elements[3][0];

	// Right
	frustum_planes[1].X = in_view_proj.Elements[0][3] - in_view_proj.Elements[0][0];
	frustum_planes[1].Y = in_view_proj.Elements[1][3] - in_view_proj.Elements[1][0];
	frustum_planes[1].Z = in_view_proj.Elements[2][3] - in_view_proj.Elements[2][0];
	frustum_planes[1].W = in_view_proj.Elements[3][3] - in_view_proj.Elements[3][0];

	// Bottom
	frustum_planes[2].X = in_view_proj.Elements[0][3] + in_view_proj.Elements[0][1];
	frustum_planes[2].Y = in_view_proj.Elements[1][3] + in_view_proj.Elements[1][1];
	frustum_planes[2].Z = in_view_proj.Elements[2][3] + in_view_proj.Elements[2][1];
	frustum_planes[2].W = in_view_proj.Elements[3][3] + in_view_proj.Elements[3][1];

	// Top
	frustum_planes[3].X = in_view_proj.Elements[0][3] - in_view_proj.Elements[0][1];
	frustum_planes[3].Y = in_view_proj.Elements[1][3] - in_view_proj.Elements[1][1];
	frustum_planes[3].Z = in_view_proj.Elements[2][3] - in_view_proj.Elements[2][1];
	frustum_planes[3].W = in_view_proj.Elements[3][3] - in_view_proj.Elements[3][1];

	// Near
	frustum_planes[4].X = in_view_proj.Elements[0][3] + in_view_proj.Elements[0][2];
	frustum_planes[4].Y = in_view_proj.Elements[1][3] + in_view_proj.Elements[1][2];
	frustum_planes[4].Z = in_view_proj.Elements[2][3] + in_view_proj.Elements[2][2];
	frustum_planes[4].W = in_view_proj.Elements[3][3] + in_view_proj.Elements[3][2];

	// Far
	frustum_planes[5].X = in_view_proj.Elements[0][3] - in_view_proj.Elements[0][2];
	frustum_planes[5].Y = in_view_proj.Elements[1][3] - in_view_proj.Elements[1][2];
	frustum_planes[5].Z = in_view_proj.Elements[2][3] - in_view_proj.Elements[2][2];
	frustum_planes[5].W = in_view_proj.Elements[3][3] - in_view_proj.Elements[3][2];

	// Normalize planes
	for (int i = 0; i < 6; i++)
	{
		float length = sqrtf(
			frustum_planes[i].X * frustum_planes[i].X +
			frustum_planes[i].Y * frustum_planes[i].Y +
			frustum_planes[i].Z * frustum_planes[i].Z
		);
		frustum_planes[i].X /= length;
		frustum_planes[i].Y /= length;
		frustum_planes[i].Z /= length;
		frustum_planes[i].W /= length;
	}

	return out_frustum;
}

/** Returns true if in_bounding_box should be culled for in_frustum */
bool frustum_cull(const Frustum& in_frustum, const BoundingBox& in_bounding_box)
{
    for (int i = 0; i < 6; i++)
	{
        // Compute positive vertex for this plane
        HMM_Vec3 p;
        p.X = (in_frustum.planes[i].X >= 0) ? in_bounding_box.max.X : in_bounding_box.min.X;
        p.Y = (in_frustum.planes[i].Y >= 0) ? in_bounding_box.max.Y : in_bounding_box.min.Y;
        p.Z = (in_frustum.planes[i].Z >= 0) ? in_bounding_box.max.Z : in_bounding_box.min.Z;

        // If positive vertex is outside, the box is fully outside this plane
        const float distance = in_frustum.planes[i].X * p.X + in_frustum.planes[i].Y * p.Y + in_frustum.planes[i].Z * p.Z + in_frustum.planes[i].W;
        if (distance < 0) {
            return true; // Cull
        }
    }
    return false; // Not culled
}
