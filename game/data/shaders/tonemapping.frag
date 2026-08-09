#version 450

#include "tonemapping_operators.h"
#include "tonemapping_validation_chart.h"

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D local_guide;
layout(set = 0, binding = 2) uniform sampler2D local_fused_lightness;
layout(set = 0, binding = 3) uniform sampler2D bloom_color;
layout(set = 0, binding = 4) uniform sampler2DArray tonemapping_lut;

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
} pc;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

void main()
{
	float exposure_scale = exp2(pc.exposure_bias);
	vec3 source_color = pc.validation_chart == 2 ? vec3(0.18)
		: pc.validation_chart == 1 ? tonemapping_validation_chart(uv)
		: texture(scene_color, uv).rgb;
	vec3 exposed_color = max(source_color, vec3(0.0)) * exposure_scale;
	vec3 exposed_bloom = vec3(0.0);
	if (pc.validation_chart == 0 && pc.bloom_intensity > 0.0)
	{
		exposed_bloom = max(texture(bloom_color, uv).rgb, vec3(0.0))
			* pc.bloom_profile_gain.rgb * exposure_scale * pc.bloom_intensity;
	}
	vec3 tonemapped_color;

	if (pc.local_enabled != 0)
	{
		// Guided upsampling transfers the reconstructed quarter-resolution
		// lightness back to the full-resolution image.
		float mean_x = 0.0;
		float mean_y = 0.0;
		float mean_x2 = 0.0;
		float mean_xy = 0.0;
		float weight_sum = 0.0;
		for (int y = -1; y <= 1; ++y)
		{
			for (int x = -1; x <= 1; ++x)
			{
				vec2 sample_uv = uv + vec2(x, y) * pc.guide_pixel_size;
				float guide = texture(local_guide, sample_uv).g;
				float fused = texture(local_fused_lightness, sample_uv).r;
				float kernel_weight = exp(-0.5 * float(x * x + y * y) / (0.7 * 0.7));
				mean_x += guide * kernel_weight;
				mean_y += fused * kernel_weight;
				mean_x2 += guide * guide * kernel_weight;
				mean_xy += guide * fused * kernel_weight;
				weight_sum += kernel_weight;
			}
		}
		mean_x /= weight_sum;
		mean_y /= weight_sum;
		mean_x2 /= weight_sum;
		mean_xy /= weight_sum;
		float slope = (mean_xy - mean_x * mean_y) /
			(max(mean_x2 - mean_x * mean_x, 0.0) + 1e-5);
		float intercept = mean_y - slope * mean_x;

		float source_lightness = tonemap_perceptual_lightness(pc.method,
			tonemap_apply(pc.method, tonemapping_lut, exposed_color, pc.lut_integration_scale));
		float target_lightness = max(slope * source_lightness + intercept, 0.0);
		float local_multiplier = target_lightness / max(source_lightness, 1e-5);
		const float low_light_threshold = 0.007;
		float low_light_fade = clamp(source_lightness / low_light_threshold, 0.0, 1.0);
		local_multiplier = mix(1.0, local_multiplier, low_light_fade * low_light_fade);
		tonemapped_color = tonemap_apply(pc.method, tonemapping_lut,
			exposed_color * max(local_multiplier, 0.0) + exposed_bloom,
			pc.lut_integration_scale);
	}
	else
	{
		tonemapped_color = tonemap_apply(
			pc.method, tonemapping_lut, exposed_color + exposed_bloom, pc.lut_integration_scale);
	}

	frag_color = vec4(tonemapped_color, 1.0);
}
