#include <cassert>
#include <cstdio>
#include "game_object/weapon_selection.h"

MechWeaponInstance hand(int owner, int rig, const char* name, const char* label = "Hand")
{
	MechWeaponInstance target;
	target.arm_template_uid = owner;
	target.armature_template_uid = rig;
	target.bone_name = name;
	target.label = label;
	return target;
}

int main()
{
	std::mt19937 random(1234);
	const std::vector<MechWeaponInstance> hands = {hand(1, 10, "L"), hand(2, 20, "R")};
	auto single_weapon = hands;
	select_mech_weapons(single_weapon, {}, {{100, "Hand"}}, random);
	assert(single_weapon[0].weapon_template_uid == 100 && single_weapon[1].weapon_template_uid == 100);
	auto selected = hands;
	select_mech_weapons(selected, {}, {{100, "Hand"}, {101, "hand"}, {102, "Foot"}}, random);
	assert(selected[0].weapon_template_uid == 100 && selected[1].weapon_template_uid == 100);
	selected[0].instance_uid = -10;
	selected[0].fire_cooldown_seconds = .125f;
	selected[1].instance_uid = -11;
	auto refreshed = hands;
	const auto before = random;
	select_mech_weapons(refreshed, selected, {{99, "Hand"}, {100, "Hand"}}, random);
	assert(random == before); // Unrelated updates and new candidates do not reroll.
	assert(refreshed[0].instance_uid == -10 && refreshed[1].instance_uid == -11);
	assert(refreshed[0].fire_cooldown_seconds == .125f && refreshed[1].fire_cooldown_seconds == 0.0f);
	for (auto& target : selected) target.instance_uid = -1; // Live Link suspension.
	refreshed = hands;
	select_mech_weapons(refreshed, selected, {{100, "Hand"}}, random);
	assert(refreshed[0].weapon_template_uid == 100 && refreshed[0].instance_uid == -1);
	refreshed = hands;
	select_mech_weapons(refreshed, selected, {{200, "Hand"}}, random); // Deleted template.
	assert(refreshed[0].weapon_template_uid == 200 && refreshed[0].instance_uid == -1);
	refreshed = {hand(3, 10, "L"), hand(2, 20, "R", "Foot"), hand(1, 10, "none", "")};
	select_mech_weapons(refreshed, selected, {{100, "Hand"}, {201, "Foot"}, {202, ""}}, random);
	assert(refreshed[0].instance_uid == -1); // Replacement arm gets its own instance.
	assert(refreshed[1].weapon_template_uid == 201 && refreshed[2].weapon_template_uid == -1);
	refreshed = hands;
	select_mech_weapons(refreshed, selected, {}, random);
	assert(refreshed[0].weapon_template_uid == -1 && refreshed[1].weapon_template_uid == -1);
	assert(weapon_target_exists(hands, 10, "L"));
	assert(!weapon_target_exists(hands, 20, "L")); // Names scoped to armature occurrence.
	assert(!weapon_target_exists(hands, 10, "R"));
	bool saw_first = false, saw_second = false;
	for (int trial = 0; trial < 64; ++trial)
	{
		auto a = hands, b = hands;
		std::mt19937 seed_a(trial), seed_b(trial);
		select_mech_weapons(a, {}, {{100, "Hand"}, {200, "Hand"}}, seed_a);
		select_mech_weapons(b, {}, {{200, "Hand"}, {100, "Hand"}}, seed_b);
		assert(a[0].weapon_template_uid == b[0].weapon_template_uid);
		saw_first |= a[0].weapon_template_uid == 100;
		saw_second |= a[0].weapon_template_uid == 200;
	}
	assert(saw_first && saw_second);
	puts("weapon selection tests passed");
}
