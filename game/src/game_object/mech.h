#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <random>

#include "state/state.h"
#include "game_object/attachment_point.h"
#include "animation/arm_ik.h"

void update_mech_transforms();

char* mech_instance_name(const char* in_template_name, i32 in_mech_id)
{
	const char* base_name = in_template_name ? in_template_name : "Mech Object";
	const i32 required = snprintf(nullptr, 0, "%s [Mech %i]", base_name, in_mech_id) + 1;
	char* result = (char*) malloc(required);
	snprintf(result, required, "%s [Mech %i]", base_name, in_mech_id);
	return result;
}

i32 mech_allocate_runtime_object_uid()
{
	while (state.scene.objects.contains(state.mech.next_runtime_object_uid))
	{
		--state.mech.next_runtime_object_uid;
	}
	return state.mech.next_runtime_object_uid--;
}

i32 mech_find_armature_instance(const MechInstance& in_mech, i32 in_template_uid)
{
	for (const MechArmatureInstance& mapping : in_mech.armature_instances)
	{
		if (mapping.template_uid == in_template_uid)
		{
			return mapping.instance_uid;
		}
	}
	return -1;
}

Object mech_make_runtime_instance_base(
	const Object& in_template,
	const MechInstance& in_mech,
	ObjectStorageKind in_storage_kind)
{
	const i32 instance_uid = mech_allocate_runtime_object_uid();
	Object instance = object_create(
		instance_uid,
		mech_instance_name(in_template.name, in_mech.runtime_id),
		false,
		in_template.initial_transform.location,
		in_template.initial_transform.rotation,
		in_template.initial_transform.scale
	);
	instance.storage_kind = in_storage_kind;
	instance.template_object_id = in_template.unique_id;
	instance.mech_instance_id = in_mech.runtime_id;
	return instance;
}

i32 mech_clone_armature(MechInstance& in_mech, i32 in_template_uid)
{
	const i32 existing_uid = mech_find_armature_instance(in_mech, in_template_uid);
	if (in_template_uid < 0)
	{
		return -1;
	}
	if (existing_uid != -1)
	{
		return existing_uid;
	}

	auto template_found = state.scene.objects.find(in_template_uid);
	if (template_found == state.scene.objects.end() || !template_found->second.has_armature ||
		object_is_runtime_instance(template_found->second))
	{
		return -1;
	}

	Object& armature_template = template_found->second;
	Object armature_instance = mech_make_runtime_instance_base(
		armature_template,
		in_mech,
		ObjectStorageKind::RuntimeArmature
	);
	const i32 instance_uid = armature_instance.unique_id;
	armature_instance.has_armature = true;
	armature_instance.armature = armature_template.armature;
	armature_instance.armature.playback_time = 0.0f;
	armature_instance.armature.current_frame = 0;
	armature_instance.armature.evaluated_pose.clear();

	scene_insert_or_replace_object(state, std::move(armature_instance));
	in_mech.armature_instances.add({
		.template_uid = in_template_uid,
		.instance_uid = instance_uid,
	});
	return instance_uid;
}

i32 mech_clone_part(MechInstance& in_mech, const Object& in_template)
{
	Object instance = mech_make_runtime_instance_base(
		in_template,
		in_mech,
		ObjectStorageKind::RuntimePart
	);
	const i32 instance_uid = instance.unique_id;
	instance.has_mesh = in_template.has_mesh;
	instance.has_light = in_template.has_light;
	instance.light = in_template.light;

	if (in_template.has_mesh)
	{
		// Initialize immutable buffers on the template before sharing their
		// wrappers. Lazy initialization after copying would create independent
		// allocations and make ownership ambiguous.
		Mesh& template_mesh = const_cast<Mesh&>(in_template.mesh);
		if (template_mesh.index_count > 0) template_mesh.index_buffer.get_gpu_buffer();
		if (template_mesh.wire_index_count > 0) template_mesh.wire_index_buffer.get_gpu_buffer();
		if (template_mesh.vertex_count > 0) template_mesh.vertex_buffer.get_gpu_buffer();
		if (template_mesh.has_skinned_vertices)
		{
			template_mesh.skinned_vertex_buffer.get_gpu_buffer();
		}

		instance.mesh = template_mesh;
		// The copy aliases the template's GpuBuffers, but must not inherit its
		// arena slice: object_cleanup skips RuntimePart, so a shared slice would
		// either double-free or leak. Runtime parts stay on the legacy path.
		instance.mesh.arena_slice = {};
		instance.mesh.skin_matrix_arena_offset = -1;
		instance.mesh.skinned_vertex_cache_buffer = {};
		instance.mesh.skinned_vertex_cache_capacity = 0;
		instance.mesh.skinned_vertex_cache_valid = false;
		instance.mesh.tessellated_geometry = {};
		instance.mesh.skin_matrices = nullptr;
		if (instance.mesh.has_skinned_vertices && instance.mesh.skin_matrix_count > 0)
		{
			instance.mesh.skin_matrices = (HMM_Mat4*) calloc(
				instance.mesh.skin_matrix_count,
				sizeof(HMM_Mat4)
			);
			mesh_reset_skin_matrices(instance.mesh);
			instance.mesh.armature_id = mech_clone_armature(in_mech, template_mesh.armature_id);
		}
	}

	scene_insert_or_replace_object(state, std::move(instance));
	return instance_uid;
}

void mech_destroy_runtime_objects(MechInstance& in_mech)
{
	for (auto& weapon : in_mech.weapons)
	{
		if (weapon.instance_uid != -1)
		{
			scene_remove_object(state, weapon.instance_uid);
		}
		weapon.instance_uid = -1;
	}
	for (i32 part_idx = 0; part_idx < (i32) PartType::Count; ++part_idx)
	{
		const i32 instance_uid = in_mech.part_instance_uids[part_idx];
		auto found = state.scene.objects.find(instance_uid);
		if (found != state.scene.objects.end() && object_is_runtime_instance(found->second))
		{
			scene_remove_object(state, instance_uid);
		}
		in_mech.part_instance_uids[part_idx] = -1;
	}
	for (const MechArmatureInstance& mapping : in_mech.armature_instances)
	{
		auto found = state.scene.objects.find(mapping.instance_uid);
		if (found != state.scene.objects.end() && object_is_runtime_instance(found->second))
		{
			scene_remove_object(state, mapping.instance_uid);
		}
	}
	in_mech.armature_instances.clear();
}

void mech_suspend_runtime_objects()
{
	for (auto& [mech_id, mech] : state.mech.instances)
	{
		mech_destroy_runtime_objects(mech);
	}

	// Defensive cleanup for a partially-created instance that was not yet
	// recorded in a descriptor.
	DynamicArray<i32> orphan_uids;
	for (auto& [object_uid, object] : state.scene.objects)
	{
		if (object.storage_kind == ObjectStorageKind::RuntimePart ||
			object.storage_kind == ObjectStorageKind::RuntimeArmature)
		{
			orphan_uids.add(object_uid);
		}
	}
	for (i32 orphan_uid : orphan_uids)
	{
		scene_remove_object(state, orphan_uid);
	}
	orphan_uids.reset();
}

void mech_resolve_templates(MechInstance& in_mech)
{
	scene_ensure_indexes(state);
	for (i32 part_idx = 0; part_idx < (i32) PartType::Count; ++part_idx)
	{
		in_mech.part_template_uids[part_idx] = -1;
		in_mech.socket_template_uids[part_idx] = -1;
		const MechLoadoutSlot& slot = in_mech.loadout.slots[part_idx];
		if (slot.selection == MechLoadoutSelectionType::TemplateUid)
		{
			auto explicit_found = state.scene.objects.find(slot.template_uid);
			if (explicit_found != state.scene.objects.end() && explicit_found->second.has_part &&
				!object_is_runtime_instance(explicit_found->second) &&
				(i32) explicit_found->second.part.type == part_idx)
			{
				in_mech.part_template_uids[part_idx] = slot.template_uid;
			}
			continue;
		}

		for (i32 candidate_uid : state.scene.indexes.part_object_ids)
		{
			auto candidate_found = state.scene.objects.find(candidate_uid);
			if (candidate_found != state.scene.objects.end() &&
				(i32) candidate_found->second.part.type == part_idx &&
				(in_mech.part_template_uids[part_idx] < 0 || candidate_uid < in_mech.part_template_uids[part_idx]))
			{
				in_mech.part_template_uids[part_idx] = candidate_uid;
			}
		}
	}

	const i32 body_template_uid = in_mech.part_template_uids[(i32) PartType::Body];
	for (i32 socket_uid : state.scene.indexes.attachment_point_object_ids)
	{
		auto socket_found = state.scene.objects.find(socket_uid);
		if (socket_found == state.scene.objects.end()) continue;
		const AttachmentPoint& socket = socket_found->second.attachment_point;
		const i32 part_idx = (i32) socket.part_type;
		if (socket.owner_part_id == body_template_uid && part_idx > (i32) PartType::Body &&
			part_idx < (i32) PartType::Count &&
			(in_mech.socket_template_uids[part_idx] < 0 || socket_uid < in_mech.socket_template_uids[part_idx]))
		{
			in_mech.socket_template_uids[part_idx] = socket_uid;
		}
	}
}

void mech_build_runtime_objects(MechInstance& in_mech)
{
	mech_resolve_templates(in_mech);
	for (i32 part_idx = 0; part_idx < (i32) PartType::Count; ++part_idx)
	{
		const i32 template_uid = in_mech.part_template_uids[part_idx];
		auto template_found = state.scene.objects.find(template_uid);
		if (template_found != state.scene.objects.end() && template_found->second.has_part &&
			!object_is_runtime_instance(template_found->second))
		{
			in_mech.part_instance_uids[part_idx] = mech_clone_part(in_mech, template_found->second);
		}
	}
}

i32 mech_create(i32 in_character_uid, const MechLoadout& in_loadout)
{
	auto character_found = state.scene.objects.find(in_character_uid);
	if (character_found == state.scene.objects.end() || !character_found->second.has_character ||
		state.mech.character_to_instance.contains(in_character_uid))
	{
		return -1;
	}

	const i32 mech_id = state.mech.next_instance_id++;
	MechInstance mech;
	mech.runtime_id = mech_id;
	mech.character_uid = in_character_uid;
	mech.loadout = in_loadout;
	state.mech.instances[mech_id] = mech;
	state.mech.character_to_instance[in_character_uid] = mech_id;
	state.mech.auto_spawn_opt_outs.erase(in_character_uid);
	mech_build_runtime_objects(state.mech.instances[mech_id]);
	return mech_id;
}

bool mech_destroy(i32 in_mech_instance_id, bool in_opt_out_auto_spawn)
{
	auto found = state.mech.instances.find(in_mech_instance_id);
	if (found == state.mech.instances.end()) return false;
	const i32 character_uid = found->second.character_uid;
	mech_destroy_runtime_objects(found->second);
	found->second.armature_instances.reset();
	state.mech.instances.erase(in_mech_instance_id);
	state.mech.character_to_instance.erase(character_uid);
	if (in_opt_out_auto_spawn) state.mech.auto_spawn_opt_outs[character_uid] = true;
	return true;
}

bool mech_set_loadout(i32 in_mech_instance_id, const MechLoadout& in_loadout)
{
	auto found = state.mech.instances.find(in_mech_instance_id);
	if (found == state.mech.instances.end()) return false;
	mech_destroy_runtime_objects(found->second);
	found->second.loadout = in_loadout;
	mech_build_runtime_objects(found->second);
	update_mech_transforms();
	return true;
}

void mech_reset_all()
{
	mech_suspend_runtime_objects();
	for (auto& [mech_id, mech] : state.mech.instances)
	{
		mech.armature_instances.reset();
	}
	state.mech.instances.clear();
	state.mech.character_to_instance.clear();
	state.mech.auto_spawn_opt_outs.clear();
	state.mech.next_instance_id = 1;
	state.mech.next_runtime_object_uid = -2;
}

void mech_reconcile_instances()
{
	mech_suspend_runtime_objects();

	DynamicArray<i32> removed_mechs;
	for (auto& [mech_id, mech] : state.mech.instances)
	{
		auto character_found = state.scene.objects.find(mech.character_uid);
		if (character_found == state.scene.objects.end() || !character_found->second.has_character)
		{
			removed_mechs.add(mech_id);
		}
	}
	for (i32 mech_id : removed_mechs)
	{
		mech_destroy(mech_id, false);
	}
	removed_mechs.reset();

	DynamicArray<i32> stale_opt_outs;
	for (auto& [character_uid, opted_out] : state.mech.auto_spawn_opt_outs)
	{
		auto character_found = state.scene.objects.find(character_uid);
		if (character_found == state.scene.objects.end() || !character_found->second.has_character)
		{
			stale_opt_outs.add(character_uid);
		}
	}
	for (i32 character_uid : stale_opt_outs)
	{
		state.mech.auto_spawn_opt_outs.erase(character_uid);
	}
	stale_opt_outs.reset();

	state.scene.player_character_id.reset();
	DynamicArray<i32> character_uids;
	for (auto& [object_uid, object] : state.scene.objects)
	{
		if (!object.has_character || object_is_runtime_instance(object)) continue;
		character_uids.add(object_uid);
		if (object.character.settings.player_controlled &&
			(!state.scene.player_character_id || object_uid < *state.scene.player_character_id))
		{
			state.scene.player_character_id = object_uid;
		}
	}
	for (i32 character_uid : character_uids)
	{
		if (!state.mech.character_to_instance.contains(character_uid) &&
			!state.mech.auto_spawn_opt_outs.contains(character_uid))
		{
			MechLoadout default_loadout;
			mech_create(character_uid, default_loadout);
		}
	}
	character_uids.reset();

	for (auto& [mech_id, mech] : state.mech.instances)
	{
		if (mech.part_instance_uids[(i32) PartType::Body] == -1)
		{
			mech_build_runtime_objects(mech);
		}
	}
	update_mech_transforms();
}

bool mech_part_instance_can_render(const Object& in_part, std::string& out_error)
{
	if (!in_part.has_mesh || !in_part.mesh.has_skinned_vertices) return true;
	auto armature_found = state.scene.objects.find(in_part.mesh.armature_id);
	if (armature_found == state.scene.objects.end() || !armature_found->second.has_armature ||
		!object_is_runtime_instance(armature_found->second))
	{
		out_error = "part armature is missing";
		return false;
	}
	return true;
}

// Injectable engine keeps selection tests reproducible; production seeds once per run.
std::mt19937& mech_weapon_random_engine()
{
	static std::mt19937 engine(std::random_device{}());
	return engine;
}

// Weapons are standalone, renderable templates: never runtime clones, parts or
// sockets, and they must name the bone label they attach to.
bool mech_is_weapon_template(const Object& in_object)
{
	if (object_is_runtime_instance(in_object) || !in_object.has_weapon || !in_object.has_mesh ||
		in_object.has_part || in_object.has_attachment_point || in_object.weapon.accepted_bone_label.empty() ||
		in_object.mesh.vertex_count == 0 || in_object.mesh.index_count == 0)
	{
		return false;
	}
	if (!in_object.mesh.has_skinned_vertices)
	{
		return true;
	}
	auto armature = state.scene.objects.find(in_object.mesh.armature_id);
	return armature != state.scene.objects.end() && armature->second.has_armature &&
		!object_is_runtime_instance(armature->second);
}

// Every attachment-labeled bone on the player's arm rigs is a weapon target.
std::vector<MechWeaponInstance> mech_collect_weapon_targets(const MechInstance& in_mech)
{
	std::vector<MechWeaponInstance> targets;
	if (state.scene.player_character_id != in_mech.character_uid)
	{
		return targets;
	}
	for (PartType slot : {PartType::LeftArm, PartType::RightArm})
	{
		auto arm = state.scene.objects.find(in_mech.part_instance_uids[(i32) slot]);
		if (arm == state.scene.objects.end() || !arm->second.has_mesh || !arm->second.mesh.has_skinned_vertices)
		{
			continue;
		}
		auto rig = state.scene.objects.find(arm->second.mesh.armature_id);
		if (rig == state.scene.objects.end() || !rig->second.has_armature)
		{
			continue;
		}
		const i32 rig_template_uid = rig->second.template_object_id;
		for (u32 bone_idx = 0; bone_idx < rig->second.armature.bone_count; ++bone_idx)
		{
			const ArmatureBone& bone = rig->second.armature.bones[bone_idx];
			if (!bone.name || !bone.attachment_label || !bone.attachment_label[0] ||
				weapon_target_exists(targets, rig_template_uid, bone.name))
			{
				continue;
			}
			MechWeaponInstance target;
			target.arm_slot = (i32) slot;
			target.arm_template_uid = in_mech.part_template_uids[(i32) slot];
			target.armature_template_uid = rig_template_uid;
			target.bone_name = bone.name;
			target.label = bone.attachment_label;
			targets.push_back(std::move(target));
		}
	}
	return targets;
}

// Drops cloned rigs that no part or weapon instance skins to anymore.
void mech_release_unused_armatures(MechInstance& io_mech)
{
	auto skins_to = [&](i32 in_object_uid, i32 in_armature_uid) {
		auto object = state.scene.objects.find(in_object_uid);
		return object != state.scene.objects.end() && object->second.has_mesh &&
			object->second.mesh.has_skinned_vertices && object->second.mesh.armature_id == in_armature_uid;
	};

	DynamicArray<MechArmatureInstance> retained;
	for (const MechArmatureInstance& mapping : io_mech.armature_instances)
	{
		bool used = false;
		for (i32 part_uid : io_mech.part_instance_uids)
		{
			used = used || skins_to(part_uid, mapping.instance_uid);
		}
		for (const MechWeaponInstance& weapon : io_mech.weapons)
		{
			used = used || skins_to(weapon.instance_uid, mapping.instance_uid);
		}

		if (used)
		{
			retained.add(mapping);
		}
		else
		{
			scene_remove_object(state, mapping.instance_uid);
		}
	}
	io_mech.armature_instances = std::move(retained);
}

void mech_reconcile_weapons(MechInstance& io_mech)
{
	std::vector<MechWeaponInstance> targets = mech_collect_weapon_targets(io_mech);
	std::vector<WeaponCandidate> candidates;
	if (!targets.empty())
	{
		for (const auto& [uid, object] : state.scene.objects)
		{
			if (mech_is_weapon_template(object))
			{
				candidates.push_back({uid, object.weapon.accepted_bone_label});
			}
		}
	}
	select_mech_weapons(targets, io_mech.weapons, std::move(candidates), mech_weapon_random_engine());

	bool removed_weapon = false;
	for (const MechWeaponInstance& old : io_mech.weapons)
	{
		const bool kept = std::any_of(targets.begin(), targets.end(), [&](const MechWeaponInstance& target) {
			return target.instance_uid == old.instance_uid;
		});
		if (old.instance_uid != -1 && !kept)
		{
			scene_remove_object(state, old.instance_uid);
			removed_weapon = true;
		}
	}
	io_mech.weapons = std::move(targets);

	// Finish enumeration before inserting objects: scene-map inserts invalidate references.
	for (MechWeaponInstance& target : io_mech.weapons)
	{
		if (target.weapon_template_uid != -1 && target.instance_uid == -1)
		{
			target.instance_uid = mech_clone_part(io_mech, state.scene.objects.at(target.weapon_template_uid));
		}
	}
	if (removed_weapon)
	{
		mech_release_unused_armatures(io_mech);
	}
}

void mech_update_weapon_transforms(MechInstance& io_mech, std::string& io_diagnostics, bool& io_has_lights)
{
	for (const MechWeaponInstance& equipped : io_mech.weapons)
	{
		auto weapon = state.scene.objects.find(equipped.instance_uid);
		if (weapon == state.scene.objects.end())
		{
			continue;
		}
		Object& object = weapon->second;
		object.visibility = false;
		io_has_lights = io_has_lights || object.has_light;

		std::string error;
		auto arm = state.scene.objects.find(io_mech.part_instance_uids[equipped.arm_slot]);
		if (arm == state.scene.objects.end() || !arm->second.visibility)
		{
			error = "owning arm is hidden or missing";
		}
		else
		{
			AttachmentPoint attachment;
			attachment.valid = true;
			attachment.binding_type = AttachmentBindingType::Bone;
			attachment.armature_id = equipped.armature_template_uid;
			attachment.bone_name = const_cast<char*>(equipped.bone_name.c_str());
			HMM_Mat4 world;
			if (attachment_point_world_matrix(io_mech, arm->second, attachment, world, error))
			{
				Transform transform = object.current_transform;
				if (!transform_from_matrix_location_rotation(world, transform))
				{
					error = "weapon bone transform is singular";
				}
				else if (mech_part_instance_can_render(object, error))
				{
					// Follow the bone's location and rotation, keeping the weapon's authored scale.
					transform.scale = object.initial_transform.scale;
					object.current_transform = transform;
					object.visibility = true;
				}
			}
		}

		io_diagnostics += " weapon[" + equipped.bone_name + "]=" + std::to_string(equipped.weapon_template_uid);
		if (!error.empty())
		{
			io_diagnostics += " error[weapon " + equipped.bone_name + "]=" + error;
		}
	}
}

void update_mech_transforms()
{
	// Reconciliation may add rigs, so it runs first. Then resample every rig from
	// its clip; IK overrides the sampled pose and must never feed back into it.
	for (auto& [mech_id, mech] : state.mech.instances)
	{
		mech_reconcile_weapons(mech);
	}
	for (auto& [uid, object] : state.scene.objects)
	{
		if (object.has_armature)
		{
			armature_sample_pose(object.armature);
		}
	}

	bool has_instanced_lights = false;
	for (auto& [mech_id, mech] : state.mech.instances)
	{
		std::string diagnostics = "mech=" + std::to_string(mech.runtime_id) +
			" character=" + std::to_string(mech.character_uid);
		for (i32 part_idx = 0; part_idx < (i32) PartType::Count; ++part_idx)
		{
			diagnostics += " template" + std::to_string(part_idx) + "=" +
				std::to_string(mech.part_template_uids[part_idx]);
			diagnostics += " instance" + std::to_string(part_idx) + "=" +
				std::to_string(mech.part_instance_uids[part_idx]);
			diagnostics += " socket" + std::to_string(part_idx) + "=" +
				std::to_string(mech.socket_template_uids[part_idx]);
		}

		auto add_error = [&](PartType part_type, const std::string& message) {
			diagnostics += " error[" + std::string(part_type_name(part_type)) + "]=" + message;
		};

		for (i32 instance_uid : mech.part_instance_uids)
		{
			auto part_found = state.scene.objects.find(instance_uid);
			if (part_found != state.scene.objects.end())
			{
				part_found->second.visibility = false;
				has_instanced_lights = has_instanced_lights || part_found->second.has_light;
			}
		}

		auto character_found = state.scene.objects.find(mech.character_uid);
		if (character_found == state.scene.objects.end() || !character_found->second.has_character)
		{
			add_error(PartType::Body, "Character is missing");
		}
		else
		{
			auto body_found = state.scene.objects.find(mech.part_instance_uids[(i32) PartType::Body]);
			if (body_found == state.scene.objects.end() || !object_is_runtime_instance(body_found->second))
			{
				const MechLoadoutSlot& slot = mech.loadout.slots[(i32) PartType::Body];
				add_error(PartType::Body, slot.selection == MechLoadoutSelectionType::TemplateUid ?
					"explicit template is unavailable" : "default template is missing");
			}
			else
			{
				Object& body = body_found->second;
				body.current_transform.location = character_found->second.current_transform.location;
				body.current_transform.rotation = character_found->second.character.body_rotation;
				body.current_transform.scale = body.initial_transform.scale;
				std::string body_error;
				if (mech_part_instance_can_render(body, body_error)) body.visibility = true;
				else add_error(PartType::Body, body_error);

				for (i32 part_idx = (i32) PartType::Legs; part_idx < (i32) PartType::Count; ++part_idx)
				{
					const PartType part_type = (PartType) part_idx;
					auto part_found = state.scene.objects.find(mech.part_instance_uids[part_idx]);
					if (part_found == state.scene.objects.end() || !object_is_runtime_instance(part_found->second))
					{
						const MechLoadoutSlot& slot = mech.loadout.slots[part_idx];
						add_error(part_type, slot.selection == MechLoadoutSelectionType::TemplateUid ?
							"explicit template is unavailable" : "default template is missing");
						continue;
					}

					auto socket_found = state.scene.objects.find(mech.socket_template_uids[part_idx]);
					if (socket_found == state.scene.objects.end() || !socket_found->second.has_attachment_point)
					{
						add_error(part_type, "Body socket is missing");
						continue;
					}

					HMM_Mat4 socket_world;
					std::string socket_error;
					if (!attachment_point_world_matrix(
						mech, body, socket_found->second.attachment_point, socket_world, socket_error))
					{
						add_error(part_type, socket_error);
						continue;
					}

					Object& part = part_found->second;
					Transform attached_transform = part.current_transform;
					if (!transform_from_matrix_location_rotation(socket_world, attached_transform))
					{
						add_error(part_type, "socket transform is singular");
						continue;
					}
					// Legs inherit the socket position, but keep their own world heading.
					if (part_type == PartType::Legs)
					{
						attached_transform.rotation = character_found->second.character.legs_rotation;
					}
					attached_transform.scale = part.initial_transform.scale;
					part.current_transform = attached_transform;
					std::string part_error;
					if (mech_part_instance_can_render(part, part_error)) part.visibility = true;
					else add_error(part_type, part_error);
				}
			}
		}

		ArmIK::update(mech, diagnostics);
		mech_update_weapon_transforms(mech, diagnostics, has_instanced_lights);
		if (diagnostics != mech.last_diagnostic_signature)
		{
			printf("Mech assembly: %s\n", diagnostics.c_str());
			mech.last_diagnostic_signature = diagnostics;
		}
	}
	if (has_instanced_lights) mark_lighting_dirty(state);
}
