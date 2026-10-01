#pragma once

#include <algorithm>
#include <random>
#include <string>
#include <vector>

// Persisted between runtime rebuilds. Names identify bones even when indices change.
struct MechWeaponInstance
{
	int arm_template_uid = -1;
	int armature_template_uid = -1;
	std::string bone_name;
	std::string label;
	int weapon_template_uid = -1;
	int instance_uid = -1;
	int arm_slot = -1;
	float fire_cooldown_seconds = 0.0f;
};

struct WeaponCandidate
{
	int uid;
	std::string label;
};

// Candidates have already passed renderability checks. Sorting makes seeded runs
// reproducible regardless of scene-map iteration order. Each target draws independently.
inline void select_mech_weapons(std::vector<MechWeaponInstance>& targets,
								const std::vector<MechWeaponInstance>& previous,
								std::vector<WeaponCandidate> candidates,
								std::mt19937& random)
{
	std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
		return a.uid < b.uid;
	});
	for (auto& target : targets)
	{
		if (target.label.empty()) continue;
		std::vector<int> compatible;
		for (const auto& candidate : candidates)
			if (candidate.label == target.label) compatible.push_back(candidate.uid);
		for (const auto& old : previous)
		{
			if (old.arm_template_uid == target.arm_template_uid &&
				old.armature_template_uid == target.armature_template_uid &&
				old.bone_name == target.bone_name && old.label == target.label &&
				std::find(compatible.begin(), compatible.end(), old.weapon_template_uid) != compatible.end())
			{
				target.weapon_template_uid = old.weapon_template_uid;
				target.instance_uid = old.instance_uid;
				target.fire_cooldown_seconds = old.fire_cooldown_seconds;
				break;
			}
		}
		if (target.weapon_template_uid == -1 && !compatible.empty())
		{
			// Templates are reusable: a lone candidate equips every matching hand.
			if (compatible.size() == 1)
				target.weapon_template_uid = compatible.front();
			else
			{
				std::uniform_int_distribution<size_t> choose(0, compatible.size() - 1);
				target.weapon_template_uid = compatible[choose(random)];
			}
		}
	}
}

inline bool weapon_target_exists(const std::vector<MechWeaponInstance>& targets,
								 int armature_uid, const std::string& bone_name)
{
	return std::any_of(targets.begin(), targets.end(), [&](const auto& target) {
		return target.armature_template_uid == armature_uid && target.bone_name == bone_name;
	});
}
