#ifndef TONEMAPPING_OPERATORS_H
#define TONEMAPPING_OPERATORS_H

const vec3 TONEMAP_LUMINANCE_WEIGHTS = vec3(0.2126, 0.7152, 0.0722);

const int TONEMAP_METHOD_GT7 = 0;
const int TONEMAP_METHOD_AGX = 1;
const int TONEMAP_METHOD_ACES_2 = 2;
const int TONEMAP_METHOD_KHRONOS_PBR_NEUTRAL = 3;

#ifndef PBR_NEUTRAL_MATCH_EXISTING_MIDDLE_GRAY
#define PBR_NEUTRAL_MATCH_EXISTING_MIDDLE_GRAY 0
#endif

const float GT7_LUT_RESOLUTION = 64.0;
const float GT7_LUT_INPUT_MAX = 64.0;
const float ACES2_LUT_RESOLUTION = 64.0;
const float ACES2_LUT_INPUT_MAX = 64.0;
const float ACES2_LUT_LAYER_OFFSET = 64.0;
const float AGX_LUT_RESOLUTION = 64.0;
const float AGX_LUT_INPUT_MAX = 64.0;
const float AGX_LUT_LAYER_OFFSET = 128.0;

vec3 sample_tonemapping_lut(
	sampler2DArray tonemapping_lut,
	vec3 lut_position,
	float resolution,
	float layer_offset)
{
	vec2 lut_uv = (lut_position.xy + vec2(0.5)) / resolution;
	float low_layer = floor(lut_position.z);
	float high_layer = min(low_layer + 1.0, resolution - 1.0);
	vec3 low = texture(tonemapping_lut, vec3(lut_uv, layer_offset + low_layer)).rgb;
	vec3 high = texture(tonemapping_lut, vec3(lut_uv, layer_offset + high_layer)).rgb;
	return mix(low, high, lut_position.z - low_layer);
}

vec3 tonemap_gt7(sampler2DArray tonemapping_lut, vec3 color, float integration_scale)
{
	vec3 calibrated = clamp(max(color, vec3(0.0)) * integration_scale,
		vec3(0.0), vec3(GT7_LUT_INPUT_MAX));
	vec3 shaped = pow(calibrated / GT7_LUT_INPUT_MAX, vec3(0.25));
	vec3 lut_position = shaped * (GT7_LUT_RESOLUTION - 1.0);
	return clamp(sample_tonemapping_lut(
		tonemapping_lut, lut_position, GT7_LUT_RESOLUTION, 0.0), 0.0, 1.0);
}

float aces2_linear_to_acescct(float value)
{
	return value <= 0.0078125
		? 10.5402377416545 * value + 0.0729055341958355
		: (log2(value) + 9.72) / 17.52;
}

vec3 tonemap_aces2(sampler2DArray tonemapping_lut, vec3 color, float integration_scale)
{
	vec3 calibrated = clamp(max(color, vec3(0.0)) * integration_scale,
		vec3(0.0), vec3(ACES2_LUT_INPUT_MAX));
	const float shaper_min = 0.0729055341958355;
	const float shaper_max = (log2(ACES2_LUT_INPUT_MAX) + 9.72) / 17.52;
	vec3 shaped = vec3(
		aces2_linear_to_acescct(calibrated.r),
		aces2_linear_to_acescct(calibrated.g),
		aces2_linear_to_acescct(calibrated.b));
	shaped = (shaped - shaper_min) / (shaper_max - shaper_min);
	vec3 lut_position = shaped * (ACES2_LUT_RESOLUTION - 1.0);
	// Wide-gamut LUT values are stored in the linear-sRGB composite basis and may
	// be negative or exceed one while still lying inside the Rec.2020 target.
	return sample_tonemapping_lut(
		tonemapping_lut, lut_position, ACES2_LUT_RESOLUTION, ACES2_LUT_LAYER_OFFSET);
}

vec3 tonemap_agx(sampler2DArray tonemapping_lut, vec3 color, float integration_scale)
{
	vec3 calibrated = clamp(max(color, vec3(0.0)) * integration_scale,
		vec3(0.0), vec3(AGX_LUT_INPUT_MAX));
	const float shaper_min = 0.0729055341958355;
	const float shaper_max = (log2(AGX_LUT_INPUT_MAX) + 9.72) / 17.52;
	vec3 shaped = vec3(
		aces2_linear_to_acescct(calibrated.r),
		aces2_linear_to_acescct(calibrated.g),
		aces2_linear_to_acescct(calibrated.b));
	shaped = (shaped - shaper_min) / (shaper_max - shaper_min);
	vec3 lut_position = shaped * (AGX_LUT_RESOLUTION - 1.0);
	return sample_tonemapping_lut(
		tonemapping_lut, lut_position, AGX_LUT_RESOLUTION, AGX_LUT_LAYER_OFFSET);
}

// Khronos PBR Neutral Tone Mapper, pinned to reference commit
// f5dc101149fc5c85c0f9852fe2ba438853e8a7d1 and adapted to clamp negative
// renderer input and support an optional integration exposure. Copyright 2024
// The Khronos Group, Inc.; licensed under Apache-2.0. See
// LICENSES/Khronos-ToneMapping-Apache-2.0.txt.
// https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral
vec3 tonemap_khronos_pbr_neutral(vec3 color)
{
	color = max(color, vec3(0.0));
#if PBR_NEUTRAL_MATCH_EXISTING_MIDDLE_GRAY
	// Opt-in integration calibration: exact Khronos behavior remains the
	// default, while this maps neutral 18% gray from 0.14 to approximately
	// the existing GT7/AgX target of 0.214519.
	color = min(color, vec3(2.4065324e38)) * 1.4139944;
#endif

	const float start_compression = 0.8 - 0.04;
	const float desaturation = 0.15;

	float x = min(color.r, min(color.g, color.b));
	float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
	color -= offset;

	float peak = max(color.r, max(color.g, color.b));
	if (peak < start_compression)
		return color;

	const float d = 1.0 - start_compression;
	float new_peak = 1.0 - d * d / (peak + d - start_compression);
	color *= new_peak / peak;

	float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
	return mix(color, new_peak * vec3(1.0), g);
}

vec3 tonemap_apply(int method, sampler2DArray tonemapping_lut, vec3 color, float lut_integration_scale)
{
	if (method == TONEMAP_METHOD_AGX)
		return tonemap_agx(tonemapping_lut, color, lut_integration_scale);
	if (method == TONEMAP_METHOD_ACES_2)
		return tonemap_aces2(tonemapping_lut, color, lut_integration_scale);
	if (method == TONEMAP_METHOD_KHRONOS_PBR_NEUTRAL)
		return tonemap_khronos_pbr_neutral(color);
	return tonemap_gt7(tonemapping_lut, color, lut_integration_scale);
}

float tonemap_perceptual_lightness(int method, vec3 tonemapped_color)
{
	float luminance = method == TONEMAP_METHOD_ACES_2 || method == TONEMAP_METHOD_AGX
		? clamp(dot(tonemapped_color, TONEMAP_LUMINANCE_WEIGHTS), 0.0, 1.0)
		: dot(clamp(tonemapped_color, 0.0, 1.0), TONEMAP_LUMINANCE_WEIGHTS);
	return sqrt(max(luminance, 0.0));
}

#endif
