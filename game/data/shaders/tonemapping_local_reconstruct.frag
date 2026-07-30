#version 450

layout(set = 0, binding = 0) uniform sampler2D exposure_fine;
layout(set = 0, binding = 1) uniform sampler2D weight_fine;
layout(set = 0, binding = 2) uniform sampler2D exposure_coarse;
layout(set = 0, binding = 3) uniform sampler2D accumulated_coarse;

layout(push_constant) uniform PushConstants
{
	int boost_local_contrast;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 reconstructed_lightness;

void main()
{
	vec3 laplacians = texture(exposure_fine, uv).rgb - texture(exposure_coarse, uv).rgb;
	vec3 weights = max(texture(weight_fine, uv).rgb, vec3(0.0));
	if (pc.boost_local_contrast != 0)
	{
		weights *= abs(laplacians) + vec3(1e-5);
	}
	weights /= dot(weights, vec3(1.0)) + 1e-5;
	float accumulated = texture(accumulated_coarse, uv).r;
	reconstructed_lightness = vec4(vec3(max(accumulated + dot(laplacians, weights), 0.0)), 1.0);
}
