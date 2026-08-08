#version 450

#include "tonemapping_operators.h"

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2DArray tonemapping_lut;

layout(push_constant) uniform PushConstants
{
	vec2 source_pixel_size;
	float exposure_bias;
	float shadow_recovery;
	float highlight_recovery;
	float preference_sigma;
	int method;
	float lut_integration_scale;
} pc;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 exposure_lightness;
layout(location = 1) out vec4 exposure_weights;

vec3 downsample_hdr_2x(vec2 sample_uv)
{
	// Eight-bilinear-tap 2x downsampling filter from Bart Wronski. The
	// negative cross lobes suppress frequencies that would alias under motion.
	vec3 color = vec3(0.0);
	color += 0.37487566 * texture(scene_color, sample_uv + vec2(-0.75777, -0.75777) * pc.source_pixel_size).rgb;
	color += 0.37487566 * texture(scene_color, sample_uv + vec2( 0.75777, -0.75777) * pc.source_pixel_size).rgb;
	color += 0.37487566 * texture(scene_color, sample_uv + vec2( 0.75777,  0.75777) * pc.source_pixel_size).rgb;
	color += 0.37487566 * texture(scene_color, sample_uv + vec2(-0.75777,  0.75777) * pc.source_pixel_size).rgb;
	color -= 0.12487566 * texture(scene_color, sample_uv + vec2(-2.907, 0.0) * pc.source_pixel_size).rgb;
	color -= 0.12487566 * texture(scene_color, sample_uv + vec2( 2.907, 0.0) * pc.source_pixel_size).rgb;
	color -= 0.12487566 * texture(scene_color, sample_uv + vec2(0.0, -2.907) * pc.source_pixel_size).rgb;
	color -= 0.12487566 * texture(scene_color, sample_uv + vec2(0.0,  2.907) * pc.source_pixel_size).rgb;
	return max(color, vec3(0.0));
}

void main()
{
	vec3 exposed = downsample_hdr_2x(uv) * exp2(pc.exposure_bias);
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
