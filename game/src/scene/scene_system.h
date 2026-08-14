#pragma once

#include <cstdio>
#include <limits>
#include <optional>

#include "state/state.h"

namespace SceneSystem
{
	void refresh_active_sky_controller(State& in_state)
	{
		const std::optional<i32> previous_id = in_state.scene.active_sky_controller_id;
		const i32 previous_candidate_count = in_state.scene.sky_controller_candidate_count;
		const i32 previous_invalid_count = in_state.scene.invalid_sky_controller_count;
		in_state.scene.active_sky_controller_id.reset();

		i32 selected_uid = std::numeric_limits<i32>::max();
		i32 candidate_count = 0;
		i32 invalid_count = 0;
		for (auto& [unique_id, object] : in_state.scene.objects)
		{
			if (!object.has_sky_atmosphere)
			{
				continue;
			}
			if (!object_is_sun_light(object))
			{
				if (object.sky_atmosphere.enabled) ++invalid_count;
				continue;
			}
			if (!object.sky_atmosphere.enabled || !object.visibility)
			{
				continue;
			}
			++candidate_count;
			selected_uid = MIN(selected_uid, unique_id);
		}
		if (selected_uid != std::numeric_limits<i32>::max())
		{
			in_state.scene.active_sky_controller_id = selected_uid;
		}
		in_state.scene.sky_controller_candidate_count = candidate_count;
		in_state.scene.invalid_sky_controller_count = invalid_count;

		if (previous_id != in_state.scene.active_sky_controller_id)
		{
			if (in_state.scene.active_sky_controller_id)
			{
				printf("Active sky controller: UID %i%s\n",
					*in_state.scene.active_sky_controller_id,
					candidate_count > 1 ? " (lowest UID selected from multiple controllers)" : "");
			}
			else
			{
				printf("Active sky controller: none; procedural sky disabled\n");
			}
			mark_lighting_dirty(in_state);
			in_state.shadow.force_recapture = true;
			if (in_state.gi.render_sky_to_probes) in_state.gi.is_updating = true;
		}
		if (candidate_count > 1 && candidate_count != previous_candidate_count)
		{
			printf("Sky atmosphere warning: %i active controllers; lowest UID wins\n", candidate_count);
		}
		if (invalid_count > 0 && invalid_count != previous_invalid_count)
		{
			printf("Sky atmosphere warning: ignored %i controller(s) not attached to a Sun\n", invalid_count);
		}
	}

	// Keeps state.scene.primary_sun_id pointing at a valid sun object,
	// rescanning the light index when the cached id goes stale.
	void refresh_primary_sun_id(State& in_state)
	{
		if (in_state.scene.active_sky_controller_id)
		{
			in_state.scene.primary_sun_id = in_state.scene.active_sky_controller_id;
			return;
		}
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

	void refresh_active_cloud_controller(State& in_state)
	{
		const std::optional<i32> previous_id = in_state.scene.active_cloud_controller_id;
		const i32 previous_invalid_count = in_state.scene.invalid_cloud_controller_count;
		in_state.scene.active_cloud_controller_id.reset();
		in_state.scene.invalid_cloud_controller_count = 0;
		in_state.clouds.active = false;
		in_state.clouds.active_layer_count = 0;

		for (auto& [unique_id, object] : in_state.scene.objects)
		{
			if (!object.has_cloud_system)
			{
				continue;
			}
			if (!object.has_sky_atmosphere || !object_is_sun_light(object))
			{
				if (object.cloud_system.enabled) ++in_state.scene.invalid_cloud_controller_count;
				continue;
			}
			if (!object.cloud_system.enabled || !object.sky_atmosphere.enabled || !object.visibility)
			{
				continue;
			}
			if (in_state.scene.active_sky_controller_id == unique_id)
			{
				in_state.scene.active_cloud_controller_id = unique_id;
				in_state.clouds.active = true;
				for (i32 layer_index = 0; layer_index < object.cloud_system.layer_count; ++layer_index)
				{
					if (object.cloud_system.layers[layer_index].enabled)
						++in_state.clouds.active_layer_count;
				}
				in_state.clouds.layer_budget_warning = in_state.clouds.active_layer_count > 2;
				if (in_state.clouds.active_layer_count == 0)
				{
					in_state.scene.active_cloud_controller_id.reset();
					in_state.clouds.active = false;
				}
			}
			else
			{
				// Valid-looking Cloud Systems on non-selected atmosphere Suns are
				// duplicate global controllers and remain non-rendering.
				++in_state.scene.invalid_cloud_controller_count;
			}
		}

		if (previous_id != in_state.scene.active_cloud_controller_id)
		{
			in_state.clouds.history_reset_requested = true;
			if (in_state.scene.active_cloud_controller_id)
				printf("Active cloud controller: UID %i (%i layer%s)\n",
					*in_state.scene.active_cloud_controller_id,
					in_state.clouds.active_layer_count,
					in_state.clouds.active_layer_count == 1 ? "" : "s");
			else
				printf("Active cloud controller: none\n");
		}
		if (in_state.scene.invalid_cloud_controller_count > 0
			&& in_state.scene.invalid_cloud_controller_count != previous_invalid_count)
		{
			printf("Cloud system warning: ignored %i invalid or duplicate controller(s)\n",
				in_state.scene.invalid_cloud_controller_count);
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
		refresh_active_sky_controller(in_state);
		refresh_primary_sun_id(in_state);
		refresh_active_cloud_controller(in_state);
		refresh_active_fog_controller(in_state);
	}
}
