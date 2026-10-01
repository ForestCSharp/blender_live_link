#pragma once

#include <cmath>

#include "game_object/projectile.h"
#include "state/state.h"

namespace WeaponSystem
{
	// Called after mech attachment and animation transforms have been updated.
	void update(f32 delta_seconds)
	{
		const i32 requests = state.input.pending_fire_requests;
		state.input.pending_fire_requests = 0;
		if (!state.runtime.is_simulating || state.debug_camera.active ||
			(state.gi.probe_isolation_enable && state.gi.show_probes) || !state.scene.player_character_id)
		{
			state.input.weapon_fire_held = false;
			return;
		}
		auto mech_id = state.mech.character_to_instance.find(*state.scene.player_character_id);
		if (mech_id == state.mech.character_to_instance.end()) return;
		auto mech = state.mech.instances.find(mech_id->second);
		if (mech == state.mech.instances.end()) return;
		for (MechWeaponInstance& equipped : mech->second.weapons)
		{
			// Idle time never accumulates a burst of overdue shots.
			const f32 cooldown = equipped.fire_cooldown_seconds > 0.0f ?
				equipped.fire_cooldown_seconds - std::max(0.0f, delta_seconds) : 0.0f;
			equipped.fire_cooldown_seconds = std::max(0.0f, cooldown);
			auto instance = state.scene.objects.find(equipped.instance_uid);
			auto templ = state.scene.objects.find(equipped.weapon_template_uid);
			if (instance == state.scene.objects.end() || !instance->second.visibility ||
				templ == state.scene.objects.end() || !templ->second.has_weapon ||
				!templ->second.weapon.muzzle_valid) continue;
			const f32 interval = templ->second.weapon.rate_of_fire_seconds;
			i32 shots = requests;
			if (std::isfinite(interval) && interval > 0.0f)
			{
				if ((!state.input.weapon_fire_held && requests <= 0) || cooldown > 0.0f) continue;
				// Keep the cadence across frame boundaries, limiting catch-up work
				// to the projectile cap for very small authored intervals.
				shots = requests > 0 ? 1 : (i32) std::min((f32) MAX_ACTIVE_PROJECTILES,
					1.0f + floorf(-cooldown / interval));
				equipped.fire_cooldown_seconds = requests > 0 ? interval :
					interval + fmodf(cooldown, interval);
			}
			else equipped.fire_cooldown_seconds = 0.0f;
			if (shots <= 0) continue;
			const HMM_Mat4 muzzle = HMM_MulM4(object_get_model_matrix(instance->second),
				templ->second.weapon.muzzle_local_transform);
			const HMM_Vec3 origin = HMM_V3(muzzle.Elements[3][0], muzzle.Elements[3][1], muzzle.Elements[3][2]);
			const HMM_Vec3 direction = HMM_V3(muzzle.Elements[1][0], muzzle.Elements[1][1], muzzle.Elements[1][2]);
			if (std::isfinite(origin.X) && std::isfinite(origin.Y) && std::isfinite(origin.Z))
				for (i32 shot = 0; shot < shots; ++shot) projectile_spawn(origin, direction);
		}
	}
}
