#ifndef TONEMAPPING_LOCAL_GUIDED_H
#define TONEMAPPING_LOCAL_GUIDED_H

struct TonemappingLocalGuidedResult
{
	float source_lightness;
	float target_lightness;
	float multiplier;
};

TonemappingLocalGuidedResult tonemapping_local_guided_transfer(
	int method,
	sampler2DArray tonemapping_lut,
	vec3 exposed_color,
	float lut_integration_scale,
	sampler2D local_guide,
	sampler2D local_fused_lightness,
	vec2 uv,
	vec2 guide_pixel_size)
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

	float source_lightness = tonemap_perceptual_lightness(method,
		tonemap_apply(method, tonemapping_lut, exposed_color, lut_integration_scale));
	float target_lightness = max(slope * source_lightness + intercept, 0.0);
	float multiplier = target_lightness / max(source_lightness, 1e-5);
	const float low_light_threshold = 0.007;
	float low_light_fade = clamp(source_lightness / low_light_threshold, 0.0, 1.0);
	multiplier = mix(1.0, multiplier, low_light_fade * low_light_fade);

	TonemappingLocalGuidedResult result;
	result.source_lightness = source_lightness;
	result.target_lightness = target_lightness;
	result.multiplier = max(multiplier, 0.0);
	return result;
}

#endif
