#version 450

layout(set = 0, binding = 0) uniform sampler2D exposure_input;
layout(set = 0, binding = 1) uniform sampler2D weight_input;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 blended_lightness;

void main()
{
	vec4 packed_exposures = texture(exposure_input, uv);
	vec4 packed_weights = texture(weight_input, uv);
	float coverage = min(packed_exposures.a, packed_weights.a);
	if (coverage <= 1.0e-5)
	{
		blended_lightness = vec4(0.0);
		return;
	}
	vec3 exposures = packed_exposures.rgb / max(packed_exposures.a, 1.0e-5);
	vec3 weights = max(packed_weights.rgb / max(packed_weights.a, 1.0e-5), vec3(0.0));
	weights /= dot(weights, vec3(1.0)) + 1e-5;
	blended_lightness = vec4(vec3(dot(exposures, weights) * coverage), coverage);
}
