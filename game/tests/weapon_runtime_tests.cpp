// Integration test using the actual scene, cloning, animation and GPU ownership paths.
// Run from game/ after generating /tmp/weapon_fixture.bin with weapon_export_blender.py.
#define GAME_BUILD_CONFIG_NAME "WeaponTest"
#define main weapon_test_game_main
#include "../src/main.cpp"
#undef main
#include <cassert>

MechInstance& player_mech()
{
	return state.mech.instances.at(state.mech.character_to_instance.at(*state.scene.player_character_id));
}

void check_attachment(const MechWeaponInstance& equipped)
{
	const Object& weapon = state.scene.objects.at(equipped.instance_uid);
	const Object& arm = state.scene.objects.at(player_mech().part_instance_uids[equipped.arm_slot]);
	const Armature& rig = state.scene.objects.at(arm.mesh.armature_id).armature;
	int index = -1;
	for (u32 i = 0; i < rig.bone_count; ++i)
		if (equipped.bone_name == rig.bones[i].name) index = i;
	assert(index >= 0 && weapon.visibility);
	HMM_Mat4 pose = HMM_InvGeneralM4(rig.bones[index].inverse_bind_matrix);
	if (rig.animation_count)
	{
		const auto& clip = rig.animations[rig.active_animation_index];
		pose = HMM_MulM4(clip.skin_matrices[rig.current_frame * clip.bone_count + index], pose);
	}
	const HMM_Mat4 expected = object_get_model_matrix(arm) * arm.mesh.armature_to_mesh * pose;
	const HMM_Mat4 actual_rotation = HMM_QToM4(weapon.current_transform.rotation);
	for (int axis = 0; axis < 3; ++axis)
	{
		assert(fabsf(weapon.current_transform.location.Elements[axis] - expected.Elements[3][axis]) < .0001f);
		assert(weapon.current_transform.scale.Elements[axis] == weapon.initial_transform.scale.Elements[axis]);
		const auto direction = HMM_NormV3(HMM_V3(expected.Elements[axis][0], expected.Elements[axis][1], expected.Elements[axis][2]));
		for (int row = 0; row < 3; ++row)
			assert(fabsf(actual_rotation.Elements[axis][row] - direction.Elements[row]) < .0001f);
	}
}

void seed_for_first_candidate()
{
	// std::uniform_int_distribution mappings differ between libc++ and libstdc++.
	for (unsigned seed = 0;; ++seed)
	{
		std::mt19937 probe(seed);
		if (std::uniform_int_distribution<size_t>(0, 1)(probe) == 0)
		{
			mech_weapon_random_engine().seed(seed);
			return;
		}
	}
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	assert(glfwInit());
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow* window = glfwCreateWindow(128, 128, "Weapon integration tests", nullptr, nullptr);
	assert(window);
	state.window.handle = window;
	jolt_init();
	vulkan_context_init(&state.vk, window);
	seed_for_first_candidate();
	assert(LiveLinkSystem::load_initial_file(state, "/tmp/weapon_fixture.bin"));
	LiveLinkSystem::drain(state);
	update_mech_transforms();
	assert(player_mech().weapons.size() == 2);
	for (const auto& weapon : player_mech().weapons) check_attachment(weapon);
	assert(state.mech.instances.size() == 2);
	for (const auto& [id, mech] : state.mech.instances)
		if (mech.character_uid != *state.scene.player_character_id) assert(mech.weapons.empty());
	const auto original = player_mech().weapons;
	assert(state.scene.objects.at(original[0].instance_uid).mesh.has_skinned_vertices);
	// Unequipping a skinned weapon must release its otherwise unused cloned rig.
	const int player_id = *state.scene.player_character_id;
	const int player_mech_id = player_mech().runtime_id;
	state.scene.player_character_id.reset();
	update_mech_transforms();
	assert(state.mech.instances.at(player_mech_id).armature_instances.length() == 2);
	state.scene.player_character_id = player_id;
	seed_for_first_candidate();
	update_mech_transforms();
	const auto equipped_again = player_mech().weapons;
	update_mech_transforms();
	assert(player_mech().weapons[0].instance_uid == equipped_again[0].instance_uid);
	// Rotate/translate the mech and advance the actual sampled bone animation.
	auto& character = state.scene.objects.at(*state.scene.player_character_id);
	character.current_transform.location.X = 13;
	character.character.body_rotation = HMM_QFromAxisAngle_RH(HMM_V3(0, 0, 1), .8f);
	state.runtime.is_simulating = true;
	state.animation.is_playing = true;
	AnimationSystem::advance(state, .4f);
	assert(state.scene.objects.at(player_mech().armature_instances[0].instance_uid).armature.current_frame > 0);
	update_mech_transforms();
	for (const auto& weapon : player_mech().weapons) check_attachment(weapon);
	// A first click locks the mouse and fires from both current animated muzzles.
	assert(!state.input.is_mouse_locked);
	InputSystem::mouse_button_callback(window, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
	assert(state.input.is_mouse_locked && state.input.pending_fire_requests == 1);
	projectile_consume_fire_requests();
	assert(state.projectiles.size() == 2);
	for (size_t index = 0; index < state.projectiles.size(); ++index)
	{
		const auto& equipped = player_mech().weapons[index];
		const Object& weapon = state.scene.objects.at(equipped.instance_uid);
		const Weapon& template_weapon = state.scene.objects.at(equipped.weapon_template_uid).weapon;
		const HMM_Mat4 muzzle = HMM_MulM4(object_get_model_matrix(weapon), template_weapon.muzzle_local_transform);
		const Object& sphere = state.scene.objects.at(state.projectiles[index].object_uid);
		for (int axis = 0; axis < 3; ++axis)
			assert(fabsf(sphere.current_transform.location.Elements[axis] - muzzle.Elements[3][axis]) < 1e-4f);
		const JPH::Vec3 velocity = jolt_state.physics_system.GetBodyInterface().GetLinearVelocity(sphere.rigid_body.jolt_body->GetID());
		assert(fabsf(velocity.Length() - PROJECTILE_SPEED_M_S) < 1e-3f);
		const HMM_Vec3 expected_direction = HMM_NormV3(HMM_V3(muzzle.Elements[1][0], muzzle.Elements[1][1], muzzle.Elements[1][2]));
		for (int axis = 0; axis < 3; ++axis)
			assert(fabsf(velocity[axis] / PROJECTILE_SPEED_M_S - expected_direction.Elements[axis]) < 1e-4f);
	}
	// One hidden gun does not emit; an invalid shared template silences both.
	state.scene.objects.at(player_mech().weapons[0].instance_uid).visibility = false;
	state.input.pending_fire_requests = 1;
	projectile_consume_fire_requests();
	assert(state.projectiles.size() == 3);
	update_mech_transforms();
	for (const auto& equipped : player_mech().weapons)
		state.scene.objects.at(equipped.weapon_template_uid).weapon.muzzle_valid = false;
	state.input.pending_fire_requests = 1;
	projectile_consume_fire_requests();
	assert(state.projectiles.size() == 3);
	for (const auto& equipped : player_mech().weapons)
		state.scene.objects.at(equipped.weapon_template_uid).weapon.muzzle_valid = true;
	const float initial_y = state.scene.objects.at(state.projectiles[0].object_uid).current_transform.location.Y;
	jolt_update(.1f);
	object_copy_physics_transform(state.scene.objects.at(state.projectiles[0].object_uid),
		jolt_state.physics_system.GetBodyInterface());
	assert(fabsf(state.scene.objects.at(state.projectiles[0].object_uid).current_transform.location.Y - initial_y) > .01f);
	state.runtime.is_simulating = false;
	InputSystem::mouse_button_callback(window, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
	assert(state.input.pending_fire_requests == 0);
	state.runtime.is_simulating = true;
	state.gi.probe_isolation_enable = state.gi.show_probes = true;
	InputSystem::mouse_button_callback(window, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
	assert(state.input.pending_fire_requests == 0 && state.input.gi_probe_pick_requested);
	state.gi.probe_isolation_enable = state.gi.show_probes = false;
	state.input.gi_probe_pick_requested = false;
	const int first_projectile = state.projectiles.front().object_uid;
	state.input.pending_fire_requests = 33;
	projectile_consume_fire_requests();
	assert(state.projectiles.size() == MAX_ACTIVE_PROJECTILES);
	assert(!state.scene.objects.contains(first_projectile));
	projectile_update_lifetimes(PROJECTILE_LIFETIME_S + .01f);
	assert(state.projectiles.empty());
	// No animation uses the bind pose, not a stale skinning matrix.
	for (const auto& mapping : player_mech().armature_instances)
		state.scene.objects.at(mapping.instance_uid).armature.animation_count = 0;
	update_mech_transforms();
	for (const auto& weapon : player_mech().weapons) check_attachment(weapon);
	mech_reconcile_instances();
	assert(player_mech().weapons[0].weapon_template_uid == original[0].weapon_template_uid);
	assert(player_mech().weapons[0].instance_uid != original[0].instance_uid);
	for (const auto& weapon : original) assert(!state.scene.objects.contains(weapon.instance_uid));

	const int socket_uid = player_mech().socket_template_uids[(int) PartType::LeftArm];
	state.scene.objects.at(socket_uid).attachment_point.valid = false;
	update_mech_transforms();
	assert(!state.scene.objects.at(player_mech().weapons[0].instance_uid).visibility);
	state.scene.objects.at(socket_uid).attachment_point.valid = true;
	update_mech_transforms();
	assert(state.scene.objects.at(player_mech().weapons[0].instance_uid).visibility);

	// Shared armature/bone targets must not equip twice.
	auto& mech = player_mech();
	state.scene.objects.at(mech.part_instance_uids[(int) PartType::RightArm]).mesh.armature_id =
		state.scene.objects.at(mech.part_instance_uids[(int) PartType::LeftArm]).mesh.armature_id;
	update_mech_transforms();
	assert(mech.weapons.size() == 1);
	mech_reconcile_instances();
	assert(player_mech().weapons.size() == 2);

	// Simulate an incoming deletion batch: suspend borrowed allocations before removal.
	const int removed = player_mech().weapons[0].weapon_template_uid;
	state.input.pending_fire_requests = 1;
	projectile_consume_fire_requests();
	assert(!state.projectiles.empty());
	const int retained_projectile = state.projectiles.front().object_uid;
	mech_suspend_runtime_objects();
	assert(state.scene.objects.contains(retained_projectile));
	scene_remove_object(state, removed);
	mech_reconcile_instances();
	for (const auto& weapon : player_mech().weapons)
	{
		assert(weapon.weapon_template_uid != removed);
		check_attachment(weapon);
	}
	MechLoadout loadout = player_mech().loadout;
	loadout.slots[(int) PartType::LeftArm].selection = MechLoadoutSelectionType::TemplateUid;
	loadout.slots[(int) PartType::LeftArm].template_uid = 999999;
	mech_set_loadout(player_mech().runtime_id, loadout);
	assert(player_mech().weapons.size() == 1);
	loadout.slots[(int) PartType::LeftArm] = {};
	mech_set_loadout(player_mech().runtime_id, loadout);
	assert(player_mech().weapons.size() == 2);
	const int rig_uid = player_mech().weapons[0].armature_template_uid;
	mech_suspend_runtime_objects();
	auto& rig = state.scene.objects.at(rig_uid).armature;
	for (u32 i = 0; i < rig.bone_count; ++i)
		if (rig.bones[i].attachment_label) rig.bones[i].attachment_label[0] = '\0';
	mech_reconcile_instances();
	assert(player_mech().weapons.size() == 1);
	// Losing player control removes equipment even without a Live Link rebuild.
	const int player = *state.scene.player_character_id;
	const int mech_id = player_mech().runtime_id;
	state.scene.player_character_id.reset();
	update_mech_transforms();
	assert(state.mech.instances.at(mech_id).weapons.empty());
	state.scene.player_character_id = player;
	update_mech_transforms();
	assert(player_mech().weapons.size() == 1);
	state.input.pending_fire_requests = 1;
	projectile_consume_fire_requests();
	assert(!state.projectiles.empty());

	VK_CHECK(vulkan_device_wait_idle(&state.vk));
	mech_reset_all();
	for (const auto& [uid, object] : state.scene.objects)
		assert(object.storage_kind != ObjectStorageKind::RuntimePart &&
			object.storage_kind != ObjectStorageKind::RuntimeArmature);
	scene_clear_objects(state);
	assert(state.projectiles.empty());
	LiveLinkSystem::cleanup_imported_resources(state);
	jolt_shutdown();
	vulkan_context_shutdown(&state.vk);
	glfwDestroyWindow(window);
	glfwTerminate();
	puts("WEAPON_RUNTIME_TESTS_PASSED");
}
