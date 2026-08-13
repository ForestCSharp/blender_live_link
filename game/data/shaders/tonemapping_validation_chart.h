#ifndef TONEMAPPING_VALIDATION_CHART_H
#define TONEMAPPING_VALIDATION_CHART_H

// Procedural scene-linear validation input. It deliberately spans values far
// below diffuse black through the LUT clamp and includes both color and spatial
// transitions needed by the local exposure-fusion path.
vec3 tonemapping_validation_chart(vec2 input_uv)
{
	vec2 uv = clamp(input_uv, vec2(0.0), vec2(1.0));
	float band_coordinate = min(uv.y * 8.0, 7.9999);
	int band = int(floor(band_coordinate));
	float y = fract(band_coordinate);
	float x = uv.x;
	if (band == 0)
	{
		float value = exp2(mix(-12.0, 6.0, x));
		return vec3(value);
	}
	if (band == 1)
	{
		float value = floor(x * 32.0) / 31.0 * 0.02;
		return vec3(value);
	}
	if (band == 2)
	{
		float ramp = exp2(mix(-6.0, 6.0, fract(x * 3.0)));
		int primary = min(int(x * 3.0), 2);
		return primary == 0 ? vec3(ramp, 0.0, 0.0)
			: primary == 1 ? vec3(0.0, ramp, 0.0) : vec3(0.0, 0.0, ramp);
	}
	if (band == 3)
	{
		float ramp = exp2(mix(-6.0, 6.0, fract(x * 3.0)));
		int secondary = min(int(x * 3.0), 2);
		return secondary == 0 ? vec3(ramp, ramp, 0.0)
			: secondary == 1 ? vec3(ramp, 0.0, ramp) : vec3(0.0, ramp, ramp);
	}
	if (band == 4)
	{
		const vec3 patches[8] = vec3[8](
			vec3(0.18), vec3(0.50, 0.18, 0.08), vec3(0.08, 0.35, 0.70),
			vec3(0.80, 0.50, 0.08), vec3(0.08, 0.80, 0.35),
			vec3(0.70, 0.08, 0.50), vec3(0.80), vec3(0.02));
		return patches[min(int(x * 8.0), 7)];
	}
	if (band == 5)
	{
		float peak = exp2(mix(0.0, 6.0, fract(x * 4.0)));
		int patch_index = min(int(x * 4.0), 3);
		return patch_index == 0 ? vec3(peak)
			: patch_index == 1 ? vec3(peak, peak * 0.25, peak * 0.04)
			: patch_index == 2 ? vec3(peak * 0.04, peak, peak * 0.25)
			: vec3(peak * 0.25, peak * 0.04, peak);
	}
	if (band == 6)
	{
		float checker = mod(floor(x * 24.0) + floor(y * 8.0), 2.0);
		float radial = smoothstep(0.32, 0.0, distance(vec2(x, y), vec2(0.5)));
		return vec3(mix(0.01, 32.0, max(checker, radial)));
	}
	return x < 1.0 / 3.0 ? vec3(0.18)
		: x < 2.0 / 3.0 ? vec3(1.0) : vec3(16.0);
}

// Synthetic sky/geometry coverage used by the full-pipeline validation. The
// skyline contains horizontal and diagonal sections, while the narrow column
// exercises geometry that cannot support a local-exposure neighborhood.
float tonemapping_validation_geometry_mask(vec2 input_uv)
{
	vec2 uv = clamp(input_uv, vec2(0.0), vec2(1.0));
	float horizon = uv.x < 0.28
		? 0.60 : 0.48 + 0.22 * uv.x;
	float ground = uv.y >= horizon ? 1.0 : 0.0;
	float thin_geometry = abs(uv.x - 0.52) < 0.006
		&& uv.y >= 0.20 && uv.y < horizon ? 1.0 : 0.0;
	return max(ground, thin_geometry);
}

vec3 tonemapping_validation_sky_geometry_chart(vec2 input_uv)
{
	vec2 uv = clamp(input_uv, vec2(0.0), vec2(1.0));
	float geometry = tonemapping_validation_geometry_mask(uv);
	if (geometry > 0.5)
	{
		float checker = mod(floor(uv.x * 12.0) + floor(uv.y * 10.0), 2.0);
		float value = exp2(mix(-4.5, 1.5, uv.x)) * mix(0.45, 1.0, checker);
		return value * mix(vec3(0.50, 0.58, 0.70), vec3(0.95, 0.42, 0.12), uv.x);
	}

	float horizon = uv.x < 0.28
		? 0.60 : 0.48 + 0.22 * uv.x;
	float horizon_glow = exp(-max(horizon - uv.y, 0.0) * 18.0);
	vec3 sky = mix(vec3(2.0, 5.0, 14.0), vec3(18.0, 11.0, 4.0), horizon_glow);
	// An intense compact highlight beside the diagonal silhouette stands in for
	// the analytic solar disc without involving the scene's actual sky pass.
	vec2 disc_center = vec2(0.72, 0.615);
	float disc = smoothstep(0.022, 0.014, distance(uv, disc_center));
	return sky + vec3(4096.0, 3072.0, 2048.0) * disc;
}

float tonemapping_validation_geometry_local_strength(vec2 uv, vec2 pixel_size)
{
	if (tonemapping_validation_geometry_mask(uv) <= 0.5)
	{
		return 0.0;
	}
	float geometry_count = 0.0;
	for (int y = -1; y <= 1; ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			geometry_count += tonemapping_validation_geometry_mask(
				uv + vec2(x, y) * pixel_size);
		}
	}
	return smoothstep(2.0 / 3.0, 1.0, geometry_count / 9.0);
}

#endif
