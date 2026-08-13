#version 450

#include "tonemapping_operators.h"
#include "tonemapping_validation_chart.h"
#include "auto_adaptation.h"
#include "tonemapping_local_guided.h"

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D local_guide;
layout(set = 0, binding = 2) uniform sampler2D local_fused_lightness;
layout(set = 0, binding = 3) uniform sampler2DArray tonemapping_lut;
layout(set = 0, binding = 4) uniform sampler2D position_tex;
layout(std430, set = 0, binding = 5) readonly buffer AutoAdaptationStateBlock
{
	vec4 auto_adaptation_values[AUTO_ADAPTATION_STATE_VEC4_COUNT];
};

layout(push_constant) uniform PushConstants
{
	int method;
	float exposure_bias;
	vec2 guide_pixel_size;
	float lut_integration_scale;
	int validation_chart;
	int auto_exposure_enabled;
	int auto_white_balance_enabled;
	vec2 recovery_limits;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 guided_debug;

void main()
{
	float auto_exposure_ev = pc.auto_exposure_enabled != 0
		? auto_adaptation_values[AUTO_ADAPTATION_STATE_EXPOSURE_WHITE].x
		: 0.0;
	vec3 source = pc.validation_chart == 2 ? vec3(0.18)
		: pc.validation_chart == 1 ? tonemapping_validation_chart(uv)
		: pc.validation_chart == 3 ? tonemapping_validation_sky_geometry_chart(uv)
		: texture(scene_color, uv).rgb;
	if (pc.auto_white_balance_enabled != 0)
	{
		source = auto_adaptation_apply_white_balance(
			source,
			auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_0],
			auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_1],
			auto_adaptation_values[AUTO_ADAPTATION_STATE_WB_COLUMN_2]);
	}
	vec3 exposed = max(source, vec3(0.0)) * exp2(pc.exposure_bias + auto_exposure_ev);
	TonemappingLocalGuidedResult result = tonemapping_local_guided_transfer(
		pc.method,
		tonemapping_lut,
		exposed,
		pc.lut_integration_scale,
		local_guide,
		local_fused_lightness,
		uv,
		pc.guide_pixel_size,
		pc.recovery_limits.x,
		pc.recovery_limits.y);
	float local_strength = pc.validation_chart == 3
		? tonemapping_validation_geometry_local_strength(
			uv, 1.0 / vec2(textureSize(position_tex, 0)))
		: pc.validation_chart != 0 ? 1.0
		: tonemapping_geometry_local_strength(position_tex, uv);
	float multiplier = mix(1.0, result.multiplier, local_strength);
	float local_ev = clamp(log2(max(multiplier, 1e-5)), -4.0, 4.0);
	guided_debug = vec4(
		result.source_lightness,
		result.source_lightness * multiplier,
		0.5 + local_ev / 8.0,
		local_strength);
}
