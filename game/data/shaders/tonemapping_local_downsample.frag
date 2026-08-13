#version 450

layout(set = 0, binding = 0) uniform sampler2D exposure_input;
layout(set = 0, binding = 1) uniform sampler2D weight_input;

layout(push_constant) uniform PushConstants
{
	vec2 source_pixel_size;
} pc;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 exposure_output;
layout(location = 1) out vec4 weight_output;

vec4 downsample_2x(sampler2D source, vec2 sample_uv)
{
	// Inputs are premultiplied by geometry coverage, so a positive tent filter
	// preserves the mask while remaining free of silhouette ringing.
	vec2 offset = 0.5 * pc.source_pixel_size;
	return 0.25 * (
		texture(source, sample_uv + vec2(-offset.x, -offset.y))
		+ texture(source, sample_uv + vec2( offset.x, -offset.y))
		+ texture(source, sample_uv + vec2( offset.x,  offset.y))
		+ texture(source, sample_uv + vec2(-offset.x,  offset.y)));
}

void main()
{
	vec4 exposures = downsample_2x(exposure_input, uv);
	vec4 weights = downsample_2x(weight_input, uv);
	exposure_output = vec4(max(exposures.rgb, vec3(0.0)), clamp(exposures.a, 0.0, 1.0));
	weight_output = vec4(max(weights.rgb, vec3(0.0)), clamp(weights.a, 0.0, 1.0));
}
