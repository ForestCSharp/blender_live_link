#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "state/state.h"

// Shared pieces of ArmIK and LegIK: math helpers, bone-hierarchy queries and
// the rest-reset / two-bone placement steps both limb solvers are built from.
namespace IK
{
	constexpr f32 epsilon = 1e-6f;
	// Rest bends below this are treated as straight modeling noise.
	constexpr f32 authored_bend_degrees = 2.0f;

	// ---- Math helpers ----

	bool is_finite(const HMM_Vec3& in_vector)
	{
		return std::isfinite(in_vector.X) && std::isfinite(in_vector.Y) && std::isfinite(in_vector.Z);
	}

	bool is_finite(const HMM_Mat4& in_matrix)
	{
		for (i32 column = 0; column < 4; ++column)
		{
			for (i32 row = 0; row < 4; ++row)
			{
				if (!std::isfinite(in_matrix.Elements[column][row]))
				{
					return false;
				}
			}
		}
		return true;
	}

	bool nearly_equal(const HMM_Mat4& in_a, const HMM_Mat4& in_b)
	{
		for (i32 column = 0; column < 4; ++column)
		{
			for (i32 row = 0; row < 4; ++row)
			{
				if (fabsf(in_a.Elements[column][row] - in_b.Elements[column][row]) >= epsilon)
				{
					return false;
				}
			}
		}
		return true;
	}

	HMM_Vec3 direction(const HMM_Mat4& in_matrix, HMM_Vec3 in_vector)
	{
		return (in_matrix * HMM_V4V(in_vector, 0.0f)).XYZ;
	}

	HMM_Vec3 transform_point(const HMM_Mat4& in_matrix, HMM_Vec3 in_point)
	{
		return (in_matrix * HMM_V4V(in_point, 1.0f)).XYZ;
	}

	HMM_Vec3 any_perpendicular(HMM_Vec3 in_vector)
	{
		const HMM_Vec3 reference = fabsf(in_vector.Z) < 0.9f ? HMM_V3(0, 0, 1) : HMM_V3(1, 0, 0);
		return HMM_NormV3(HMM_Cross(in_vector, reference));
	}

	// Deterministic shortest-arc rotation, including the antiparallel case.
	HMM_Mat4 swing(HMM_Vec3 in_from, HMM_Vec3 in_to)
	{
		const HMM_Vec3 from = HMM_NormV3(in_from);
		const HMM_Vec3 to = HMM_NormV3(in_to);
		const f32 dot = HMM_Clamp(-1.0f, HMM_DotV3(from, to), 1.0f);
		if (dot < -1.0f + epsilon)
		{
			const HMM_Vec3 axis = any_perpendicular(from);
			return HMM_QToM4(HMM_Q(axis.X, axis.Y, axis.Z, 0.0f));
		}
		const HMM_Vec3 axis = HMM_Cross(from, to);
		return HMM_QToM4(HMM_NormQ(HMM_Q(axis.X, axis.Y, axis.Z, 1.0f + dot)));
	}

	HMM_Mat4 rotate_about(const HMM_Mat4& in_rotation, HMM_Vec3 in_pivot)
	{
		return HMM_Translate(in_pivot) * in_rotation * HMM_Translate(-in_pivot);
	}

	// Angle at in_middle as deviation from straight (0 = straight).
	f32 bend_angle(HMM_Vec3 in_root, HMM_Vec3 in_middle, HMM_Vec3 in_end)
	{
		const HMM_Vec3 upper = in_middle - in_root;
		const HMM_Vec3 lower = in_end - in_middle;
		return acosf(HMM_Clamp(-1.0f, HMM_DotV3(upper, lower) / (HMM_LenV3(upper) * HMM_LenV3(lower)), 1.0f));
	}

	// ---- Bone hierarchy ----

	i32 named_bone(const Armature& in_rig, const std::string& in_name)
	{
		for (u32 bone_idx = 0; bone_idx < in_rig.bone_count; ++bone_idx)
		{
			if (in_rig.bones[bone_idx].name && in_name == in_rig.bones[bone_idx].name)
			{
				return (i32) bone_idx;
			}
		}
		return -1;
	}

	// Parent steps from in_bone up to in_root, or -1 when in_bone is outside
	// in_root's subtree. Bounded, so malformed parent cycles terminate.
	i32 depth_below(const Armature& in_rig, i32 in_bone, i32 in_root)
	{
		for (i32 depth = 0; depth < (i32) in_rig.bone_count; ++depth)
		{
			if (in_bone < 0 || in_bone >= (i32) in_rig.bone_count)
			{
				return -1;
			}
			if (in_bone == in_root)
			{
				return depth;
			}
			in_bone = in_rig.bones[in_bone].parent_index;
		}
		return -1;
	}

	// True when walking parents from in_bone reaches a root without leaving the
	// rig or revisiting a bone.
	bool has_rooted_ancestry(const Armature& in_rig, i32 in_bone)
	{
		for (u32 step = 0; step <= in_rig.bone_count; ++step)
		{
			if (in_bone == -1)
			{
				return true;
			}
			if (in_bone < 0 || in_bone >= (i32) in_rig.bone_count)
			{
				return false;
			}
			in_bone = in_rig.bones[in_bone].parent_index;
		}
		return false;
	}

	// ---- Solver steps ----

	// A limb's bones as (depth, bone), sorted so parents precede children.
	using Subtree = std::vector<std::pair<i32, i32>>;

	Subtree collect_subtree(const Armature& in_rig, i32 in_root)
	{
		Subtree subtree;
		for (u32 bone_idx = 0; bone_idx < in_rig.bone_count; ++bone_idx)
		{
			const i32 depth = depth_below(in_rig, (i32) bone_idx, in_root);
			if (depth >= 0)
			{
				subtree.push_back({depth, (i32) bone_idx});
			}
		}
		std::sort(subtree.begin(), subtree.end());
		return subtree;
	}

	// The sampled pose with in_subtree reset to rest locals under its sampled
	// parent. pose[parent] * inverse_bind[parent] is the parent's bind-to-pose
	// delta, so each bone keeps its rest local relative to its parent.
	std::vector<HMM_Mat4> rest_pose_under_parent(const Armature& in_rig, const Subtree& in_subtree)
	{
		std::vector<HMM_Mat4> pose = in_rig.evaluated_pose;
		for (const auto& [depth, bone] : in_subtree)
		{
			const i32 parent = in_rig.bones[bone].parent_index;
			pose[bone] = HMM_InvGeneralM4(in_rig.bones[bone].inverse_bind_matrix);
			if (parent >= 0)
			{
				pose[bone] = pose[parent] * in_rig.bones[parent].inverse_bind_matrix * pose[bone];
			}
		}
		return pose;
	}

	// Applies in_transform to in_bone and everything below it.
	void transform_below(const Armature& in_rig, const Subtree& in_subtree, i32 in_bone,
		const HMM_Mat4& in_transform, std::vector<HMM_Mat4>& io_pose)
	{
		for (const auto& [depth, bone] : in_subtree)
		{
			if (depth_below(in_rig, bone, in_bone) >= 0)
			{
				io_pose[bone] = in_transform * io_pose[bone];
			}
		}
	}

	// Two-bone placement: points root->end along in_aim (setting the baseline
	// twist), then bends at in_middle so in_end lands in_reach along the aim
	// line. The bend keeps the rest bend's side; a straight rest limb bends
	// toward in_fallback_pole instead. Returns the pole used, in rig space.
	HMM_Vec3 place_two_bone(const Armature& in_rig, const Subtree& in_subtree, i32 in_root, i32 in_middle, i32 in_end,
		HMM_Vec3 in_aim, f32 in_reach, f32 in_rest_bend, HMM_Vec3 in_fallback_pole, std::vector<HMM_Mat4>& io_pose)
	{
		const HMM_Vec3 root = io_pose[in_root].Columns[3].XYZ;
		transform_below(in_rig, in_subtree, in_root,
			rotate_about(swing(io_pose[in_end].Columns[3].XYZ - root, in_aim), root), io_pose);

		const HMM_Vec3 aimed_middle = io_pose[in_middle].Columns[3].XYZ;
		const f32 upper_length = HMM_LenV3(aimed_middle - root);
		const f32 lower_length = HMM_LenV3(io_pose[in_end].Columns[3].XYZ - aimed_middle);
		const f32 upper_sq = upper_length * upper_length;

		// Middle joint relative to the aim line: distance along it, then distance off it.
		const f32 along_unclamped = (upper_sq - lower_length * lower_length + in_reach * in_reach) / (2.0f * in_reach);
		const f32 along = HMM_Clamp(-upper_length, along_unclamped, upper_length);
		const f32 bend = sqrtf(std::max(0.0f, upper_sq - along * along));

		// Pole: which way the middle joint sticks out, perpendicular to aim. Prefer the rest bend's direction.
		HMM_Vec3 pole = (aimed_middle - root) - in_aim * HMM_DotV3(aimed_middle - root, in_aim);
		if (in_rest_bend < HMM_AngleDeg(authored_bend_degrees) || HMM_LenSqrV3(pole) < epsilon * upper_sq)
		{
			pole = in_fallback_pole - in_aim * HMM_DotV3(in_fallback_pole, in_aim);
			if (HMM_LenSqrV3(pole) < epsilon * HMM_LenSqrV3(in_fallback_pole))
			{
				// Aim parallel to the fallback: any fixed perpendicular will do.
				pole = any_perpendicular(in_aim);
			}
		}
		pole = HMM_NormV3(pole);

		// Swing the upper bone onto the bent middle joint, then the lower bone onto the end.
		const HMM_Vec3 bent_middle = root + in_aim * along + pole * bend;
		const HMM_Vec3 bent_end = root + in_aim * in_reach;
		transform_below(in_rig, in_subtree, in_root, rotate_about(swing(aimed_middle - root, bent_middle - root), root), io_pose);
		const HMM_Vec3 swung_end = io_pose[in_end].Columns[3].XYZ;
		transform_below(in_rig, in_subtree, in_middle,
			rotate_about(swing(swung_end - bent_middle, bent_end - bent_middle), bent_middle), io_pose);
		return pole;
	}

	// Transactional commit: a non-finite solve leaves the sampled pose untouched.
	bool commit_if_finite(Armature& io_rig, const Subtree& in_subtree, std::vector<HMM_Mat4>& io_pose, std::string& out_error)
	{
		for (const auto& [depth, bone] : in_subtree)
		{
			if (!is_finite(io_pose[bone]))
			{
				out_error = "non-finite solved pose";
				return false;
			}
		}
		io_rig.evaluated_pose = std::move(io_pose);
		return true;
	}
}
