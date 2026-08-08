#ifndef DISPLAY_ENCODING_H
#define DISPLAY_ENCODING_H

#include "shader_common.h"

// Interleaved gradient noise from Jorge Jimenez's presentation:
// http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
float display_gradient_noise(vec2 noise_uv)
{
	return fract(52.9829189 * fract(dot(noise_uv, vec2(0.06711056, 0.00583715))));
}

vec3 display_linear_to_srgb(vec3 linear_color)
{
	linear_color = clamp(linear_color, vec3(0.0), vec3(1.0));
	vec3 lower = linear_color * 12.92;
	vec3 upper = 1.055 * pow(linear_color, vec3(1.0 / 2.4)) - 0.055;
	return mix(lower, upper, step(vec3(0.0031308), linear_color));
}

vec3 display_srgb_to_linear(vec3 srgb_color)
{
	srgb_color = clamp(srgb_color, vec3(0.0), vec3(1.0));
	vec3 lower = srgb_color / 12.92;
	vec3 upper = pow((srgb_color + 0.055) / 1.055, vec3(2.4));
	return mix(lower, upper, step(vec3(0.04045), srgb_color));
}

float display_st2084_from_nits(float nits)
{
	const float m1 = 0.1593017578125;
	const float m2 = 78.84375;
	const float c1 = 0.8359375;
	const float c2 = 18.8515625;
	const float c3 = 18.6875;
	float l = clamp(nits / 10000.0, 0.0, 1.0);
	float lm = pow(l, m1);
	return pow((c1 + c2 * lm) / (1.0 + c3 * lm), m2);
}

vec3 display_linear_srgb_to_rec2020(vec3 color)
{
	return vec3(
		dot(color, vec3(0.6274039, 0.3292830, 0.0433131)),
		dot(color, vec3(0.0690973, 0.9195404, 0.0113623)),
		dot(color, vec3(0.0163914, 0.0880133, 0.8955953)));
}

vec3 display_encode(
	vec3 scene,
	int output_mode,
	int sdr_attachment_is_srgb,
	vec2 pixel_coordinate)
{
	if (output_mode == DISPLAY_OUTPUT_MODE_PRESENTATION) return scene;
	if (output_mode == DISPLAY_OUTPUT_MODE_SDR)
	{
		vec3 encoded_color = display_linear_to_srgb(scene);
		encoded_color = clamp(
			encoded_color + vec3((display_gradient_noise(pixel_coordinate) - 0.5) / 255.0),
			vec3(0.0), vec3(1.0));
		return sdr_attachment_is_srgb != 0
			? display_srgb_to_linear(encoded_color)
			: encoded_color;
	}
	if (output_mode == DISPLAY_OUTPUT_MODE_EDR)
		return max(scene, vec3(0.0)) * (1000.0 / 203.0);

	vec3 rec2020_nits = max(display_linear_srgb_to_rec2020(scene), vec3(0.0)) * 1000.0;
	return vec3(
		display_st2084_from_nits(rec2020_nits.r),
		display_st2084_from_nits(rec2020_nits.g),
		display_st2084_from_nits(rec2020_nits.b));
}

#endif
