#version 450

layout(set = 0, binding = 0) uniform sampler2D exposure_input;
layout(set = 0, binding = 1) uniform sampler2D weight_input;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 blended_lightness;

void main()
{
	vec3 exposures = texture(exposure_input, uv).rgb;
	vec3 weights = max(texture(weight_input, uv).rgb, vec3(0.0));
	weights /= dot(weights, vec3(1.0)) + 1e-5;
	blended_lightness = vec4(vec3(dot(exposures, weights)), 1.0);
}
