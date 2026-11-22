#ifndef SDF_SCREEN_TRACE_H
#define SDF_SCREEN_TRACE_H

// Screen-space march against the G-buffer, shared by the SDF GI probe rays and
// reflections. The hit's colour is last frame's TAA output there: lit, with
// sub-voxel detail and objects the SDF lacks (skinned meshes). Callers continue
// in the SDF from t_clear on a miss.
// shortcut: the TAA output includes fog, so hits carry a little of it; use a
// pre-fog history if fogged scenes show haze in GI or reflections.
// Requires sdf_common.h with SDF_PARAMS_SET (view_projection, prev_view_projection).

bool sdf_project_uv(mat4 view_projection, vec3 p, out vec2 uv, out float view_depth)
{
	vec4 clip = view_projection * vec4(p, 1.0);
	view_depth = clip.w;
	if (clip.w <= 1e-4) return false;
	vec2 ndc = clip.xy / clip.w;
	uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);	// temporal_aa.frag convention
	return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)));
}

// Returns the hit's confidence (0 = no usable hit; fades at the screen edge).
// t_clear: how far the ray stayed in front of the depth buffer (where an SDF
// continuation can start without skipping anything the screen saw).
float sdf_screen_trace(sampler2D gbuffer_position_tex, sampler2D gbuffer_normal_tex, sampler2D previous_color_tex,
	vec3 origin, vec3 dir, float max_distance, int steps, float jitter, out vec3 radiance, out float t_clear)
{
	radiance = vec3(0.0);
	t_clear = 0.0;
	float t_previous = 0.0;
	for (int i = 0; i < steps; ++i)
	{
		float s = (float(i) + jitter) / float(steps);
		float t = max_distance * s * s;	// dense near the surface, where SDF voxels are too coarse
		vec2 uv;
		float ray_depth;
		if (!sdf_project_uv(sdf.view_projection, origin + dir * t, uv, ray_depth)) return 0.0;
		vec4 surface = textureLod(gbuffer_position_tex, uv, 0.0);
		if (surface.w < 0.5) { t_previous = t; t_clear = t; continue; }	// sky behind this step
		float behind = ray_depth - (sdf.view_projection * vec4(surface.xyz, 1.0)).w;
		if (behind > 0.0)
		{
			float thickness = max(0.1, 2.0 * (t - t_previous)) + 0.01 * ray_depth;
			// Passed behind something thick: unknown what is there, let the SDF answer.
			if (behind > thickness) return 0.0;
			vec3 surface_normal = textureLod(gbuffer_normal_tex, uv, 0.0).xyz;
			if (dot(surface_normal, dir) > 0.0) return 0.0;	// back face
			vec2 previous_uv;
			float previous_depth;
			if (!sdf_project_uv(sdf.prev_view_projection, surface.xyz, previous_uv, previous_depth)) return 0.0;
			radiance = textureLod(previous_color_tex, previous_uv, 0.0).rgb;
			float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
			return smoothstep(0.0, 0.08, edge);
		}
		t_previous = t;
		t_clear = t;
	}
	return 0.0;
}

#endif // SDF_SCREEN_TRACE_H
