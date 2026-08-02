#pragma once

#include <cstdio>
#include <limits>
#include <optional>

#include "state/state.h"

namespace SceneSystem
{
	// Keeps state.scene.primary_sun_id pointing at a valid sun object,
	// rescanning the light index when the cached id goes stale.
	void refresh_primary_sun_id(State& in_state)
	{
		if (in_state.scene.primary_sun_id)
		{
			auto found = in_state.scene.objects.find(*in_state.scene.primary_sun_id);
			if (found != in_state.scene.objects.end() && object_is_sun_light(found->second))
			{
				return;
			}
			in_state.scene.primary_sun_id.reset();
		}

		i32 selected_uid = std::numeric_limits<i32>::max();
		for (i32 light_object_id : in_state.scene.indexes.light_object_ids)
		{
			auto found = in_state.scene.objects.find(light_object_id);
			if (found != in_state.scene.objects.end() && object_is_sun_light(found->second))
			{
				selected_uid = MIN(selected_uid, light_object_id);
			}
		}
		if (selected_uid != std::numeric_limits<i32>::max())
		{
			in_state.scene.primary_sun_id = selected_uid;
		}
	}

	// Picks the lowest-uid enabled and visible fog controller. Logging is kept
	// edge-triggered so scene refreshes do not spam an unchanged selection.
	void refresh_active_fog_controller(State& in_state)
	{
		const std::optional<i32> previous_id = in_state.fog.active_fog_controller_id;

		in_state.fog.active_fog_controller_id.reset();
		in_state.fog.active = false;

		i32 selected_uid = std::numeric_limits<i32>::max();
		for (auto& [unique_id, object] : in_state.scene.objects)
		{
			if (!object.has_fog_controller || !object.fog_controller.enabled || !object.visibility)
			{
				continue;
			}

			if (unique_id < selected_uid)
			{
				selected_uid = unique_id;
				in_state.fog.active_fog_controller_id = unique_id;
				in_state.fog.active = true;
			}
		}

		if (in_state.fog.active_fog_controller_id != previous_id)
		{
			if (in_state.fog.active_fog_controller_id)
			{
				printf("Active fog controller: UID %i\n", *in_state.fog.active_fog_controller_id);
			}
			else
			{
				printf("Active fog controller: none\n");
			}
		}
	}

	// Refreshes scene-derived selections after transforms and imports have been
	// applied, before lighting and rendering consume them.
	void refresh_derived_state(State& in_state)
	{
		scene_ensure_indexes(in_state);
		refresh_primary_sun_id(in_state);
		refresh_active_fog_controller(in_state);
	}
}
