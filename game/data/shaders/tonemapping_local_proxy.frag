#version 450

#include "tonemapping_operators.h"
#include "tonemapping_validation_chart.h"
#include "auto_adaptation.h"

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2DArray tonemapping_lut;
layout(std430, set = 0, binding = 4) readonly buffer AutoAdaptationStateBlock
{
	vec4 auto_adaptation_values[AUTO_ADAPTATION_STATE_VEC4_COUNT];
};

layout(push_constant) uniform PushConstants
{
	vec2 source_pixel_size;
	float exposure_bias;
	float shadow_recovery;
	float highlight_recovery;
	float preference_sigma;
	int method;
	float lut_integration_scale;
	int validation_chart;
	int auto_exposure_enabled;
	int auto_white_balance_enabled;
} pc;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 exposure_lightness;
layout(location = 1) out vec4 exposure_weights;

vec3 downsample_hdr_2x(vec2 sample_uv)
{
	// Eight-bilinear-tap 2x downsampling filter from Bart Wronski. The
	// negative cross lobes suppress frequencies that would alias under motion.
	vec3 color = vec3(0.0);
	vec2 offsets[8] = vec2[8](
		vec2(-0.75777, -0.75777), vec2( 0.75777, -0.75777),
		vec2( 0.75777,  0.75777), vec2(-0.75777,  0.75777),
		vec2(-2.907, 0.0), vec2(2.907, 0.0),
		vec2(0.0, -2.907), vec2(0.0, 2.907));
	for (int tap = 0; tap < 8; ++tap)
	{
		vec2 tap_uv = sample_uv + offsets[tap] * pc.source_pixel_size;
		vec3 tap_color = pc.validation_chart == 2 ? vec3(0.18)
			: pc.validation_chart == 1 ? tonemapping_validation_chart(tap_uv)
			: texture(scene_color, tap_uv).rgb;
		color += (tap < 4 ? 0.37487566 : -0.12487566) * tap_color;
	}
	return max(color, vec3(0.0));
}

void main()
{
	vec3 source = downsample_hdr_2x(uv);
	if (pc.auto_white_balance_enabled != 0)
	{
		source = auto_adaptation_apply_white_balance(
			source,
			auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_0],
			auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_1],
			auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_2]);
	}
	float auto_exposure_ev = pc.auto_exposure_enabled != 0
		? auto_adaptation_values[AUTO_ADAPTATION_STATE_EXPOSURE_WHITE].x
		: 0.0;
	vec3 exposed = source * exp2(pc.exposure_bias + auto_exposure_ev);
	vec3 lightness = vec3(
		tonemap_perceptual_lightness(pc.method, tonemap_apply(
			pc.method, tonemapping_lut, exposed * exp2(-pc.highlight_recovery), pc.lut_integration_scale)),
		tonemap_perceptual_lightness(pc.method, tonemap_apply(
			pc.method, tonemapping_lut, exposed, pc.lut_integration_scale)),
		tonemap_perceptual_lightness(pc.method, tonemap_apply(
			pc.method, tonemapping_lut, exposed * exp2(pc.shadow_recovery), pc.lut_integration_scale))
	);

	vec3 distance_from_middle_gray = lightness - vec3(0.5);
	float sigma_squared = pc.preference_sigma * pc.preference_sigma;
	vec3 weights = exp(-0.5 * distance_from_middle_gray * distance_from_middle_gray * sigma_squared);
	weights /= dot(weights, vec3(1.0)) + 1e-5;

	exposure_lightness = vec4(lightness, 1.0);
	exposure_weights = vec4(weights, 1.0);
}
