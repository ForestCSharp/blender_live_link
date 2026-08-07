#version 450

#include "shader_common.h"

layout(set = 0, binding = 0) uniform sampler2D scene_color;

layout(push_constant) uniform PushConstants
{
	int output_mode; // One of DISPLAY_OUTPUT_MODE_* from shader_common.h.
} pc;

layout(location = 0) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

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
		// Linear scene color; hardware sRGB-encodes on the swapchain-view write.
		// Tonemapping has already produced this LDR input in its own pass.
		out_color = texture(scene_color, in_uv);
		return;
	}

	// Experimental modes deliberately replace the normal renderer at the
	// display boundary. This isolates surface/colorspace correctness from the
	// production SDR renderer and GT7 operator.
	out_color = vec4(
		display_boundary_chart(in_uv, pc.output_mode == DISPLAY_OUTPUT_MODE_HDR10),
		1.0);
}
