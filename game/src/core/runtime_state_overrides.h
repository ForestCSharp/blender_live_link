#pragma once

#include "core/runtime_config.h"
#include "state/state.h"

namespace RuntimeStateOverrides
{
	inline void apply(State& in_state)
	{
		const RuntimeConfig::Config& config = RuntimeConfig::get();
		if (config.render_scale)
		{
			in_state.window.resolution_percentage = (i32) *config.render_scale;
		}
		if (config.hide_ui) { in_state.debug_ui.visible = false; }
		if (config.wireframe) { in_state.wireframe.shaded_wireframe = true; }
	}
}
