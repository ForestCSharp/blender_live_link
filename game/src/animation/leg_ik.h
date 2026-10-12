#pragma once

#include <string>

#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyLock.h>

#include "animation/ik_common.h"
#include "physics/physics_system.h"
#include "render/debug_draw.h"

// Procedural ground placement for the mech's feet. Runs after base poses are
// sampled and after arm IK, so it sees the frame's finalized Legs transform
// and the already-stepped physics world.
namespace LegIK
{
	using namespace IK;

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

	// ---- Bone hierarchy ----

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
		f32 in_rig_sole_z, f32& out_ground_offset, HMM_Vec3& out_normal_world, std::string& out_error)
	{
		const f32 length = leg_length(in_rig, in_chain);
		if (length < 0.0f)
		{
			out_error = "zero-length leg segment";
			return false;
		}

		const HMM_Vec3 animated_foot = in_rig.evaluated_pose[in_chain.foot].Columns[3].XYZ;
		const HMM_Vec3 animated_foot_world = transform_point(in_rig_world, animated_foot);
		if (!is_finite(animated_foot_world))
		{
			out_error = "degenerate rig transform";
			return false;
		}

		// Start above the foot so a step up is still found.
		const HMM_Vec3 ray_start = animated_foot_world + HMM_V3(0, 0, ground_cast_up_bias_m);
		const f32 ray_length = ground_cast_up_bias_m + length + ground_cast_step_down_margin_m;
		f32 foot_ground_z = 0.0f;
		if (!ground_below(ray_start, ray_length, foot_ground_z, &out_normal_world))
		{
			if (state.character.show_ik)
			{
				DebugDraw::line(ray_start, ray_start - HMM_V3(0, 0, ray_length), DebugDraw::red);
			}
			out_error = "no ground within reach";
			return false;
		}
		// A ray grazing a ledge can report the side face; don't stand the foot on its side.
		const bool too_steep = out_normal_world.Z < max_ground_tilt_cos;
		if (state.character.show_ik)
		{
			const HMM_Vec3 hit = HMM_V3(ray_start.X, ray_start.Y, foot_ground_z);
			DebugDraw::line(ray_start, hit, DebugDraw::green);
			DebugDraw::line(hit, hit + out_normal_world * 0.4f, too_steep ? DebugDraw::orange : DebugDraw::cyan);
		}
		if (too_steep)
		{
			out_normal_world = HMM_V3(0, 0, 1);
		}

		// The foot bone's head is the ankle, not the sole, so it can't sit on the
		// hit point. Move the animated foot by the gap between the real ground and
		// the sole plane the animation was authored on, which keeps the ankle's
		// height above the sole and any lift from the walk cycle.
		const f32 authored_ground_z = transform_point(in_rig_world, HMM_V3(animated_foot.X, animated_foot.Y, in_rig_sole_z)).Z;
		out_ground_offset = foot_ground_z - authored_ground_z;
		if (state.character.show_ik)
		{
			DebugDraw::point(HMM_V3(animated_foot_world.X, animated_foot_world.Y, authored_ground_z), DebugDraw::magenta);
		}
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

		const Subtree subtree = collect_subtree(io_rig, in_chain.hip);
		std::vector<HMM_Mat4> pose = rest_pose_under_parent(io_rig, subtree);

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

		const f32 full_reach = thigh_length + shin_length;
		const f32 min_reach = fabsf(thigh_length - shin_length) + epsilon * full_reach;
		// Debug-only: state.character.leg_ik_max_extension_scale lets a developer
		// shrink how far the thigh/shin pair is allowed to straighten, to see the
		// solver visibly react (the "Leg Max Extension" slider under Character).
		const f32 max_reach = std::max(min_reach, full_reach * state.character.leg_ik_max_extension_scale);

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

		// Unlike arm IK, the reach comes from the hock target's distance, not a chosen bend angle.
		// A straight rest leg bends the knee forward.
		const HMM_Vec3 aim = HMM_NormV3(hock_target - hip);
		const f32 target_reach = HMM_Clamp(min_reach, HMM_LenV3(hock_target - hip), max_reach);
		const HMM_Vec3 pole = place_two_bone(io_rig, subtree, in_chain.hip, in_chain.knee, in_chain.hock,
			aim, target_reach, bend_angle(hip, knee, hock), pole_hint, pose);

		// ---- Aim the hock the rest of the way at the actual ground target ----

		const HMM_Vec3 placed_hock = pose[in_chain.hock].Columns[3].XYZ;
		const HMM_Vec3 placed_foot = pose[in_chain.foot].Columns[3].XYZ;
		transform_below(io_rig, subtree, in_chain.hock,
			rotate_about(swing(placed_foot - placed_hock, target - placed_hock), placed_hock), pose);

		// Place the foot flat on the ground at its solved position: keep the
		// animated heading, but tilt it so its rest-pose up (the rig's +Z, where
		// the sole was modeled flat) matches the ground normal.
		const HMM_Vec3 animated_foot_position = animated_foot.Columns[3].XYZ;
		const HMM_Vec3 foot_rest_up = direction(io_rig.bones[in_chain.foot].inverse_bind_matrix, HMM_V3(0, 0, 1));
		const HMM_Mat4 flatten = rotate_about(swing(direction(animated_foot, foot_rest_up), ground_normal), animated_foot_position);
		const HMM_Vec3 solved_foot = pose[in_chain.foot].Columns[3].XYZ;
		transform_below(io_rig, subtree, in_chain.foot,
			HMM_Translate(solved_foot - animated_foot_position) * flatten * animated_foot * HMM_InvGeneralM4(pose[in_chain.foot]), pose);

		// animated_foot refers into io_rig.evaluated_pose; don't use it past this commit.
		if (!commit_if_finite(io_rig, subtree, pose, out_error))
		{
			return false;
		}
		if (state.character.show_ik)
		{
			auto world = [&](i32 in_bone) { return transform_point(in_rig_world, io_rig.evaluated_pose[in_bone].Columns[3].XYZ); };
			DebugDraw::chain({world(in_chain.hip), world(in_chain.knee), world(in_chain.hock), world(in_chain.foot)}, DebugDraw::white);
			DebugDraw::point(transform_point(in_rig_world, hock_target), DebugDraw::blue);
			DebugDraw::line(world(in_chain.knee), world(in_chain.knee) + HMM_NormV3(direction(in_rig_world, pole)) * 0.4f, DebugDraw::magenta);
		}
		return true;
	}

	// ---- Mech integration ----

	void update(MechInstance& io_mech, std::string& io_diagnostics, f32 in_delta_time)
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

		// The legs mesh's lowest rest-pose point is the sole plane the animation was authored on.
		// shortcut: assumes armature_to_mesh has no tilt; take the min over transformed corners if a rig needs it.
		const f32 rig_sole_z = transform_point(HMM_InvGeneralM4(legs->second.mesh.armature_to_mesh), legs->second.mesh.bounding_box.min).Z;
		if (rig->second.armature.evaluated_pose.size() != rig->second.armature.bone_count || !std::isfinite(rig_sole_z))
		{
			report(0, "pose is unavailable");
			report(1, "pose is unavailable");
			return;
		}
		// Confirmed project convention: Blender/game forward is +Y, up is +Z.
		const HMM_Vec3 legs_forward_world = quat_forward(legs->second.current_transform.rotation);

		// Frame-rate independent exponential approach toward each frame's desired values.
		const f32 position_blend = 1.0f - expf(-state.character.leg_ik_position_lerp_rate * in_delta_time);
		const f32 rotation_blend = 1.0f - expf(-state.character.leg_ik_rotation_lerp_rate * in_delta_time);

		const Chain chains[2] = {left_chain, right_chain};
		for (i32 side = 0; side < 2; ++side)
		{
			const Armature& armature = rig->second.armature;
			const HMM_Mat4& animated_foot = armature.evaluated_pose[chains[side].foot];
			const HMM_Vec3 animated_foot_world = transform_point(rig_world, animated_foot.Columns[3].XYZ);
			// The foot's own up as animated: the rest-pose rig +Z carried through the animated foot.
			const HMM_Vec3 foot_rest_up = direction(armature.bones[chains[side].foot].inverse_bind_matrix, HMM_V3(0, 0, 1));
			const HMM_Vec3 animated_up_world = HMM_NormV3(direction(rig_world * animated_foot, foot_rest_up));
			if (!is_finite(animated_foot_world) || !is_finite(animated_up_world))
			{
				report(side, "degenerate rig transform");
				continue;
			}

			// With no ground, ease back to the plain animation rather than snapping to it.
			f32 desired_offset = 0.0f;
			HMM_Vec3 desired_normal = animated_up_world;
			std::string ground_error;
			const bool has_ground = find_ground_target(armature, chains[side], rig_world, rig_sole_z, desired_offset, desired_normal, ground_error);
			if (!has_ground)
			{
				report(side, ground_error);
				desired_offset = 0.0f;
				desired_normal = animated_up_world;
			}

			MechLegIKBlend& blend = io_mech.leg_ik_blends[side];
			if (!blend.valid)
			{
				blend = {desired_offset, desired_normal, true};
			}
			else
			{
				blend.ground_offset += (desired_offset - blend.ground_offset) * position_blend;
				const HMM_Vec3 normal = HMM_LerpV3(blend.ground_normal, rotation_blend, desired_normal);
				blend.ground_normal = HMM_LenSqrV3(normal) > epsilon ? HMM_NormV3(normal) : desired_normal;
			}

			// Fully eased out: leave the sampled pose exactly as animated.
			if (!has_ground && fabsf(blend.ground_offset) < 1e-3f && HMM_DotV3(blend.ground_normal, animated_up_world) > 0.9999f)
			{
				continue;
			}

			const HMM_Vec3 target_world = animated_foot_world + HMM_V3(0, 0, blend.ground_offset);
			if (state.character.show_ik)
			{
				DebugDraw::point(target_world, DebugDraw::yellow, 0.12f);
			}
			std::string solve_error;
			if (!solve(rig->second.armature, chains[side], rig_world, target_world, blend.ground_normal, legs_forward_world, solve_error))
			{
				report(side, solve_error);
			}
		}
	}
}
