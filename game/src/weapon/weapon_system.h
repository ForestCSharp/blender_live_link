#pragma once

#include <algorithm>
#include <cmath>

#include "game_object/projectile.h"
#include "state/state.h"

namespace WeaponSystem
{
	// Called after mech attachment and animation transforms have been updated.
	void update(f32 in_delta_seconds)
	{
		const i32 requests = state.input.pending_fire_requests;
		state.input.pending_fire_requests = 0;
		const bool firing_suppressed = !state.runtime.is_simulating || state.debug_camera.active ||
			(state.gi.probe_isolation_enable && state.gi.show_probes);
		if (firing_suppressed || !state.scene.player_character_id)
		{
			state.input.weapon_fire_held = false;
			return;
		}

		auto mech_id = state.mech.character_to_instance.find(*state.scene.player_character_id);
		if (mech_id == state.mech.character_to_instance.end())
		{
			return;
		}
		auto mech = state.mech.instances.find(mech_id->second);
		if (mech == state.mech.instances.end())
		{
			return;
		}

		for (MechWeaponInstance& equipped : mech->second.weapons)
		{
			// May go negative while the trigger is held, carrying the overdue
			// time into the next shot. Idle time never accumulates a burst.
			const f32 cooldown = equipped.fire_cooldown_seconds > 0.0f
				? equipped.fire_cooldown_seconds - std::max(0.0f, in_delta_seconds)
				: 0.0f;
			equipped.fire_cooldown_seconds = std::max(0.0f, cooldown);

			auto weapon = state.scene.objects.find(equipped.instance_uid);
			auto weapon_template = state.scene.objects.find(equipped.weapon_template_uid);
			if (weapon == state.scene.objects.end() || !weapon->second.visibility ||
				weapon_template == state.scene.objects.end() || !weapon_template->second.has_weapon ||
				!weapon_template->second.weapon.muzzle_valid)
			{
				continue;
			}

			const f32 interval = weapon_template->second.weapon.rate_of_fire_seconds;
			i32 shots = requests;
			if (std::isfinite(interval) && interval > 0.0f)
			{
				const bool triggered = requests > 0;
				if ((!triggered && !state.input.weapon_fire_held) || cooldown > 0.0f)
				{
					continue;
				}
				// Keep the cadence across frame boundaries, limiting catch-up work
				// to the projectile cap for very small authored intervals.
				if (triggered)
				{
					shots = 1;
					equipped.fire_cooldown_seconds = interval;
				}
				else
				{
					shots = (i32) std::min((f32) MAX_ACTIVE_PROJECTILES, 1.0f + floorf(-cooldown / interval));
					equipped.fire_cooldown_seconds = interval + fmodf(cooldown, interval);
				}
			}
			else
			{
				equipped.fire_cooldown_seconds = 0.0f;
			}
			if (shots <= 0)
			{
				continue;
			}

			const HMM_Mat4 muzzle = object_get_model_matrix(weapon->second) *
				weapon_template->second.weapon.muzzle_local_transform;
			const HMM_Vec3 origin = muzzle.Columns[3].XYZ;
			const HMM_Vec3 direction = muzzle.Columns[1].XYZ;
			if (!std::isfinite(origin.X) || !std::isfinite(origin.Y) || !std::isfinite(origin.Z))
			{
				continue;
			}
			for (i32 shot = 0; shot < shots; ++shot)
			{
				projectile_spawn(origin, direction);
			}
		}
	}
}
