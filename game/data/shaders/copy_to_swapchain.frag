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

vec3 encode_boundary_value(vec3 value, bool hdr10)
{
	if (!hdr10)
		return value; // Extended-linear sRGB, where 1.0 is SDR white.
	return vec3(
		st2084_from_nits(value.r),
		st2084_from_nits(value.g),
		st2084_from_nits(value.b));
}

vec3 chart_primary(int index)
{
	if (index == 0) return vec3(1.0, 0.0, 0.0);
	if (index == 1) return vec3(0.0, 1.0, 0.0);
	if (index == 2) return vec3(0.0, 0.0, 1.0);
	if (index == 3) return vec3(1.0, 1.0, 0.0);
	if (index == 4) return vec3(0.0, 1.0, 1.0);
	return vec3(1.0, 0.0, 1.0);
}

vec3 display_boundary_chart(vec2 uv, bool hdr10)
{
	// In HDR10 mode the values below are BT.2020-primary nits. In EDR mode
	// they are extended-linear sRGB multiples of SDR white.
	if (uv.y < 0.22)
	{
		float peak = hdr10 ? 1000.0 : 8.0;
		return encode_boundary_value(vec3(uv.x * peak), hdr10);
	}
	if (uv.y < 0.38)
	{
		float step_index = floor(clamp(uv.x, 0.0, 0.99999) * 16.0);
		float near_black = step_index / 15.0 * (hdr10 ? 5.0 : 0.05);
		return encode_boundary_value(vec3(near_black), hdr10);
	}
	int primary_index = int(floor(clamp(uv.x, 0.0, 0.99999) * 6.0));
	float within_patch = fract(uv.x * 6.0);
	if (uv.y < 0.65)
	{
		float peak = hdr10 ? 400.0 : 4.0;
		return encode_boundary_value(chart_primary(primary_index) * within_patch * peak, hdr10);
	}
	if (uv.y < 0.82)
	{
		float peak = hdr10 ? 1000.0 : 8.0;
		return encode_boundary_value(chart_primary(primary_index) * peak, hdr10);
	}

	const float levels[4] = float[4](100.0, 203.0, 400.0, 1000.0);
	int level_index = int(floor(clamp(uv.x, 0.0, 0.99999) * 4.0));
	float value = hdr10 ? levels[level_index] : levels[level_index] / 100.0;
	return encode_boundary_value(vec3(value), hdr10);
}

void main()
{
	if (pc.output_mode == DISPLAY_OUTPUT_MODE_SDR)
	{
		// Dither once at the 8-bit display boundary. Work in sRGB code space so
		// the noise spans one stored code regardless of scene brightness.
		vec3 encoded_color = linear_to_srgb(texture(scene_color, in_uv).rgb);
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

	// Experimental modes deliberately replace the normal renderer at the
	// display boundary. This isolates surface/colorspace correctness from the
	// production SDR renderer and GT7 operator.
	out_color = vec4(
		display_boundary_chart(in_uv, pc.output_mode == DISPLAY_OUTPUT_MODE_HDR10),
		1.0);
}
