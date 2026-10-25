#pragma once

#include <cmath>
#include <random>

#include "game_object/enemy.h"
#include "game_object/character.h"
#include "state/state.h"

// Roaming enemy mechs: wander near their spawn point, then turn, aim and fire
// at the player once inside engage_range. Movement goes through the same
// character_move / character_turn_heading calls the player uses, and aim/fire
// feed MechInstance so arm IK and WeaponSystem treat them like the player.
namespace EnemySystem
{
	constexpr f32 roam_speed_scale = 0.3f;
	constexpr f32 engage_speed_scale = 0.5f;
	constexpr f32 arrive_distance_m = 1.5f;
	constexpr f32 fire_interval_s = 0.75f;
	// Only fire once the body is roughly facing the player (cos 20 degrees)
	constexpr f32 fire_facing_cos = 0.94f;

	HMM_Vec3 flat(HMM_Vec3 in_vector)
	{
		return HMM_V3(in_vector.X, in_vector.Y, 0.0f);
	}

	HMM_Vec3 character_position(const Character& in_character)
	{
		const JPH::Vec3 position = in_character.jph_character->GetPosition();
		return HMM_V3(position.GetX(), position.GetY(), position.GetZ());
	}

	// Approximate shoulder/chest height above the capsule centre
	HMM_Vec3 chest(const Character& in_character)
	{
		return character_position(in_character) + HMM_V3(0, 0, 0.25f * in_character.settings.height);
	}

	void update_enemy(i32 in_uid, Object& io_object, const Object* in_player, f32 in_delta_time)
	{
		Character& character = io_object.character;
		Enemy& enemy = io_object.enemy;
		auto mech_id = state.mech.character_to_instance.find(in_uid);
		MechInstance* mech = mech_id != state.mech.character_to_instance.end()
			? &state.mech.instances.at(mech_id->second)
			: nullptr;

		const HMM_Vec3 position = character_position(character);
		HMM_Vec3 move = HMM_V3(0, 0, 0);
		HMM_Vec3 aim = HMM_V3(0, 0, 0);
		enemy.fire_cooldown_seconds = MAX(enemy.fire_cooldown_seconds - in_delta_time, 0.0f);

		const HMM_Vec3 to_player = in_player ? flat(character_position(in_player->character) - position) : HMM_V3(0, 0, 0);
		const f32 player_distance = HMM_LenV3(to_player);
		if (in_player && player_distance <= enemy.settings.engage_range && player_distance > 1.0e-3f)
		{
			// Engage: hold a standoff at half the engage range
			enemy.has_wander_target = false;
			const HMM_Vec3 to_player_dir = to_player / player_distance;
			const f32 standoff = 0.5f * enemy.settings.engage_range;
			if (player_distance > standoff + 2.0f)
			{
				move = to_player_dir * engage_speed_scale;
			}
			else if (player_distance < standoff - 2.0f)
			{
				move = to_player_dir * -engage_speed_scale;
			}
			character_turn_heading(character.body_rotation, to_player_dir, in_delta_time);

			const HMM_Vec3 aim_vector = chest(in_player->character) - chest(character);
			if (HMM_LenSqrV3(aim_vector) > 1.0e-6f)
			{
				aim = HMM_NormV3(aim_vector);
			}
			const HMM_Vec3 body_forward = HMM_RotateV3Q(HMM_V3(0, 1, 0), character.body_rotation);
			if (mech && enemy.fire_cooldown_seconds <= 0.0f && HMM_DotV3(body_forward, to_player_dir) >= fire_facing_cos)
			{
				mech->fire_requests += 1;
				enemy.fire_cooldown_seconds = fire_interval_s;
			}
		}
		else
		{
			// Roam: walk to a random point near home, pause, repeat
			const HMM_Vec3 home = character.settings.initial_location.XYZ;
			if (!enemy.has_wander_target)
			{
				enemy.pause_seconds -= in_delta_time;
				if (enemy.pause_seconds <= 0.0f)
				{
					enemy.wander_target = enemy_pick_wander_target(home, enemy.settings.roam_radius, enemy.rng);
					enemy.has_wander_target = true;
				}
			}
			if (enemy.has_wander_target)
			{
				const HMM_Vec3 to_target = flat(enemy.wander_target - position);
				const f32 target_distance = HMM_LenV3(to_target);
				if (target_distance <= arrive_distance_m)
				{
					enemy.has_wander_target = false;
					enemy.pause_seconds = std::uniform_real_distribution<f32>(1.0f, 3.0f)(enemy.rng);
				}
				else
				{
					move = (to_target / target_distance) * roam_speed_scale;
					character_turn_heading(character.body_rotation, move, in_delta_time);
				}
			}
		}

		character_move(character, move, false, in_delta_time);
		character_turn_heading(character.legs_rotation, move, in_delta_time);
		if (mech)
		{
			mech->aim_direction = aim;
		}
	}

	void update(f32 in_delta_time)
	{
		const Object* player = nullptr;
		if (state.scene.player_character_id)
		{
			auto found = state.scene.objects.find(*state.scene.player_character_id);
			if (found != state.scene.objects.end() && found->second.has_character && found->second.character.jph_character)
			{
				player = &found->second;
			}
		}
		for (auto& [uid, object] : state.scene.objects)
		{
			if (!object.has_enemy || !object.has_character || !object.character.jph_character ||
				object.character.settings.player_controlled)
			{
				continue;
			}
			update_enemy(uid, object, player, in_delta_time);
		}
	}
}
