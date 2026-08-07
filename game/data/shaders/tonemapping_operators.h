#ifndef TONEMAPPING_OPERATORS_H
#define TONEMAPPING_OPERATORS_H

const vec3 TONEMAP_LUMINANCE_WEIGHTS = vec3(0.2126, 0.7152, 0.0722);

const int TONEMAP_METHOD_GT7 = 0;
const int TONEMAP_METHOD_AGX = 1;
const int TONEMAP_METHOD_ACES_FITTED = 2;
const int TONEMAP_METHOD_NEUTRAL_HDR = 3;

const float GT7_LUT_RESOLUTION = 64.0;
const float GT7_LUT_INPUT_MAX = 64.0;
// The renderer's scene units are not photometrically calibrated. This fixed
// integration exposure matches 18% gray to the previous AgX-backed default.
const float GT7_INTEGRATION_SCALE = 2.978276;

vec3 tonemap_gt7(sampler2DArray gt7_lut, vec3 color)
{
	vec3 calibrated = clamp(max(color, vec3(0.0)) * GT7_INTEGRATION_SCALE,
		vec3(0.0), vec3(GT7_LUT_INPUT_MAX));
	vec3 shaped = pow(calibrated / GT7_LUT_INPUT_MAX, vec3(0.25));
	vec3 lut_position = shaped * (GT7_LUT_RESOLUTION - 1.0);
	vec2 lut_uv = (lut_position.xy + vec2(0.5)) / GT7_LUT_RESOLUTION;
	float low_layer = floor(lut_position.z);
	float high_layer = min(low_layer + 1.0, GT7_LUT_RESOLUTION - 1.0);
	vec3 low = texture(gt7_lut, vec3(lut_uv, low_layer)).rgb;
	vec3 high = texture(gt7_lut, vec3(lut_uv, high_layer)).rgb;
	return clamp(mix(low, high, lut_position.z - low_layer), 0.0, 1.0);
}

vec3 tonemap_neutral_hdr(vec3 color)
{
	color = max(color, vec3(0.0));
	float input_peak = max(color.r, max(color.g, color.b));
	if (input_peak <= 1e-8)
		return vec3(0.0);
	// Normalizing first avoids overflow while evaluating luminance for extreme
	// but still finite scene values.
	float luminance = input_peak
		* dot(color / input_peak, TONEMAP_LUMINANCE_WEIGHTS);
	if (luminance <= 1e-8)
		return vec3(0.0);

	// Calibrated so neutral 18% gray maps to the GT7/AgX integration target
	// of approximately 0.214519. Applying one multiplier to all channels keeps
	// the input RGB ratios intact through the luminance shoulder.
	const float exposure_scale = 1.5172523;
	float exposed_luminance = luminance * exposure_scale;
	float mapped_luminance = 1.0 - 1.0 / (1.0 + exposed_luminance);
	vec3 mapped = color * (mapped_luminance / luminance);

	// A common peak normalization keeps saturated HDR colors bounded without
	// introducing per-channel clipping or hue shifts.
	float peak = max(mapped.r, max(mapped.g, mapped.b));
	if (peak > 1.0)
		mapped /= peak;
	return clamp(mapped, 0.0, 1.0);
}

vec3 tonemap_aces_fitted(vec3 color)
{
	color = max(color, vec3(0.0));
	const float a = 2.51;
	const float b = 0.03;
	const float c = 2.43;
	const float d = 0.59;
	const float e = 0.14;
	return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 agx_default_contrast(vec3 x)
{
	vec3 x2 = x * x;
	vec3 x4 = x2 * x2;
	return 15.5 * x4 * x2
		- 40.14 * x4 * x
		+ 31.96 * x4
		- 6.868 * x2 * x
		+ 0.4298 * x2
		+ 0.1191 * x
		- 0.00232;
}

vec3 tonemap_agx(vec3 color)
{
	// Linear sRGB -> linear Rec.2020.
	const mat3 srgb_to_rec2020 = mat3(
		vec3(0.6274, 0.0691, 0.0164),
		vec3(0.3293, 0.9195, 0.0880),
		vec3(0.0433, 0.0113, 0.8956)
	);
	const mat3 rec2020_to_srgb = mat3(
		vec3(1.6605, -0.1246, -0.0182),
		vec3(-0.5876, 1.1329, -0.1006),
		vec3(-0.0728, -0.0083, 1.1187)
	);
	const mat3 inset = mat3(
		vec3(0.856627153315983, 0.137318972929847, 0.111898212999950),
		vec3(0.095121240538159, 0.761241990602591, 0.076799418603190),
		vec3(0.048251606145858, 0.101439036467562, 0.811302368396859)
	);
	const mat3 outset = mat3(
		vec3(1.127100581814437, -0.141329763498438, -0.141329763498438),
		vec3(-0.110606643096603, 1.157823702216272, -0.110606643096603),
		vec3(-0.016493938717835, -0.016493938717834, 1.251936406595041)
	);

	vec3 value = inset * (srgb_to_rec2020 * max(color, vec3(0.0)));
	value = log2(max(value, vec3(1e-10)));
	const float min_ev = -12.47393;
	const float max_ev = 4.026069;
	value = clamp((value - min_ev) / (max_ev - min_ev), 0.0, 1.0);
	value = agx_default_contrast(value);
	value = outset * value;
	value = pow(max(value, vec3(0.0)), vec3(2.2));
	return clamp(rec2020_to_srgb * value, 0.0, 1.0);
}

vec3 tonemap_apply(int method, sampler2DArray gt7_lut, vec3 color)
{
	if (method == TONEMAP_METHOD_AGX)
		return tonemap_agx(color);
	if (method == TONEMAP_METHOD_ACES_FITTED)
		return tonemap_aces_fitted(color);
	if (method == TONEMAP_METHOD_NEUTRAL_HDR)
		return tonemap_neutral_hdr(color);
	return tonemap_gt7(gt7_lut, color);
}

float tonemap_perceptual_lightness(vec3 tonemapped_color)
{
	return sqrt(max(dot(clamp(tonemapped_color, 0.0, 1.0), TONEMAP_LUMINANCE_WEIGHTS), 0.0));
}

#endif
