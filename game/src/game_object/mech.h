#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

#include "state/state.h"
#include "game_object/attachment_point.h"

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
		template_found->second.is_runtime_instance)
	{
		return -1;
	}

	Object& armature_template = template_found->second;
	const i32 instance_uid = mech_allocate_runtime_object_uid();
	Object armature_instance = object_create(
		instance_uid,
		mech_instance_name(armature_template.name, in_mech.runtime_id),
		false,
		armature_template.initial_transform.location,
		armature_template.initial_transform.rotation,
		armature_template.initial_transform.scale
	);
	armature_instance.is_runtime_instance = true;
	armature_instance.template_object_id = in_template_uid;
	armature_instance.mech_instance_id = in_mech.runtime_id;
	armature_instance.owns_armature_resources = false;
	armature_instance.has_armature = true;
	armature_instance.armature = armature_template.armature;
	armature_instance.armature.playback_time = 0.0f;
	armature_instance.armature.current_frame = 0;

	state.scene.objects[instance_uid] = armature_instance;
	in_mech.armature_instances.add({
		.template_uid = in_template_uid,
		.instance_uid = instance_uid,
	});
	scene_mark_indexes_dirty(state);
	return instance_uid;
}

i32 mech_clone_part(MechInstance& in_mech, const Object& in_template)
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
	instance.is_runtime_instance = true;
	instance.template_object_id = in_template.unique_id;
	instance.mech_instance_id = in_mech.runtime_id;
	instance.owns_mesh_resources = false;
	instance.owns_skin_matrices = true;
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

	state.scene.objects[instance_uid] = instance;
	scene_mark_indexes_dirty(state);
	if (instance.has_light) mark_lighting_dirty(state);
	return instance_uid;
}

void mech_destroy_runtime_objects(MechInstance& in_mech)
{
	for (i32 part_idx = 0; part_idx < (i32) PartType::Count; ++part_idx)
	{
		const i32 instance_uid = in_mech.part_instance_uids[part_idx];
		auto found = state.scene.objects.find(instance_uid);
		if (found != state.scene.objects.end() && found->second.is_runtime_instance)
		{
			if (found->second.has_light) mark_lighting_dirty(state);
			object_cleanup(found->second);
			state.scene.objects.erase(instance_uid);
		}
		in_mech.part_instance_uids[part_idx] = -1;
	}
	for (const MechArmatureInstance& mapping : in_mech.armature_instances)
	{
		auto found = state.scene.objects.find(mapping.instance_uid);
		if (found != state.scene.objects.end() && found->second.is_runtime_instance)
		{
			object_cleanup(found->second);
			state.scene.objects.erase(mapping.instance_uid);
		}
	}
	in_mech.armature_instances.clear();
	scene_mark_indexes_dirty(state);
}

void mech_suspend_runtime_objects()
{
	for (auto& [mech_id, mech] : state.mech.instances)
	{
		mech_destroy_runtime_objects(mech);
	}

	// Defensive cleanup for a partially-created instance that was not yet
	// recorded in a descriptor.
	StretchyBuffer<i32> orphan_uids;
	for (auto& [object_uid, object] : state.scene.objects)
	{
		if (object.is_runtime_instance) orphan_uids.add(object_uid);
	}
	for (i32 orphan_uid : orphan_uids)
	{
		object_cleanup(state.scene.objects[orphan_uid]);
		state.scene.objects.erase(orphan_uid);
	}
	orphan_uids.reset();
	scene_mark_indexes_dirty(state);
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
				!explicit_found->second.is_runtime_instance &&
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
			!template_found->second.is_runtime_instance)
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

	StretchyBuffer<i32> removed_mechs;
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

	StretchyBuffer<i32> stale_opt_outs;
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
	StretchyBuffer<i32> character_uids;
	for (auto& [object_uid, object] : state.scene.objects)
	{
		if (!object.has_character || object.is_runtime_instance) continue;
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
		!armature_found->second.is_runtime_instance)
	{
		out_error = "part armature is missing";
		return false;
	}
	return true;
}

void update_mech_transforms()
{
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
			if (body_found == state.scene.objects.end() || !body_found->second.is_runtime_instance)
			{
				const MechLoadoutSlot& slot = mech.loadout.slots[(i32) PartType::Body];
				add_error(PartType::Body, slot.selection == MechLoadoutSelectionType::TemplateUid ?
					"explicit template is unavailable" : "default template is missing");
			}
			else
			{
				Object& body = body_found->second;
				body.current_transform.location = character_found->second.current_transform.location;
				body.current_transform.rotation = character_found->second.current_transform.rotation;
				body.current_transform.scale = body.initial_transform.scale;
				std::string body_error;
				if (mech_part_instance_can_render(body, body_error)) body.visibility = true;
				else add_error(PartType::Body, body_error);

				for (i32 part_idx = (i32) PartType::Legs; part_idx < (i32) PartType::Count; ++part_idx)
				{
					const PartType part_type = (PartType) part_idx;
					auto part_found = state.scene.objects.find(mech.part_instance_uids[part_idx]);
					if (part_found == state.scene.objects.end() || !part_found->second.is_runtime_instance)
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
					attached_transform.scale = part.initial_transform.scale;
					part.current_transform = attached_transform;
					std::string part_error;
					if (mech_part_instance_can_render(part, part_error)) part.visibility = true;
					else add_error(part_type, part_error);
				}
			}
		}

		if (diagnostics != mech.last_diagnostic_signature)
		{
			printf("Mech assembly: %s\n", diagnostics.c_str());
			mech.last_diagnostic_signature = diagnostics;
		}
	}
	if (has_instanced_lights) mark_lighting_dirty(state);
}

