#version 450

#include "shader_common.h"

layout(set = 0, binding = 0) uniform sampler2D scene_color;

layout(push_constant) uniform PushConstants
{
	int output_mode; // One of DISPLAY_OUTPUT_MODE_* from shader_common.h.
	int sdr_attachment_is_srgb;
} pc;

layout(location = 0) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

// Interleaved gradient noise from Jorge Jimenez's presentation:
// http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
float gradient_noise(vec2 noise_uv)
{
	return fract(52.9829189 * fract(dot(noise_uv, vec2(0.06711056, 0.00583715))));
}

vec3 linear_to_srgb(vec3 linear_color)
{
	linear_color = clamp(linear_color, vec3(0.0), vec3(1.0));
	vec3 lower = linear_color * 12.92;
	vec3 upper = 1.055 * pow(linear_color, vec3(1.0 / 2.4)) - 0.055;
	return mix(lower, upper, step(vec3(0.0031308), linear_color));
}

vec3 srgb_to_linear(vec3 srgb_color)
{
	srgb_color = clamp(srgb_color, vec3(0.0), vec3(1.0));
	vec3 lower = srgb_color / 12.92;
	vec3 upper = pow((srgb_color + 0.055) / 1.055, vec3(2.4));
	return mix(lower, upper, step(vec3(0.04045), srgb_color));
}

float st2084_from_nits(float nits)
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

vec3 linear_srgb_to_rec2020(vec3 color)
{
	return vec3(
		dot(color, vec3(0.6274039, 0.3292830, 0.0433131)),
		dot(color, vec3(0.0690973, 0.9195404, 0.0113623)),
		dot(color, vec3(0.0163914, 0.0880133, 0.8955953)));
}

void main()
{
	vec3 scene = texture(scene_color, in_uv).rgb;
	if (pc.output_mode == DISPLAY_OUTPUT_MODE_PRESENTATION)
	{
		out_color = vec4(scene, 1.0);
		return;
	}

	if (pc.output_mode == DISPLAY_OUTPUT_MODE_SDR)
	{
		// Dither once at the 8-bit display boundary. Work in sRGB code space so
		// the noise spans one stored code regardless of scene brightness.
		vec3 encoded_color = linear_to_srgb(scene);
		encoded_color = clamp(
			encoded_color + vec3((gradient_noise(gl_FragCoord.xy) - 0.5) / 255.0),
			vec3(0.0), vec3(1.0));

		// An sRGB attachment encodes shader output in hardware. The UNORM SDR
		// fallback stores shader output directly but uses the same display curve.
		vec3 attachment_color = pc.sdr_attachment_is_srgb != 0
			? srgb_to_linear(encoded_color)
			: encoded_color;
		out_color = vec4(attachment_color, 1.0);
		return;
	}

	if (pc.output_mode == DISPLAY_OUTPUT_MODE_EDR)
	{
		// Extended-linear sRGB defines 1.0 as diffuse white. The composite uses
		// 1.0 as 1000 nits, so scale it relative to the 203-nit paper white.
		out_color = vec4(max(scene, vec3(0.0)) * (1000.0 / 203.0), 1.0);
		return;
	}

	// HDR10 stores nonlinear PQ code values in a Rec.2020 container.
	vec3 rec2020_nits = max(
		linear_srgb_to_rec2020(max(scene, vec3(0.0))), vec3(0.0)) * 1000.0;
	out_color = vec4(
		st2084_from_nits(rec2020_nits.r),
		st2084_from_nits(rec2020_nits.g),
		st2084_from_nits(rec2020_nits.b),
		1.0);
}
