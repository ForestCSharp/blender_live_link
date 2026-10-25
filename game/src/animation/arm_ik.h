#pragma once

#include <string>

#include "animation/ik_common.h"
#include "game_object/attachment_point.h"
#include "render/debug_draw.h"

// Procedural aim for mech arms (player: camera forward, others: aim_direction). Runs after base poses are
// sampled and before weapons attach, so sockets and skinning see the result.
namespace ArmIK
{
	using namespace IK;

	// Minimum elbow bend while aiming, in degrees away from straight. Rest poses
	// bent further keep their authored bend.
	constexpr f32 min_elbow_bend_degrees = 50.0f;

	struct Chain
	{
		i32 shoulder = -1;
		i32 elbow = -1;
		i32 hand = -1;

		bool operator==(const Chain&) const = default;
	};

	// Explicit names win. An empty hand falls back to the rig's single
	// attachment-labeled bone; an empty elbow or shoulder uses the parent.
	bool resolve(const Armature& in_rig, const Part& in_part, Chain& out_chain, std::string& out_error)
	{
		out_chain = {};
		if (!in_part.ik_hand_bone.empty())
		{
			out_chain.hand = named_bone(in_rig, in_part.ik_hand_bone);
		}
		else
		{
			for (u32 bone_idx = 0; bone_idx < in_rig.bone_count; ++bone_idx)
			{
				const char* label = in_rig.bones[bone_idx].attachment_label;
				if (!label || !label[0])
				{
					continue;
				}
				if (out_chain.hand >= 0)
				{
					out_error = "multiple labeled hands; select Hand Bone";
					return false;
				}
				out_chain.hand = (i32) bone_idx;
			}
		}
		if (out_chain.hand < 0)
		{
			out_error = "hand bone is missing";
			return false;
		}

		out_chain.elbow = in_part.ik_elbow_bone.empty()
			? in_rig.bones[out_chain.hand].parent_index
			: named_bone(in_rig, in_part.ik_elbow_bone);
		if (out_chain.elbow < 0 || out_chain.elbow >= (i32) in_rig.bone_count)
		{
			out_error = "elbow bone is missing";
			return false;
		}

		out_chain.shoulder = in_part.ik_shoulder_bone.empty()
			? in_rig.bones[out_chain.elbow].parent_index
			: named_bone(in_rig, in_part.ik_shoulder_bone);
		if (out_chain.shoulder < 0 || out_chain.shoulder >= (i32) in_rig.bone_count)
		{
			out_error = "shoulder bone is missing";
			return false;
		}

		if (in_rig.bones[out_chain.hand].parent_index != out_chain.elbow ||
			in_rig.bones[out_chain.elbow].parent_index != out_chain.shoulder)
		{
			out_error = "expected direct shoulder -> elbow -> hand hierarchy";
			return false;
		}
		// Rejects parent cycles, which also covers repeated chain bones.
		if (!has_rooted_ancestry(in_rig, out_chain.hand))
		{
			out_error = "invalid bone hierarchy";
			return false;
		}
		return true;
	}

	// ---- Solver ----

	// Resets the shoulder subtree (including fingers) to rest locals under the
	// sampled parent, rotates it about the shoulder so the rest shoulder->hand
	// line follows the view, then bends the elbow by at least
	// min_elbow_bend_degrees. The wrist then turns so in_muzzle_axis
	// (hand-local, scaled by the weapon) points along in_forward.
	//
	// Transactional: a failed solve leaves the sampled pose untouched.
	bool solve(Armature& io_rig, const Chain& in_chain, const HMM_Mat4& in_rig_world,
		HMM_Vec3 in_forward, HMM_Vec3 in_muzzle_axis, std::string& out_error)
	{
		if (io_rig.evaluated_pose.size() != io_rig.bone_count)
		{
			out_error = "pose is unavailable";
			return false;
		}
		if (depth_below(io_rig, in_chain.hand, in_chain.elbow) != 1 ||
			depth_below(io_rig, in_chain.elbow, in_chain.shoulder) != 1 ||
			!has_rooted_ancestry(io_rig, in_chain.shoulder))
		{
			out_error = "invalid bone hierarchy";
			return false;
		}

		const HMM_Mat4 world_to_rig = HMM_InvGeneralM4(in_rig_world);
		HMM_Vec3 aim = direction(world_to_rig, in_forward);
		if (!is_finite(world_to_rig) || !is_finite(aim) || !is_finite(in_muzzle_axis) ||
			HMM_LenSqrV3(aim) < epsilon || HMM_LenSqrV3(in_muzzle_axis) < epsilon)
		{
			out_error = "degenerate aim or rig transform";
			return false;
		}
		aim = HMM_NormV3(aim);

		const Subtree subtree = collect_subtree(io_rig, in_chain.shoulder);
		std::vector<HMM_Mat4> pose = rest_pose_under_parent(io_rig, subtree);

		const HMM_Vec3 shoulder = pose[in_chain.shoulder].Columns[3].XYZ;
		const HMM_Vec3 elbow = pose[in_chain.elbow].Columns[3].XYZ;
		const HMM_Vec3 hand = pose[in_chain.hand].Columns[3].XYZ;
		const f32 upper_length = HMM_LenV3(elbow - shoulder);
		const f32 lower_length = HMM_LenV3(hand - elbow);
		const f32 reach = HMM_LenV3(hand - shoulder);
		if (!std::isfinite(upper_length + lower_length + reach) ||
			upper_length < epsilon || lower_length < epsilon || reach < epsilon)
		{
			out_error = "zero-length segment or shoulder-to-hand reach";
			return false;
		}

		// Shoulder-to-hand distance for the elbow bend (law of cosines), kept within what the arm can reach.
		const f32 rest_bend = bend_angle(shoulder, elbow, hand);
		const f32 elbow_bend = std::max(rest_bend, HMM_AngleDeg(min_elbow_bend_degrees));
		// u² + l² is the reach at a 90° elbow; 2ul·cos θ adjusts it for any other bend (0 = straight).
		const f32 bent_reach = sqrtf(upper_length * upper_length + lower_length * lower_length +
			2.0f * upper_length * lower_length * cosf(elbow_bend));
		const f32 max_reach = upper_length + lower_length;
		const f32 min_reach = fabsf(upper_length - lower_length) + epsilon * max_reach;
		const f32 target_reach = HMM_Clamp(min_reach, bent_reach, max_reach);

		// A straight rest arm bends toward world down.
		const HMM_Vec3 pole = place_two_bone(io_rig, subtree, in_chain.shoulder, in_chain.elbow, in_chain.hand,
			aim, target_reach, rest_bend, direction(world_to_rig, HMM_V3(0, 0, -1)), pose);

		// Weapon roots take the attachment's world rotation and their own
		// authored scale, so align the scaled muzzle axis rather than bone +Y.
		const HMM_Mat4 hand_world = in_rig_world * pose[in_chain.hand];
		Transform wrist;
		if (!transform_from_matrix_location_rotation(hand_world, wrist))
		{
			out_error = "singular wrist transform";
			return false;
		}
		const HMM_Mat4 wrist_rotation = HMM_QToM4(wrist.rotation);
		HMM_Mat4 aimed_hand_world = swing(direction(wrist_rotation, in_muzzle_axis), in_forward) * wrist_rotation;
		for (i32 axis = 0; axis < 3; ++axis)
		{
			aimed_hand_world.Columns[axis].XYZ *= HMM_LenV3(hand_world.Columns[axis].XYZ);
		}
		aimed_hand_world.Columns[3] = hand_world.Columns[3];

		// Carry the wrist correction down to the hand and its children.
		transform_below(io_rig, subtree, in_chain.hand, world_to_rig * aimed_hand_world * HMM_InvGeneralM4(pose[in_chain.hand]), pose);

		if (!commit_if_finite(io_rig, subtree, pose, out_error))
		{
			return false;
		}
		if (state.character.show_ik)
		{
			auto world = [&](i32 in_bone) { return transform_point(in_rig_world, io_rig.evaluated_pose[in_bone].Columns[3].XYZ); };
			const HMM_Vec3 hand_at = world(in_chain.hand);
			DebugDraw::chain({world(in_chain.shoulder), world(in_chain.elbow), hand_at}, DebugDraw::white);
			DebugDraw::line(world(in_chain.shoulder), world(in_chain.shoulder) + HMM_NormV3(in_forward) * 3.0f, DebugDraw::yellow);
			DebugDraw::line(world(in_chain.elbow), world(in_chain.elbow) + HMM_NormV3(direction(in_rig_world, pole)) * 0.4f, DebugDraw::magenta);
			DebugDraw::line(hand_at, hand_at + HMM_NormV3(direction(aimed_hand_world, in_muzzle_axis)) * 1.0f, DebugDraw::cyan);
		}
		return true;
	}

	// ---- Mech integration ----

	// Hand-local axis that should point along the view: the equipped weapon's
	// scaled muzzle +Y when one is attached to this hand, otherwise bone +Y.
	HMM_Vec3 hand_aim_axis(const MechInstance& in_mech, const Object& in_rig_object, i32 in_hand)
	{
		const char* hand_name = in_rig_object.armature.bones[in_hand].name;
		for (const MechWeaponInstance& equipped : in_mech.weapons)
		{
			if (!hand_name || equipped.armature_template_uid != in_rig_object.template_object_id ||
				equipped.bone_name != hand_name)
			{
				continue;
			}
			auto weapon = state.scene.objects.find(equipped.weapon_template_uid);
			if (weapon != state.scene.objects.end() && weapon->second.weapon.muzzle_valid)
			{
				return HMM_MulV3(
					weapon->second.weapon.muzzle_local_transform.Columns[1].XYZ,
					weapon->second.initial_transform.scale
				);
			}
		}
		return HMM_V3(0, 1, 0);
	}

	void update(MechInstance& io_mech, std::string& io_diagnostics)
	{
		struct Target
		{
			Object* rig_object = nullptr;
			Chain chain;
			HMM_Mat4 rig_world;
			HMM_Vec3 aim_axis;
			i32 slot = -1;
		};
		Target targets[2];
		i32 target_count = 0;

		auto report = [&](i32 in_slot, const std::string& in_error) {
			io_diagnostics += std::string(in_slot == 0 ? " error[Left Arm IK]=" : " error[Right Arm IK]=") + in_error;
		};

		const bool is_player = state.scene.player_character_id &&
			*state.scene.player_character_id == io_mech.character_uid;
		auto camera = state.scene.camera_control_id
			? state.scene.objects.find(*state.scene.camera_control_id)
			: state.scene.objects.end();

		for (i32 slot = 0; slot < 2; ++slot)
		{
			const i32 part_idx = (i32) (slot == 0 ? PartType::LeftArm : PartType::RightArm);

			// Replacing the arm template drops any runtime pose override.
			MechArmPose& arm_pose = io_mech.arm_poses[slot];
			if (arm_pose.template_uid != io_mech.part_template_uids[part_idx])
			{
				arm_pose.template_uid = io_mech.part_template_uids[part_idx];
				arm_pose.mode = ArmPoseMode::AimIK;
			}
			const bool has_ai_aim = HMM_LenSqrV3(io_mech.aim_direction) > epsilon;
			if ((!is_player && !has_ai_aim) || arm_pose.mode == ArmPoseMode::Animation)
			{
				continue;
			}

			auto arm = state.scene.objects.find(io_mech.part_instance_uids[part_idx]);
			auto arm_template = state.scene.objects.find(io_mech.part_template_uids[part_idx]);
			if (arm == state.scene.objects.end() || arm_template == state.scene.objects.end() || !arm->second.visibility)
			{
				continue;
			}
			if (is_player && (camera == state.scene.objects.end() || !camera->second.has_camera_control))
			{
				report(slot, "player camera is missing");
				continue;
			}
			auto rig = arm->second.has_mesh
				? state.scene.objects.find(arm->second.mesh.armature_id)
				: state.scene.objects.end();
			if (rig == state.scene.objects.end() || !rig->second.has_armature)
			{
				report(slot, "arm rig is missing");
				continue;
			}

			Chain chain;
			std::string error;
			if (!resolve(rig->second.armature, arm_template->second.part, chain, error))
			{
				report(slot, error);
				continue;
			}
			targets[target_count++] = {
				.rig_object = &rig->second,
				.chain = chain,
				.rig_world = object_get_model_matrix(arm->second) * arm->second.mesh.armature_to_mesh,
				.aim_axis = hand_aim_axis(io_mech, rig->second, chain.hand),
				.slot = slot,
			};
		}

		// Both arms may skin to one rig. An identical chain with identical inputs
		// solves once; overlapping subtrees would fight, so neither solves.
		bool conflict = false;
		if (target_count == 2 && targets[0].rig_object == targets[1].rig_object)
		{
			const Target& a = targets[0];
			const Target& b = targets[1];
			const Armature& rig = a.rig_object->armature;
			const bool overlap = depth_below(rig, a.chain.shoulder, b.chain.shoulder) >= 0 ||
				depth_below(rig, b.chain.shoulder, a.chain.shoulder) >= 0;
			if (overlap)
			{
				const bool identical = a.chain == b.chain &&
					HMM_LenSqrV3(a.aim_axis - b.aim_axis) < epsilon &&
					nearly_equal(a.rig_world, b.rig_world);
				if (identical)
				{
					target_count = 1;
				}
				else
				{
					conflict = true;
				}
			}
		}

		for (i32 target_idx = 0; target_idx < target_count; ++target_idx)
		{
			const Target& target = targets[target_idx];
			if (conflict)
			{
				report(target.slot, "overlapping or incompatible shared-rig chains");
				continue;
			}
			std::string error;
			const HMM_Vec3 aim_forward = is_player ? camera->second.camera_control.camera.forward : io_mech.aim_direction;
			if (!solve(target.rig_object->armature, target.chain, target.rig_world,
				aim_forward, target.aim_axis, error))
			{
				report(target.slot, error);
			}
		}
	}
}
