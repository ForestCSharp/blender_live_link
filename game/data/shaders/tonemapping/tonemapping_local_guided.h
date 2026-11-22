#ifndef TONEMAPPING_LOCAL_GUIDED_H
#define TONEMAPPING_LOCAL_GUIDED_H

struct TonemappingLocalGuidedResult
{
	float source_lightness;
	float target_lightness;
	float multiplier;
	float geometry_coverage;
};

float tonemapping_geometry_local_strength(sampler2D position_tex, vec2 uv)
{
	ivec2 size = textureSize(position_tex, 0);
	ivec2 center = clamp(ivec2(uv * vec2(size)), ivec2(0), size - ivec2(1));
	if (texelFetch(position_tex, center, 0).w == 0.0)
	{
		return 0.0;
	}

	float geometry_count = 0.0;
	for (int y = -1; y <= 1; ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			ivec2 pixel = clamp(center + ivec2(x, y), ivec2(0), size - ivec2(1));
			geometry_count += texelFetch(position_tex, pixel, 0).w == 0.0 ? 0.0 : 1.0;
		}
	}
	// A straight silhouette has six geometry samples and is fully suppressed;
	// local recovery fades back in as the 3x3 neighborhood becomes geometry.
	return smoothstep(2.0 / 3.0, 1.0, geometry_count / 9.0);
}

TonemappingLocalGuidedResult tonemapping_local_guided_transfer(
	int method,
	sampler2DArray tonemapping_lut,
	vec3 exposed_color,
	float lut_integration_scale,
	sampler2D local_guide,
	sampler2D local_fused_lightness,
	vec2 uv,
	vec2 guide_pixel_size,
	float shadow_recovery,
	float highlight_recovery)
{
	float mean_x = 0.0;
	float mean_y = 0.0;
	float mean_x2 = 0.0;
	float mean_xy = 0.0;
	float weight_sum = 0.0;
	for (int y = -1; y <= 1; ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			vec2 sample_uv = uv + vec2(x, y) * guide_pixel_size;
			vec4 packed_guide = texture(local_guide, sample_uv);
			vec4 packed_fused = texture(local_fused_lightness, sample_uv);
			float coverage = min(packed_guide.a, packed_fused.a);
			float guide = packed_guide.a > 1.0e-5
				? packed_guide.g / packed_guide.a : 0.0;
			float fused = packed_fused.a > 1.0e-5
				? packed_fused.r / packed_fused.a : 0.0;
			float kernel_weight = exp(-0.5 * float(x * x + y * y) / (0.7 * 0.7));
			kernel_weight *= coverage;
			mean_x += guide * kernel_weight;
			mean_y += fused * kernel_weight;
			mean_x2 += guide * guide * kernel_weight;
			mean_xy += guide * fused * kernel_weight;
			weight_sum += kernel_weight;
		}
	}
	float source_lightness = tonemap_perceptual_lightness(method,
		tonemap_apply(method, tonemapping_lut, exposed_color, lut_integration_scale));
	if (weight_sum <= 1.0e-5)
	{
		TonemappingLocalGuidedResult neutral;
		neutral.source_lightness = source_lightness;
		neutral.target_lightness = source_lightness;
		neutral.multiplier = 1.0;
		neutral.geometry_coverage = 0.0;
		return neutral;
	}
	mean_x /= weight_sum;
	mean_y /= weight_sum;
	mean_x2 /= weight_sum;
	mean_xy /= weight_sum;
	float slope = (mean_xy - mean_x * mean_y) /
		(max(mean_x2 - mean_x * mean_x, 0.0) + 1e-5);
	float intercept = mean_y - slope * mean_x;

	float target_lightness = max(slope * source_lightness + intercept, 0.0);
	float multiplier = target_lightness / max(source_lightness, 1e-5);
	const float low_light_threshold = 0.007;
	float low_light_fade = clamp(source_lightness / low_light_threshold, 0.0, 1.0);
	multiplier = mix(1.0, multiplier, low_light_fade * low_light_fade);
	multiplier = clamp(multiplier,
		exp2(-max(highlight_recovery, 0.0)),
		exp2(max(shadow_recovery, 0.0)));

	TonemappingLocalGuidedResult result;
	result.source_lightness = source_lightness;
	result.target_lightness = source_lightness * multiplier;
	result.multiplier = max(multiplier, 0.0);
	result.geometry_coverage = clamp(weight_sum, 0.0, 1.0);
	return result;
}

#endif
