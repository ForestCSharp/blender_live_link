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
	vec4 packed_fine = texture(exposure_fine, uv);
	vec4 packed_weights = texture(weight_fine, uv);
	float fine_coverage = min(packed_fine.a, packed_weights.a);
	if (fine_coverage <= 1.0e-5)
	{
		reconstructed_lightness = vec4(0.0);
		return;
	}
	vec3 fine = packed_fine.rgb / max(packed_fine.a, 1.0e-5);
	vec3 weights = max(
		packed_weights.rgb / max(packed_weights.a, 1.0e-5), vec3(0.0));
	vec4 packed_coarse = texture(exposure_coarse, uv);
	vec3 coarse = packed_coarse.a > 1.0e-5
		? packed_coarse.rgb / packed_coarse.a : fine;
	vec3 laplacians = fine - coarse;
	if (pc.boost_local_contrast != 0)
	{
		weights *= abs(laplacians) + vec3(1e-5);
	}
	weights /= dot(weights, vec3(1.0)) + 1e-5;
	vec4 packed_accumulated = texture(accumulated_coarse, uv);
	float accumulated = packed_accumulated.a > 1.0e-5
		? packed_accumulated.r / packed_accumulated.a
		: dot(fine, weights);
	float result = max(accumulated + dot(laplacians, weights), 0.0);
	reconstructed_lightness = vec4(vec3(result * fine_coverage), fine_coverage);
}
