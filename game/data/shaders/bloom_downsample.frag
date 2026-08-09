#version 450

#include "auto_adaptation.h"

layout(set = 0, binding = 0) uniform sampler2D source_tex;
layout(std430, set = 0, binding = 1) readonly buffer AutoAdaptationStateBlock
{
	vec4 auto_adaptation_values[AUTO_ADAPTATION_STATE_VEC4_COUNT];
};

layout(push_constant) uniform PushConstants
{
	vec2 source_pixel_size;
	float threshold;
	float soft_knee;
	float exposure_scale;
	int apply_threshold;
	int auto_exposure_enabled;
	int auto_white_balance_enabled;
	float auto_exposure_influence;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag_color;

const vec3 LUMINANCE_WEIGHTS = vec3(0.2126, 0.7152, 0.0722);

float luminance(vec3 color)
{
	return max(dot(max(color, vec3(0.0)), LUMINANCE_WEIGHTS), 0.0);
}

vec3 threshold_color(vec3 raw_color)
{
	vec3 result = raw_color;
	if (pc.auto_white_balance_enabled != 0)
	{
		result = auto_adaptation_apply_white_balance(
			result,
			auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_0],
			auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_1],
			auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_2]);
	}
	float auto_exposure_ev = pc.auto_exposure_enabled != 0
		? auto_adaptation_values[AUTO_ADAPTATION_STATE_EXPOSURE_WHITE].x
		: 0.0;
	return result * pc.exposure_scale
		* exp2(auto_exposure_ev * clamp(pc.auto_exposure_influence, 0.0, 1.0));
}

vec3 downsample_13_tap(vec2 sample_uv)
{
	const vec2 offsets[13] = vec2[](
		vec2(-2.0,  2.0), vec2( 0.0,  2.0), vec2( 2.0,  2.0),
		vec2(-2.0,  0.0), vec2( 0.0,  0.0), vec2( 2.0,  0.0),
		vec2(-2.0, -2.0), vec2( 0.0, -2.0), vec2( 2.0, -2.0),
		vec2(-1.0,  1.0), vec2( 1.0,  1.0),
		vec2(-1.0, -1.0), vec2( 1.0, -1.0)
	);
	const float weights[13] = float[](
		0.03125, 0.0625, 0.03125,
		0.0625,  0.125,  0.0625,
		0.03125, 0.0625, 0.03125,
		0.125,   0.125,
		0.125,   0.125
	);

	vec3 result = vec3(0.0);
	float weight_sum = 0.0;
	for (int sample_index = 0; sample_index < 13; ++sample_index)
	{
		vec3 sample_color = max(texture(
			source_tex,
			sample_uv + offsets[sample_index] * pc.source_pixel_size).rgb, vec3(0.0));
		float weight = weights[sample_index];
		if (pc.apply_threshold != 0)
		{
			// Karis weighting prevents isolated HDR fireflies from dominating
			// the entire reconstruction pyramid.
			weight /= 1.0 + luminance(threshold_color(sample_color));
		}
		result += sample_color * weight;
		weight_sum += weight;
	}
	return result / max(weight_sum, 1e-5);
}

void main()
{
	vec3 color = downsample_13_tap(uv);
	if (pc.apply_threshold != 0)
	{
		float brightness = luminance(threshold_color(color));
		float knee = pc.threshold * pc.soft_knee;
		float soft = brightness - pc.threshold + knee;
		soft = clamp(soft, 0.0, 2.0 * knee);
		soft = soft * soft / max(4.0 * knee, 1e-5);
		float contribution = max(soft, brightness - pc.threshold)
			/ max(brightness, 1e-5);
		color *= clamp(contribution, 0.0, 1.0);
	}
	frag_color = vec4(color, 1.0);
}
