#pragma once

#include "core/runtime_config.h"
#include "render/tessellation.h"
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
		if (config.shadow_placement)
		{
			in_state.shadow.cascade_placement_mode = *config.shadow_placement == 1
				? EShadowCascadePlacementMode::CenteredSquares
				: EShadowCascadePlacementMode::Frustum;
		}
		if (config.shadow_cascade_debug) { in_state.shadow.debug_show_cascade_selection = true; }
		if (config.hide_ui) { in_state.debug_ui.visible = false; }
		if (config.ssao) { in_state.ssao.enable = *config.ssao; }
		if (config.dof) { in_state.dof.enable = *config.dof; }
		if (config.dof_focus) { in_state.dof.focus_distance = (f32) *config.dof_focus; }
		if (config.dof_range) { in_state.dof.focus_range = (f32) *config.dof_range; }
		if (config.dof_debug) { in_state.dof.debug_show_coc = true; }
		if (config.wireframe) { in_state.wireframe.shaded_wireframe = true; }
		if (config.taa) { in_state.temporal_aa.enable = *config.taa; }
		if (config.fxaa) { in_state.temporal_aa.enable_fxaa = *config.fxaa; }
		if (config.tonemap_mode)
		{
			// Preserve the historical environment interface: named methods were
			// global, while "local" selected the old GT7-backed local default.
			in_state.tonemapping.local_enabled = false;
			if (*config.tonemap_mode == "gt7") { in_state.tonemapping.method = ETonemappingMethod::GT7; }
			else if (*config.tonemap_mode == "agx") { in_state.tonemapping.method = ETonemappingMethod::AgX; }
			else if (*config.tonemap_mode == "aces") { in_state.tonemapping.method = ETonemappingMethod::Aces2; }
			else if (*config.tonemap_mode == "neutral") { in_state.tonemapping.method = ETonemappingMethod::KhronosPBRNeutral; }
			else
			{
				in_state.tonemapping.method = ETonemappingMethod::GT7;
				in_state.tonemapping.local_enabled = true;
			}
		}
		// The explicit independent control always wins over the legacy mode's
		// implied global/local state.
		if (config.local_tonemap)
			in_state.tonemapping.local_enabled = *config.local_tonemap;
		printf("Tonemapping: method %s, local %s\n",
			ETonemappingMethodNames[(i32)in_state.tonemapping.method],
			in_state.tonemapping.local_enabled ? "enabled" : "disabled");
		if (config.bloom) { in_state.bloom.enable = *config.bloom; }
		if (config.bloom_threshold)
		{
			in_state.bloom.threshold = CLAMP((f32)*config.bloom_threshold, 0.0f, 10.0f);
		}
		if (config.bloom_soft_knee)
		{
			in_state.bloom.soft_knee = CLAMP((f32)*config.bloom_soft_knee, 0.0f, 1.0f);
		}
		if (config.bloom_intensity)
		{
			in_state.bloom.intensity = CLAMP(
				(f32)*config.bloom_intensity, 0.0f, State::BloomState::MAX_INTENSITY);
		}
		if (config.bloom_mips)
		{
			in_state.bloom.requested_mip_count = CLAMP((i32)*config.bloom_mips, 1, 8);
		}
		if (config.tessellation) { in_state.tessellation.enabled = *config.tessellation; }
		if (config.tessellation_mode)
		{
			in_state.tessellation.mode = (ETessellationMode) CLAMP((i32) *config.tessellation_mode, 0, 2);
		}
		if (config.tessellation_factor)
		{
			in_state.tessellation.fixed_factor = CLAMP((i32) *config.tessellation_factor, 1, (i32) Tessellation::MAX_FACTOR);
		}
		if (config.gi_probes) { in_state.gi.show_probes = true; }
		if (config.gi_radiance_mode)
		{
			in_state.gi.probe_radiance_mode = (EProbeRadianceMode) CLAMP((i32) *config.gi_radiance_mode, 0, 2);
		}
		if (config.gi_occlusion_mode)
		{
			in_state.gi.probe_occlusion_mode = (EProbeOcclusionMode) CLAMP((i32) *config.gi_occlusion_mode, 0, 1);
		}
		if (config.gi_specular) { in_state.gi.probe_specular_enable = *config.gi_specular; }
	}
}
