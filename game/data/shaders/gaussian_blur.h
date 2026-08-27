#ifndef GAUSSIAN_BLUR_H
#define GAUSSIAN_BLUR_H

#ifndef GAUSSIAN_BLUR_FETCH
#error "Define GAUSSIAN_BLUR_FETCH(sample_uv) before including gaussian_blur.h"
#endif

vec4 gaussian_blur_apply(
	vec2 sample_uv,
	vec2 screen_size,
	vec2 direction,
	int blur_size)
{
	vec2 texel_size = 1.0 / screen_size;
	vec4 result = vec4(0.0);
	float total_weight = 0.0;
	float hlim = float(-blur_size) * 0.5 + 0.5;
	float sigma = max(float(blur_size) * 0.25, 1.0);
	float two_sigma_squared = 2.0 * sigma * sigma;
	for (int i = 0; i < blur_size; ++i)
	{
		float sample_offset = hlim + float(i);
		float weight = exp(-(sample_offset * sample_offset) / two_sigma_squared);
		vec2 offset = direction * sample_offset * texel_size;
		result += GAUSSIAN_BLUR_FETCH(sample_uv + offset) * weight;
		total_weight += weight;
	}
	return result / total_weight;
}

#endif // GAUSSIAN_BLUR_H
