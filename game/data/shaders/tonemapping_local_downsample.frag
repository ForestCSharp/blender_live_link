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

vec3 downsample_2x(sampler2D source, vec2 sample_uv)
{
	vec3 value = vec3(0.0);
	value += 0.37487566 * texture(source, sample_uv + vec2(-0.75777, -0.75777) * pc.source_pixel_size).rgb;
	value += 0.37487566 * texture(source, sample_uv + vec2( 0.75777, -0.75777) * pc.source_pixel_size).rgb;
	value += 0.37487566 * texture(source, sample_uv + vec2( 0.75777,  0.75777) * pc.source_pixel_size).rgb;
	value += 0.37487566 * texture(source, sample_uv + vec2(-0.75777,  0.75777) * pc.source_pixel_size).rgb;
	value -= 0.12487566 * texture(source, sample_uv + vec2(-2.907, 0.0) * pc.source_pixel_size).rgb;
	value -= 0.12487566 * texture(source, sample_uv + vec2( 2.907, 0.0) * pc.source_pixel_size).rgb;
	value -= 0.12487566 * texture(source, sample_uv + vec2(0.0, -2.907) * pc.source_pixel_size).rgb;
	value -= 0.12487566 * texture(source, sample_uv + vec2(0.0,  2.907) * pc.source_pixel_size).rgb;
	return value;
}

void main()
{
	vec3 exposures = clamp(downsample_2x(exposure_input, uv), 0.0, 1.0);
	vec3 weights = max(downsample_2x(weight_input, uv), vec3(0.0));
	weights /= dot(weights, vec3(1.0)) + 1e-5;
	exposure_output = vec4(exposures, 1.0);
	weight_output = vec4(weights, 1.0);
}
