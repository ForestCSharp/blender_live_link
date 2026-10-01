#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "state/state.h"
#include "game_object/attachment_point.h"

// Procedural aim for the active player's arms. Runs after base poses are
// sampled and before weapons attach, so sockets and skinning see the result.
namespace ArmIK
{
	constexpr f32 epsilon = 1e-6f;
	// Minimum elbow bend while aiming, in degrees away from straight. Rest poses
	// bent further keep their authored bend.
	constexpr f32 min_elbow_bend_degrees = 50.0f;
	// Rest bends below this are treated as straight modeling noise.
	constexpr f32 authored_bend_degrees = 2.0f;

	struct Chain
	{
		i32 shoulder = -1;
		i32 elbow = -1;
		i32 hand = -1;

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

		// Shoulder subtree as (depth, bone), sorted so parents precede children.
		std::vector<std::pair<i32, i32>> subtree;
		for (u32 bone_idx = 0; bone_idx < io_rig.bone_count; ++bone_idx)
		{
			const i32 depth = depth_below(io_rig, (i32) bone_idx, in_chain.shoulder);
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

		// Point the rest arm along the view first; this sets the baseline twist.
		const HMM_Mat4 arm_rotation = rotate_about(swing(hand - shoulder, aim), shoulder);
		for (const auto& [depth, bone] : subtree)
		{
			pose[bone] = arm_rotation * pose[bone];
		}

		// Bend the elbow to at least min_elbow_bend_degrees, then place the elbow
		// for the resulting reach (law of cosines). An authored rest bend keeps
		// its direction; a straight rest arm drops its elbow toward world down.
		const f32 rest_bend = acosf(HMM_Clamp(-1.0f, HMM_DotV3(elbow - shoulder, hand - elbow) / (upper_length * lower_length), 1.0f));
		const f32 bend_angle = std::max(rest_bend, HMM_AngleDeg(min_elbow_bend_degrees));
		const f32 min_reach = fabsf(upper_length - lower_length) + epsilon * (upper_length + lower_length);
		const f32 target_reach = HMM_Clamp(min_reach,
			sqrtf(upper_length * upper_length + lower_length * lower_length + 2.0f * upper_length * lower_length * cosf(bend_angle)),
			upper_length + lower_length);
		const f32 along = HMM_Clamp(-upper_length,
			(upper_length * upper_length - lower_length * lower_length + target_reach * target_reach) / (2.0f * target_reach),
			upper_length);
		const f32 bend = sqrtf(std::max(0.0f, upper_length * upper_length - along * along));

		const HMM_Vec3 aimed_elbow = pose[in_chain.elbow].Columns[3].XYZ;
		HMM_Vec3 pole = (aimed_elbow - shoulder) - aim * HMM_DotV3(aimed_elbow - shoulder, aim);
		if (rest_bend < HMM_AngleDeg(authored_bend_degrees) || HMM_LenSqrV3(pole) < epsilon * upper_length * upper_length)
		{
			const HMM_Vec3 down = direction(world_to_rig, HMM_V3(0, 0, -1));
			pole = down - aim * HMM_DotV3(down, aim);
			if (HMM_LenSqrV3(pole) < epsilon * HMM_LenSqrV3(down))
			{
				pole = any_perpendicular(aim);
			}
		}
		pole = HMM_NormV3(pole);
		const HMM_Vec3 bent_elbow = shoulder + aim * along + pole * bend;
		const HMM_Vec3 bent_hand = shoulder + aim * target_reach;

		// Swing the upper arm onto the new elbow, then the forearm onto the hand.
		const HMM_Mat4 upper_rotation = rotate_about(swing(aimed_elbow - shoulder, bent_elbow - shoulder), shoulder);
		for (const auto& [depth, bone] : subtree)
		{
			pose[bone] = upper_rotation * pose[bone];
		}
		const HMM_Vec3 swung_hand = pose[in_chain.hand].Columns[3].XYZ;
		const HMM_Mat4 lower_rotation = rotate_about(swing(swung_hand - bent_elbow, bent_hand - bent_elbow), bent_elbow);
		for (const auto& [depth, bone] : subtree)
		{
			if (depth_below(io_rig, bone, in_chain.elbow) >= 0)
			{
				pose[bone] = lower_rotation * pose[bone];
			}
		}

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
		const HMM_Mat4 wrist_delta = world_to_rig * aimed_hand_world * HMM_InvGeneralM4(pose[in_chain.hand]);
		for (const auto& [depth, bone] : subtree)
		{
			if (depth_below(io_rig, bone, in_chain.hand) >= 0)
			{
				pose[bone] = wrist_delta * pose[bone];
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
			if (!is_player || arm_pose.mode == ArmPoseMode::Animation)
			{
				continue;
			}

			auto arm = state.scene.objects.find(io_mech.part_instance_uids[part_idx]);
			auto arm_template = state.scene.objects.find(io_mech.part_template_uids[part_idx]);
			if (arm == state.scene.objects.end() || arm_template == state.scene.objects.end() || !arm->second.visibility)
			{
				continue;
			}
			if (camera == state.scene.objects.end() || !camera->second.has_camera_control)
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
			if (!solve(target.rig_object->armature, target.chain, target.rig_world,
				camera->second.camera_control.camera.forward, target.aim_axis, error))
			{
				report(target.slot, error);
			}
		}
	}
}
