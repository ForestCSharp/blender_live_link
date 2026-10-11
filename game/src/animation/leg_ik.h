#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyLock.h>

#include "state/state.h"
#include "physics/physics_system.h"

// Procedural ground placement for the mech's feet. Runs after base poses are
// sampled and after arm IK, so it sees the frame's finalized Legs transform
// and the already-stepped physics world.
namespace LegIK
{
	constexpr f32 epsilon = 1e-6f;
	// Rest bends below this are treated as straight modeling noise.
	constexpr f32 authored_bend_degrees = 2.0f;
	// Margin above the animated foot the raycast starts from.
	constexpr f32 ground_cast_up_bias_m = 1.0f;
	// Extra reach below full leg extension, so a step down still finds ground.
	constexpr f32 ground_cast_step_down_margin_m = 1.0f;
	// Ground steeper than ~45 degrees is treated as flat when placing the foot.
	constexpr f32 max_ground_tilt_cos = 0.7f;

	struct Chain
	{
		i32 hip = -1;
		i32 knee = -1;
		i32 hock = -1;	// Reverse joint between knee and foot (e.g. a digitigrade ankle).
		i32 foot = -1;

		bool operator==(const Chain&) const = default;
	};

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

	// Bind-pose armature-space X, used only to tell the rig's two labeled feet
	// apart when neither is explicitly named.
	f32 bind_pose_x(const Armature& in_rig, i32 in_bone)
	{
		return HMM_InvGeneralM4(in_rig.bones[in_bone].inverse_bind_matrix).Columns[3].X;
	}

	// Explicit names win. With both feet left blank, the rig's two
	// attachment-labeled bones are used, split into left/right by bind-pose X
	// (more negative X is treated as left; flip here if an authored rig
	// disagrees). An empty knee, hock, or hip uses the parent.
	bool resolve(const Armature& in_rig, const Part& in_part, Chain& out_left, Chain& out_right, std::string& out_error)
	{
		out_left = {};
		out_right = {};

		i32 left_foot = -1;
		i32 right_foot = -1;
		if (!in_part.ik_left_foot_bone.empty() || !in_part.ik_right_foot_bone.empty())
		{
			left_foot = named_bone(in_rig, in_part.ik_left_foot_bone);
			right_foot = named_bone(in_rig, in_part.ik_right_foot_bone);
			if (left_foot < 0)
			{
				out_error = "left foot bone is missing";
				return false;
			}
			if (right_foot < 0)
			{
				out_error = "right foot bone is missing";
				return false;
			}
		}
		else
		{
			i32 labeled[2] = {-1, -1};
			i32 labeled_count = 0;
			for (u32 bone_idx = 0; bone_idx < in_rig.bone_count; ++bone_idx)
			{
				const char* label = in_rig.bones[bone_idx].attachment_label;
				if (!label || !label[0])
				{
					continue;
				}
				if (labeled_count >= 2)
				{
					out_error = "expected two labeled feet; set Left/Right Foot Bone";
					return false;
				}
				labeled[labeled_count++] = (i32) bone_idx;
			}
			if (labeled_count != 2)
			{
				out_error = "expected two labeled feet; set Left/Right Foot Bone";
				return false;
			}
			if (bind_pose_x(in_rig, labeled[0]) <= bind_pose_x(in_rig, labeled[1]))
			{
				left_foot = labeled[0];
				right_foot = labeled[1];
			}
			else
			{
				left_foot = labeled[1];
				right_foot = labeled[0];
			}
		}

		auto resolve_side = [&](i32 in_foot, const std::string& in_knee_name, const std::string& in_hock_name,
			const std::string& in_hip_name, Chain& out_chain) -> bool
		{
			out_chain.foot = in_foot;
			out_chain.hock = in_hock_name.empty()
				? in_rig.bones[out_chain.foot].parent_index
				: named_bone(in_rig, in_hock_name);
			if (out_chain.hock < 0 || out_chain.hock >= (i32) in_rig.bone_count)
			{
				out_error = "hock bone is missing";
				return false;
			}

			out_chain.knee = in_knee_name.empty()
				? in_rig.bones[out_chain.hock].parent_index
				: named_bone(in_rig, in_knee_name);
			if (out_chain.knee < 0 || out_chain.knee >= (i32) in_rig.bone_count)
			{
				out_error = "knee bone is missing";
				return false;
			}

			out_chain.hip = in_hip_name.empty()
				? in_rig.bones[out_chain.knee].parent_index
				: named_bone(in_rig, in_hip_name);
			if (out_chain.hip < 0 || out_chain.hip >= (i32) in_rig.bone_count)
			{
				out_error = "hip bone is missing";
				return false;
			}

			if (in_rig.bones[out_chain.foot].parent_index != out_chain.hock ||
				in_rig.bones[out_chain.hock].parent_index != out_chain.knee ||
				in_rig.bones[out_chain.knee].parent_index != out_chain.hip)
			{
				out_error = "expected direct hip -> knee -> hock -> foot hierarchy";
				return false;
			}
			if (!has_rooted_ancestry(in_rig, out_chain.foot))
			{
				out_error = "invalid bone hierarchy";
				return false;
			}
			return true;
		};

		if (!resolve_side(left_foot, in_part.ik_left_knee_bone, in_part.ik_left_hock_bone, in_part.ik_left_hip_bone, out_left))
		{
			return false;
		}
		if (!resolve_side(right_foot, in_part.ik_right_knee_bone, in_part.ik_right_hock_bone, in_part.ik_right_hip_bone, out_right))
		{
			return false;
		}

		// Rejects parent cycles and a shared chain between the two legs.
		if (depth_below(in_rig, out_left.hip, out_right.hip) >= 0 ||
			depth_below(in_rig, out_right.hip, out_left.hip) >= 0)
		{
			out_error = "overlapping left/right leg chains";
			return false;
		}
		return true;
	}

	// ---- Ground query ----

	struct NonMovingLayerFilter : public JPH::ObjectLayerFilter
	{
		bool ShouldCollide(JPH::ObjectLayer in_layer) const override
		{
			return in_layer == Layers::NON_MOVING;
		}
	};

	// Thigh + shin + hock length in the animated pose, or -1 when any segment
	// is degenerate.
	f32 leg_length(const Armature& in_rig, const Chain& in_chain)
	{
		const HMM_Vec3 hip = in_rig.evaluated_pose[in_chain.hip].Columns[3].XYZ;
		const HMM_Vec3 knee = in_rig.evaluated_pose[in_chain.knee].Columns[3].XYZ;
		const HMM_Vec3 hock = in_rig.evaluated_pose[in_chain.hock].Columns[3].XYZ;
		const HMM_Vec3 foot = in_rig.evaluated_pose[in_chain.foot].Columns[3].XYZ;
		const f32 thigh_length = HMM_LenV3(knee - hip);
		const f32 shin_length = HMM_LenV3(hock - knee);
		const f32 hock_length = HMM_LenV3(foot - hock);
		if (!std::isfinite(thigh_length + shin_length + hock_length) ||
			thigh_length < epsilon || shin_length < epsilon || hock_length < epsilon)
		{
			return -1.0f;
		}
		return thigh_length + shin_length + hock_length;
	}

	// Height of the first static surface straight below in_from, and optionally its normal.
	bool ground_below(HMM_Vec3 in_from, f32 in_max_distance, f32& out_ground_z, HMM_Vec3* out_normal)
	{
		JPH::RRayCast ray(JPH::RVec3(in_from.X, in_from.Y, in_from.Z), JPH::Vec3(0, 0, -in_max_distance));
		JPH::RayCastResult hit;
		NonMovingLayerFilter layer_filter;
		if (!jolt_state.physics_system.GetNarrowPhaseQuery().CastRay(ray, hit, {}, layer_filter, {}))
		{
			return false;
		}
		const JPH::RVec3 hit_point = ray.GetPointOnRay(hit.mFraction);
		out_ground_z = (f32) hit_point.GetZ();
		if (out_normal)
		{
			JPH::BodyLockRead lock(jolt_state.physics_system.GetBodyLockInterface(), hit.mBodyID);
			if (!lock.Succeeded())
			{
				return false;
			}
			const JPH::Vec3 normal = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hit_point);
			*out_normal = HMM_V3(normal.GetX(), normal.GetY(), normal.GetZ());
		}
		return true;
	}

	// Casts straight down from the animated (pre-IK) foot position, so a walk
	// cycle's horizontal foot placement survives. Fails closed: a miss within
	// leg reach plus a small step-down margin leaves the leg's already-sampled
	// pose untouched rather than snapping it to a grotesque full stretch.
	bool find_ground_target(const Armature& in_rig, const Chain& in_chain, const HMM_Mat4& in_rig_world,
		f32 in_root_ground_z, HMM_Vec3& out_target_world, HMM_Vec3& out_normal_world, std::string& out_error)
	{
		const f32 length = leg_length(in_rig, in_chain);
		if (length < 0.0f)
		{
			out_error = "zero-length leg segment";
			return false;
		}

		const HMM_Vec3 animated_foot_world = transform_point(in_rig_world, in_rig.evaluated_pose[in_chain.foot].Columns[3].XYZ);
		if (!is_finite(animated_foot_world))
		{
			out_error = "degenerate rig transform";
			return false;
		}

		// Start above the foot so a step up is still found.
		f32 foot_ground_z = 0.0f;
		if (!ground_below(animated_foot_world + HMM_V3(0, 0, ground_cast_up_bias_m),
			ground_cast_up_bias_m + length + ground_cast_step_down_margin_m, foot_ground_z, &out_normal_world))
		{
			out_error = "no ground within reach";
			return false;
		}
		// A ray grazing a ledge can report the side face; don't stand the foot on its side.
		if (out_normal_world.Z < max_ground_tilt_cos)
		{
			out_normal_world = HMM_V3(0, 0, 1);
		}

		// The foot bone's head is the ankle, not the sole, so it can't sit on the
		// hit point. The animation is authored for the ground under the mech, so
		// only shift the animated foot by how much its own ground differs.
		out_target_world = animated_foot_world + HMM_V3(0, 0, foot_ground_z - in_root_ground_z);
		return true;
	}

	// ---- Solver ----

	// Resets the hip subtree (including toes) to rest locals under the sampled
	// parent, then solves in stages. A 3-bone chain targeting one point is
	// underdetermined, so the hock's direction is chosen first: it keeps its
	// animated slant, straightening toward the hip-to-target line only when
	// the target is otherwise out of reach. That leaves a standard 2-bone
	// thigh/shin solve onto the hock target, a terminal hock aim onto the
	// ground target (as arm IK's wrist step), and finally the foot is laid
	// flat on the ground with its animated heading.
	//
	// Transactional: a failed solve leaves the sampled pose untouched.
	bool solve(Armature& io_rig, const Chain& in_chain, const HMM_Mat4& in_rig_world,
		HMM_Vec3 in_target_world, HMM_Vec3 in_ground_normal_world, HMM_Vec3 in_pole_hint_world, std::string& out_error)
	{
		if (io_rig.evaluated_pose.size() != io_rig.bone_count)
		{
			out_error = "pose is unavailable";
			return false;
		}
		if (depth_below(io_rig, in_chain.foot, in_chain.hock) != 1 ||
			depth_below(io_rig, in_chain.hock, in_chain.knee) != 1 ||
			depth_below(io_rig, in_chain.knee, in_chain.hip) != 1 ||
			!has_rooted_ancestry(io_rig, in_chain.hip))
		{
			out_error = "invalid bone hierarchy";
			return false;
		}

		const HMM_Mat4 world_to_rig = HMM_InvGeneralM4(in_rig_world);
		const HMM_Vec3 target = transform_point(world_to_rig, in_target_world);
		const HMM_Vec3 pole_hint = direction(world_to_rig, in_pole_hint_world);
		const HMM_Vec3 ground_normal = HMM_NormV3(direction(world_to_rig, in_ground_normal_world));
		if (!is_finite(world_to_rig) || !is_finite(target) || !is_finite(pole_hint) || !is_finite(ground_normal))
		{
			out_error = "degenerate target or rig transform";
			return false;
		}

		// Hip subtree as (depth, bone), sorted so parents precede children.
		std::vector<std::pair<i32, i32>> subtree;
		for (u32 bone_idx = 0; bone_idx < io_rig.bone_count; ++bone_idx)
		{
			const i32 depth = depth_below(io_rig, (i32) bone_idx, in_chain.hip);
			if (depth >= 0)
			{
				subtree.push_back({depth, (i32) bone_idx});
			}
		}
		std::sort(subtree.begin(), subtree.end());

		// pose[parent] * inverse_bind[parent] is the parent's bind-to-pose delta,
		// so each bone keeps its rest local relative to its parent.
		std::vector<HMM_Mat4> pose = io_rig.evaluated_pose;
		for (const auto& [depth, bone] : subtree)
		{
			const i32 parent = io_rig.bones[bone].parent_index;
			pose[bone] = HMM_InvGeneralM4(io_rig.bones[bone].inverse_bind_matrix);
			if (parent >= 0)
			{
				pose[bone] = pose[parent] * io_rig.bones[parent].inverse_bind_matrix * pose[bone];
			}
		}

		const HMM_Vec3 hip = pose[in_chain.hip].Columns[3].XYZ;
		const HMM_Vec3 knee = pose[in_chain.knee].Columns[3].XYZ;
		const HMM_Vec3 hock = pose[in_chain.hock].Columns[3].XYZ;
		const HMM_Vec3 foot = pose[in_chain.foot].Columns[3].XYZ;
		const f32 thigh_length = HMM_LenV3(knee - hip);
		const f32 shin_length = HMM_LenV3(hock - knee);
		const f32 hock_length = HMM_LenV3(foot - hock);
		const f32 target_distance = HMM_LenV3(target - hip);
		if (!std::isfinite(thigh_length + shin_length + hock_length + target_distance) ||
			thigh_length < epsilon || shin_length < epsilon || hock_length < epsilon || target_distance < epsilon)
		{
			out_error = "zero-length segment or degenerate hip-to-target distance";
			return false;
		}

		const f32 upper_sq = thigh_length * thigh_length;
		const f32 lower_sq = shin_length * shin_length;
		const f32 full_reach = thigh_length + shin_length;
		const f32 min_reach = fabsf(thigh_length - shin_length) + epsilon * full_reach;
		// Debug-only: state.animation.leg_ik_max_extension_scale lets a developer
		// shrink how far the thigh/shin pair is allowed to straighten, to see the
		// solver visibly react (the "Leg Max Extension" slider under Animation).
		const f32 max_reach = std::max(min_reach, full_reach * state.animation.leg_ik_max_extension_scale);

		// Back the thigh/shin target off the ground point by the hock segment.
		// It keeps its animated slant when the target is in reach; past that it
		// straightens toward the hip-to-target line only as far as needed, so
		// the whole thigh/shin/hock chain contributes to the extension.
		const HMM_Mat4& animated_foot = io_rig.evaluated_pose[in_chain.foot];
		const HMM_Vec3 animated_hock_dir = HMM_NormV3(animated_foot.Columns[3].XYZ - io_rig.evaluated_pose[in_chain.hock].Columns[3].XYZ);
		const HMM_Vec3 straight_hock_dir = HMM_NormV3(target - hip);
		auto hock_target_at = [&](f32 in_straighten)
		{
			return target - HMM_NormV3(HMM_LerpV3(animated_hock_dir, in_straighten, straight_hock_dir)) * hock_length;
		};
		f32 straighten = 0.0f;
		if (HMM_LenV3(hock_target_at(0.0f) - hip) > max_reach)
		{
			// Distance to the hip shrinks monotonically as the hock straightens; bisect for the least straightening.
			f32 low = 0.0f;
			f32 high = 1.0f;
			for (i32 step = 0; step < 16; ++step)
			{
				const f32 mid = 0.5f * (low + high);
				(HMM_LenV3(hock_target_at(mid) - hip) > max_reach ? low : high) = mid;
			}
			straighten = high;
		}
		const HMM_Vec3 hock_target = hock_target_at(straighten);

		// Point the rest leg at the hock target first; this sets the baseline twist.
		const HMM_Vec3 aim = HMM_NormV3(hock_target - hip);
		const HMM_Mat4 leg_rotation = rotate_about(swing(hock - hip, aim), hip);
		for (const auto& [depth, bone] : subtree)
		{
			pose[bone] = leg_rotation * pose[bone];
		}

		// ---- Work out where the bent knee and hock should go ----

		// Rest knee angle, measured as deviation from a straight leg (0 = straight).
		const f32 rest_bend = acosf(HMM_Clamp(-1.0f, HMM_DotV3(knee - hip, hock - knee) / (thigh_length * shin_length), 1.0f));

		// Unlike arm IK, the reach comes from the hock target's distance, not a chosen bend angle.
		const f32 hock_distance = HMM_LenV3(hock_target - hip);
		const f32 target_reach = HMM_Clamp(min_reach, hock_distance, max_reach);

		// Knee position relative to the aim line: distance along it, then distance off it.
		const f32 along_unclamped = (upper_sq - lower_sq + target_reach * target_reach) / (2.0f * target_reach);
		const f32 along = HMM_Clamp(-thigh_length, along_unclamped, thigh_length);
		const f32 bend = sqrtf(std::max(0.0f, upper_sq - along * along));

		// Pole: which way the knee sticks out, perpendicular to aim. Prefer the rest bend's direction.
		const HMM_Vec3 aimed_knee = pose[in_chain.knee].Columns[3].XYZ;
		HMM_Vec3 pole = (aimed_knee - hip) - aim * HMM_DotV3(aimed_knee - hip, aim);
		if (rest_bend < HMM_AngleDeg(authored_bend_degrees) || HMM_LenSqrV3(pole) < epsilon * thigh_length * thigh_length)
		{
			// Straight rest leg: bend the knee forward, projected perpendicular to aim.
			pole = pole_hint - aim * HMM_DotV3(pole_hint, aim);
			if (HMM_LenSqrV3(pole) < epsilon * HMM_LenSqrV3(pole_hint))
			{
				// Target directly above or below the hip: any fixed perpendicular will do.
				pole = any_perpendicular(aim);
			}
		}
		pole = HMM_NormV3(pole);

		// Target joint positions: hock on the aim line, knee offset from it along the pole.
		const HMM_Vec3 bent_knee = hip + aim * along + pole * bend;
		const HMM_Vec3 bent_hock = hip + aim * target_reach;

		// Swing the thigh onto the new knee, then the shin onto the hock.
		const HMM_Mat4 upper_rotation = rotate_about(swing(aimed_knee - hip, bent_knee - hip), hip);
		for (const auto& [depth, bone] : subtree)
		{
			pose[bone] = upper_rotation * pose[bone];
		}

		const HMM_Vec3 swung_hock = pose[in_chain.hock].Columns[3].XYZ;
		const HMM_Mat4 lower_rotation = rotate_about(swing(swung_hock - bent_knee, bent_hock - bent_knee), bent_knee);
		for (const auto& [depth, bone] : subtree)
		{
			if (depth_below(io_rig, bone, in_chain.knee) >= 0)
			{
				pose[bone] = lower_rotation * pose[bone];
			}
		}

		// ---- Aim the hock the rest of the way at the actual ground target ----

		const HMM_Vec3 placed_hock = pose[in_chain.hock].Columns[3].XYZ;
		const HMM_Vec3 placed_foot = pose[in_chain.foot].Columns[3].XYZ;
		const HMM_Mat4 hock_rotation = rotate_about(swing(placed_foot - placed_hock, target - placed_hock), placed_hock);
		for (const auto& [depth, bone] : subtree)
		{
			if (depth_below(io_rig, bone, in_chain.hock) >= 0)
			{
				pose[bone] = hock_rotation * pose[bone];
			}
		}

		// Place the foot flat on the ground at its solved position: keep the
		// animated heading, but tilt it so its rest-pose up (the rig's +Z, where
		// the sole was modeled flat) matches the ground normal.
		const HMM_Vec3 animated_foot_position = animated_foot.Columns[3].XYZ;
		const HMM_Vec3 foot_rest_up = direction(io_rig.bones[in_chain.foot].inverse_bind_matrix, HMM_V3(0, 0, 1));
		const HMM_Mat4 flatten = rotate_about(swing(direction(animated_foot, foot_rest_up), ground_normal), animated_foot_position);
		const HMM_Vec3 solved_foot = pose[in_chain.foot].Columns[3].XYZ;
		const HMM_Mat4 foot_correction = HMM_Translate(solved_foot - animated_foot_position) * flatten * animated_foot * HMM_InvGeneralM4(pose[in_chain.foot]);
		for (const auto& [depth, bone] : subtree)
		{
			if (depth_below(io_rig, bone, in_chain.foot) >= 0)
			{
				pose[bone] = foot_correction * pose[bone];
			}
		}

		for (const auto& [depth, bone] : subtree)
		{
			if (!is_finite(pose[bone]))
			{
				out_error = "non-finite solved pose";
				return false;
			}
		}
		io_rig.evaluated_pose = std::move(pose);
		return true;
	}

	// ---- Mech integration ----

	void update(MechInstance& io_mech, std::string& io_diagnostics)
	{
		auto report = [&](i32 in_side, const std::string& in_error) {
			io_diagnostics += std::string(in_side == 0 ? " error[Left Leg IK]=" : " error[Right Leg IK]=") + in_error;
		};

		const i32 part_idx = (i32) PartType::Legs;
		auto legs = state.scene.objects.find(io_mech.part_instance_uids[part_idx]);
		auto legs_template = state.scene.objects.find(io_mech.part_template_uids[part_idx]);
		if (legs == state.scene.objects.end() || legs_template == state.scene.objects.end() || !legs->second.visibility)
		{
			return;
		}
		auto rig = legs->second.has_mesh
			? state.scene.objects.find(legs->second.mesh.armature_id)
			: state.scene.objects.end();
		if (rig == state.scene.objects.end() || !rig->second.has_armature)
		{
			report(0, "legs rig is missing");
			report(1, "legs rig is missing");
			return;
		}

		Chain left_chain;
		Chain right_chain;
		std::string resolve_error;
		if (!resolve(rig->second.armature, legs_template->second.part, left_chain, right_chain, resolve_error))
		{
			report(0, resolve_error);
			report(1, resolve_error);
			return;
		}

		const HMM_Mat4 rig_world = object_get_model_matrix(legs->second) * legs->second.mesh.armature_to_mesh;

		// Ground under the mech, cast once for both legs. Starts at the rig origin
		// with no upward bias, so geometry overhead (a doorway, a bridge) can't be
		// mistaken for the ground.
		const Armature& armature = rig->second.armature;
		if (armature.evaluated_pose.size() != armature.bone_count)
		{
			report(0, "pose is unavailable");
			report(1, "pose is unavailable");
			return;
		}
		const f32 root_cast_distance = std::max(leg_length(armature, left_chain), leg_length(armature, right_chain)) + ground_cast_step_down_margin_m;
		f32 root_ground_z = 0.0f;
		if (!ground_below(rig_world.Columns[3].XYZ, root_cast_distance, root_ground_z, nullptr))
		{
			report(0, "no ground under mech");
			report(1, "no ground under mech");
			return;
		}
		// Confirmed project convention: Blender/game forward is +Y, up is +Z.
		const HMM_Vec3 legs_forward_world = direction(HMM_QToM4(legs->second.current_transform.rotation), HMM_V3(0, 1, 0));

		const Chain chains[2] = {left_chain, right_chain};
		for (i32 side = 0; side < 2; ++side)
		{
			HMM_Vec3 target_world;
			HMM_Vec3 ground_normal_world;
			std::string ground_error;
			if (!find_ground_target(rig->second.armature, chains[side], rig_world, root_ground_z, target_world, ground_normal_world, ground_error))
			{
				report(side, ground_error);
				continue;
			}
			std::string solve_error;
			if (!solve(rig->second.armature, chains[side], rig_world, target_world, ground_normal_world, legs_forward_world, solve_error))
			{
				report(side, solve_error);
			}
		}
	}
}
