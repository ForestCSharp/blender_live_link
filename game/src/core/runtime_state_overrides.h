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
		if (config.tessellation) { in_state.tessellation.enabled = true; }
		if (config.gi_method == "sdf") { in_state.gi.method = EGiMethod::SdfLumen; }
		if (config.gi_method == "octree") { in_state.gi.method = EGiMethod::ProbeOctree; }
		if (config.sdf_gi_debug_view)
		{
			in_state.sdf_gi.debug_view = (ESdfGiDebugView) std::atoi(config.sdf_gi_debug_view->c_str());
		}
	}
}
