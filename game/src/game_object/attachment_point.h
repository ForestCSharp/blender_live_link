#pragma once

#include <cstring>
#include <string>

#include "state/state.h"

// Implemented by mech.h. Attachment evaluation only needs the resolved
// per-mech armature mapping, while ownership and cloning stay in that module.
i32 mech_find_armature_instance(const MechInstance& in_mech, i32 in_template_uid);

const char* part_type_name(PartType in_type)
{
	switch (in_type)
	{
		case PartType::Body: return "Body";
		case PartType::Legs: return "Legs";
		case PartType::LeftArm: return "Left Arm";
		case PartType::RightArm: return "Right Arm";
		case PartType::Head: return "Head";
		default: return "Invalid";
	}
}

bool transform_from_matrix_location_rotation(const HMM_Mat4& in_matrix, Transform& out_transform)
{
	out_transform.location = HMM_V4(
		in_matrix.Elements[3][0],
		in_matrix.Elements[3][1],
		in_matrix.Elements[3][2],
		1.0f
	);

	HMM_Mat4 rotation_matrix = HMM_M4D(1.0f);
	for (i32 column = 0; column < 3; ++column)
	{
		HMM_Vec3 axis = HMM_V3(
			in_matrix.Elements[column][0],
			in_matrix.Elements[column][1],
			in_matrix.Elements[column][2]
		);
		if (HMM_LenSqrV3(axis) <= 0.0000001f)
		{
			return false;
		}
		axis = HMM_NormV3(axis);
		rotation_matrix.Elements[column][0] = axis.X;
		rotation_matrix.Elements[column][1] = axis.Y;
		rotation_matrix.Elements[column][2] = axis.Z;
	}
	out_transform.rotation = HMM_NormQ(HMM_M4ToQ_RH(rotation_matrix));
	return true;
}

bool attachment_point_world_matrix(
	const MechInstance& in_mech,
	const Object& in_body,
	const AttachmentPoint& in_attachment,
	HMM_Mat4& out_world_matrix,
	std::string& out_error)
{
	if (!in_attachment.valid)
	{
		out_error = "socket was invalid when exported";
		return false;
	}

	const HMM_Mat4 body_model = object_get_model_matrix(in_body);
	if (in_attachment.binding_type == AttachmentBindingType::Object)
	{
		out_world_matrix = HMM_MulM4(body_model, in_attachment.local_transform);
		return true;
	}

	if (!in_body.has_mesh || !in_body.mesh.has_skinned_vertices)
	{
		out_error = "Body is not a skinned mesh";
		return false;
	}
	const i32 armature_instance_uid = mech_find_armature_instance(in_mech, in_attachment.armature_id);
	if (armature_instance_uid == -1 || in_body.mesh.armature_id != armature_instance_uid)
	{
		out_error = "socket armature does not match the Body mesh armature";
		return false;
	}

	auto armature_found = state.scene.objects.find(armature_instance_uid);
	if (armature_found == state.scene.objects.end() || !armature_found->second.has_armature)
	{
		out_error = "socket armature is missing";
		return false;
	}

	Armature& armature = armature_found->second.armature;
	i32 bone_idx = -1;
	for (i32 candidate_idx = 0; candidate_idx < (i32) armature.bone_count; ++candidate_idx)
	{
		const char* candidate_name = armature.bones[candidate_idx].name;
		if (candidate_name && in_attachment.bone_name &&
			strcmp(candidate_name, in_attachment.bone_name) == 0)
		{
			bone_idx = candidate_idx;
			break;
		}
	}
	if (bone_idx < 0)
	{
		out_error = "socket bone is missing";
		return false;
	}

	const HMM_Mat4 bind_matrix = HMM_InvGeneralM4(armature.bones[bone_idx].inverse_bind_matrix);
	HMM_Mat4 pose_matrix = bind_matrix;
	AnimationClip* animation = armature_get_active_animation(armature);
	if (animation && animation->skin_matrices && animation->frame_count > 0 &&
		bone_idx < animation->bone_count)
	{
		const i32 frame_idx = CLAMP(armature.current_frame, 0, animation->frame_count - 1);
		const HMM_Mat4& skin_matrix = animation->skin_matrices[
			frame_idx * animation->bone_count + bone_idx
		];
		pose_matrix = HMM_MulM4(skin_matrix, bind_matrix);
	}

	out_world_matrix = HMM_MulM4(
		body_model,
		HMM_MulM4(
			in_body.mesh.armature_to_mesh,
			HMM_MulM4(pose_matrix, in_attachment.local_transform)
		)
	);
	return true;
}
