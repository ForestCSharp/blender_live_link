// Common Shader Code
#include "shader_common.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.glslh"

@fs fs

layout(binding=0) uniform texture2D tex;
layout(binding=1) uniform texture2D blur_tex;
layout(binding=2) uniform texture2D position_tex;

layout(binding=0) uniform sampler smp;

layout(binding=0) uniform fs_params {
	vec4 cam_pos;
	float min_distance;
	float max_distance;
	PADDING(2);
};

in vec2 uv;

out vec4 frag_color;

vec3 tonemap_filmic(vec3 x)
{
    x = max(vec3(0.0), x - 0.004);
    return (x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06);
}

vec3 tonemap_reinhard(vec3 x)
{
    return x / (x + vec3(1.0));
}

vec3 tonemap_uncharted_2_helper(vec3 x)
{
    const float A = 0.15; const float B = 0.50; const float C = 0.10;
	const float D = 0.20; const float E = 0.02; const float F = 0.30;
    // Apply the tone mapping curve
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 tonemap_uncharted_2(vec3 color) {
    const float W = 11.2; // white point — tweak to taste or keep as 11.2
    return tonemap_uncharted_2_helper(color) / tonemap_uncharted_2_helper(vec3(W));
}

vec3 gamma_correct(vec3 color)
{
    return pow(color, vec3(1.0 / 2.2));
}

void main()
{
	// Depth of Field Combine
 	vec4 color = texture(sampler2D(tex, smp), uv);
	vec4 blurred_color = texture(sampler2D(blur_tex, smp), uv);
	vec4 world_position	= texture(sampler2D(position_tex, smp), uv);

	const float min_distance_squared = pow(min_distance, 2);
	const float max_distance_squared = pow(max_distance, 2);

	vec4 cam_pos_to_world_pos = world_position - cam_pos;
	float distance_squared = dot(cam_pos_to_world_pos, cam_pos_to_world_pos);

	float blur_amount = smoothstep(min_distance_squared, max_distance_squared, distance_squared);

	vec4 dof_combine_color = mix(color, blurred_color, blur_amount);

	// Tonemapping
	vec3 final_color = tonemap_reinhard(dof_combine_color.xyz);

	frag_color = vec4(final_color,1);
}

@end

@program tonemapping vs fs
