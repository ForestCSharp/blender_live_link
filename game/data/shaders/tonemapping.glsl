// Common Shader Code
#include "shader_common.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.glslh"

@fs fs

layout(binding=0) uniform fs_params {
	float exposure_bias; 
};

layout(binding=0) uniform texture2D tex;

layout(binding=0) uniform sampler smp;

in vec2 uv;

out vec4 frag_color;

float get_exposure_multiplier()
{
	return pow(2.0, exposure_bias);
}

vec3 tonemap_filmic(vec3 x)
{
    x = max(vec3(0.0), x - 0.004);
    return (x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06);
}

vec3 tonemap_reinhard(vec3 x)
{
    return x / (x + vec3(1.0));
}

vec3 tonemap_uncharted2_helper(vec3 x)
{
    const float A = 0.15; const float B = 0.50; const float C = 0.10;
	const float D = 0.20; const float E = 0.02; const float F = 0.30;
    // Apply the tone mapping curve
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 tonemap_uncharted2(vec3 color) {
    const float W = 11.2; // white point — tweak to taste or keep as 11.2
    return tonemap_uncharted2_helper(color) / tonemap_uncharted2_helper(vec3(W));
}

vec3 gamma_correct(vec3 color)
{
    return pow(color, vec3(1.0 / 2.2));
}

void main()
{
	// Input Color 
 	vec3 color = texture(sampler2D(tex, smp), uv).xyz;

	// Exposure
	vec3 exposed_color = color * get_exposure_multiplier();

	// Tonemapping
	vec3 tonemapped_color = tonemap_reinhard(exposed_color); 

	// Write final color output
	frag_color = vec4(tonemapped_color, 1.0);
}

@end

@program tonemapping vs fs
