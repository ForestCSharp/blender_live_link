#version 450

#include "tonemapping_operators.h"
#include "tonemapping_validation_chart.h"
#include "auto_adaptation.h"
#include "tonemapping_local_guided.h"

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D local_guide;
layout(set = 0, binding = 2) uniform sampler2D local_fused_lightness;
layout(set = 0, binding = 3) uniform sampler2D bloom_color;
layout(set = 0, binding = 4) uniform sampler2DArray tonemapping_lut;
layout(set = 0, binding = 5) uniform sampler2D position_tex;
layout(std430, set = 0, binding = 6) readonly buffer AutoAdaptationStateBlock
{
	vec4 auto_adaptation_values[AUTO_ADAPTATION_STATE_VEC4_COUNT];
};

layout(push_constant) uniform PushConstants
{
	int method;
	int local_enabled;
	float exposure_bias;
	float bloom_intensity;
	vec2 guide_pixel_size;
	float lut_integration_scale;
	int validation_chart;
	vec4 bloom_profile_gain;
	int auto_exposure_enabled;
	int auto_white_balance_enabled;
	float bloom_auto_exposure_influence;
	vec4 local_recovery;
} pc;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

void main()
{
	float auto_exposure_ev = pc.auto_exposure_enabled != 0
		? auto_adaptation_values[AUTO_ADAPTATION_STATE_EXPOSURE_WHITE].x
		: 0.0;
	float exposure_scale = exp2(pc.exposure_bias + auto_exposure_ev);
	float bloom_exposure_scale = exp2(pc.exposure_bias
		+ auto_exposure_ev * clamp(pc.bloom_auto_exposure_influence, 0.0, 1.0));
	vec3 source_color = pc.validation_chart == 2 ? vec3(0.18)
		: pc.validation_chart == 1 ? tonemapping_validation_chart(uv)
		: pc.validation_chart == 3 ? tonemapping_validation_sky_geometry_chart(uv)
		: texture(scene_color, uv).rgb;
	if (pc.auto_white_balance_enabled != 0)
	{
		source_color = auto_adaptation_apply_white_balance(
			source_color,
			auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_0],
			auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_1],
			auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_2]);
	}
	vec3 exposed_color = max(source_color, vec3(0.0)) * exposure_scale;
	vec3 exposed_bloom = vec3(0.0);
	if (pc.validation_chart == 0 && pc.bloom_intensity > 0.0)
	{
		vec3 bloom = texture(bloom_color, uv).rgb;
		if (pc.auto_white_balance_enabled != 0)
		{
			bloom = auto_adaptation_apply_white_balance(
				bloom,
				auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_0],
				auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_1],
				auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_2]);
		}
		exposed_bloom = max(bloom, vec3(0.0)) * pc.bloom_profile_gain.rgb
			* bloom_exposure_scale * pc.bloom_intensity;
	}
	vec3 tonemapped_color;

	if (pc.local_enabled != 0)
	{
		float local_strength = pc.validation_chart == 3
			? tonemapping_validation_geometry_local_strength(
				uv, 1.0 / vec2(textureSize(position_tex, 0)))
			: pc.validation_chart != 0 ? 1.0
			: tonemapping_geometry_local_strength(position_tex, uv);
		float local_multiplier = 1.0;
		if (local_strength > 1.0e-5)
		{
			TonemappingLocalGuidedResult guided = tonemapping_local_guided_transfer(
				pc.method,
				tonemapping_lut,
				exposed_color,
				pc.lut_integration_scale,
				local_guide,
				local_fused_lightness,
				uv,
				pc.guide_pixel_size,
				pc.local_recovery.x,
				pc.local_recovery.y);
			local_multiplier = mix(1.0, guided.multiplier, local_strength);
		}
		tonemapped_color = tonemap_apply(pc.method, tonemapping_lut,
			exposed_color * local_multiplier + exposed_bloom,
			pc.lut_integration_scale);
	}
	else
	{
		tonemapped_color = tonemap_apply(
			pc.method, tonemapping_lut, exposed_color + exposed_bloom, pc.lut_integration_scale);
	}

	frag_color = vec4(tonemapped_color, 1.0);
}
