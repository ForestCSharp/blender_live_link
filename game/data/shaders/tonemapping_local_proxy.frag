#version 450

#include "tonemapping_operators.h"
#include "tonemapping_validation_chart.h"
#include "auto_adaptation.h"

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2DArray tonemapping_lut;
layout(set = 0, binding = 2) uniform sampler2D position_tex;
layout(std430, set = 0, binding = 5) readonly buffer AutoAdaptationStateBlock
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

// The local field is geometry-only. A positive binomial kernel avoids the
// ringing that negative-lobe HDR filters produce at bright sky silhouettes.
vec4 downsample_geometry_hdr(vec2 sample_uv)
{
	ivec2 source_size = textureSize(scene_color, 0);
	ivec2 center = clamp(ivec2(sample_uv * vec2(source_size)),
		ivec2(0), source_size - ivec2(1));
	const float kernel[4] = float[4](1.0, 3.0, 3.0, 1.0);
	vec3 weighted_color = vec3(0.0);
	float geometry_weight = 0.0;
	for (int y = 0; y < 4; ++y)
	{
		for (int x = 0; x < 4; ++x)
		{
			ivec2 pixel = clamp(center + ivec2(x - 1, y - 1),
				ivec2(0), source_size - ivec2(1));
			vec2 tap_uv = (vec2(pixel) + 0.5) / vec2(source_size);
			float weight = kernel[x] * kernel[y];
			float geometry = pc.validation_chart == 3
				? tonemapping_validation_geometry_mask(tap_uv)
				: pc.validation_chart != 0 ? 1.0
				: (texelFetch(position_tex, pixel, 0).w == 0.0 ? 0.0 : 1.0);
			vec3 tap_color = pc.validation_chart == 2 ? vec3(0.18)
				: pc.validation_chart == 1 ? tonemapping_validation_chart(tap_uv)
				: pc.validation_chart == 3 ? tonemapping_validation_sky_geometry_chart(tap_uv)
				: texelFetch(scene_color, pixel, 0).rgb;
			weighted_color += weight * geometry * tap_color;
			geometry_weight += weight * geometry;
		}
	}
	const float total_weight = 64.0;
	float coverage = geometry_weight / total_weight;
	vec3 color = geometry_weight > 1.0e-5
		? weighted_color / geometry_weight : vec3(0.0);
	return vec4(max(color, vec3(0.0)), coverage);
}

void main()
{
	vec4 filtered = downsample_geometry_hdr(uv);
	vec3 source = filtered.rgb;
	float coverage = filtered.a;
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

	// Premultiplication lets every later mip filter coverage without leaking
	// unsupported sky values into the geometry field.
	exposure_lightness = vec4(lightness * coverage, coverage);
	exposure_weights = vec4(weights * coverage, coverage);
}
