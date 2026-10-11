#pragma once

#include <cmath>
#include <vector>

// World-space debug lines recorded during the frame's update and drawn by the
// debug UI as a see-through overlay. Colors are packed like IM_COL32 (ABGR).
namespace DebugDraw
{
	struct Line
	{
		HMM_Vec3 from;
		HMM_Vec3 to;
		u32 color;
	};

	constexpr u32 rgb(u32 in_r, u32 in_g, u32 in_b)
	{
		return 0xFF000000u | (in_b << 16) | (in_g << 8) | in_r;
	}

	constexpr u32 red = rgb(240, 70, 70);
	constexpr u32 green = rgb(80, 220, 100);
	constexpr u32 blue = rgb(80, 150, 255);
	constexpr u32 yellow = rgb(250, 220, 60);
	constexpr u32 orange = rgb(255, 150, 40);
	constexpr u32 magenta = rgb(230, 90, 230);
	constexpr u32 cyan = rgb(70, 220, 230);
	constexpr u32 white = rgb(235, 235, 235);

	inline std::vector<Line> lines;

	void line(HMM_Vec3 in_from, HMM_Vec3 in_to, u32 in_color)
	{
		lines.push_back({in_from, in_to, in_color});
	}

	// Small axis-aligned cross marking a point.
	void point(std::vector<Line>& io_lines, HMM_Vec3 in_at, u32 in_color, f32 in_size = 0.08f)
	{
		for (i32 axis = 0; axis < 3; ++axis)
		{
			HMM_Vec3 offset = HMM_V3(0, 0, 0);
			offset.Elements[axis] = in_size;
			io_lines.push_back({in_at - offset, in_at + offset, in_color});
		}
	}

	void point(HMM_Vec3 in_at, u32 in_color, f32 in_size = 0.08f)
	{
		point(lines, in_at, in_color, in_size);
	}

	// Connected polyline through in_points.
	void chain(std::initializer_list<HMM_Vec3> in_points, u32 in_color)
	{
		const HMM_Vec3* previous = nullptr;
		for (const HMM_Vec3& point_at : in_points)
		{
			if (previous)
			{
				line(*previous, point_at, in_color);
			}
			previous = &point_at;
		}
	}

	// Wire capsule: in_axis is the unit spine direction, in_half_height the
	// half-length of the cylinder part (end caps add in_radius beyond it).
	void capsule(std::vector<Line>& io_lines, HMM_Vec3 in_center, HMM_Vec3 in_axis, f32 in_half_height, f32 in_radius, u32 in_color)
	{
		constexpr i32 segments = 16;
		const HMM_Vec3 side_a = HMM_NormV3(HMM_Cross(in_axis, fabsf(in_axis.Z) < 0.9f ? HMM_V3(0, 0, 1) : HMM_V3(1, 0, 0)));
		const HMM_Vec3 side_b = HMM_Cross(in_axis, side_a);
		const HMM_Vec3 top = in_center + in_axis * in_half_height;
		const HMM_Vec3 bottom = in_center - in_axis * in_half_height;
		for (i32 segment = 0; segment < segments; ++segment)
		{
			const f32 angle_0 = HMM_PI32 * 2.0f * (f32) segment / segments;
			const f32 angle_1 = HMM_PI32 * 2.0f * (f32) (segment + 1) / segments;
			// Rings at both ends of the cylinder.
			const HMM_Vec3 ring_0 = (side_a * cosf(angle_0) + side_b * sinf(angle_0)) * in_radius;
			const HMM_Vec3 ring_1 = (side_a * cosf(angle_1) + side_b * sinf(angle_1)) * in_radius;
			io_lines.push_back({top + ring_0, top + ring_1, in_color});
			io_lines.push_back({bottom + ring_0, bottom + ring_1, in_color});
			// Two half-circle arcs over each cap.
			const f32 arc_0 = HMM_PI32 * (f32) segment / segments;
			const f32 arc_1 = HMM_PI32 * (f32) (segment + 1) / segments;
			for (const HMM_Vec3& side : {side_a, side_b})
			{
				const HMM_Vec3 cap_0 = (side * cosf(arc_0) + in_axis * sinf(arc_0)) * in_radius;
				const HMM_Vec3 cap_1 = (side * cosf(arc_1) + in_axis * sinf(arc_1)) * in_radius;
				io_lines.push_back({top + cap_0, top + cap_1, in_color});
				io_lines.push_back({bottom - cap_0, bottom - cap_1, in_color});
			}
		}
		for (const HMM_Vec3& side : {side_a, -side_a, side_b, -side_b})
		{
			io_lines.push_back({top + side * in_radius, bottom + side * in_radius, in_color});
		}
	}
}
