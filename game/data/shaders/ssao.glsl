
// Common Shader Code
#include "shader_common.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.glslh"

@fs fs

#include "ssao_constants.h"

layout(binding=0) uniform texture2D ssao_position_tex;
layout(binding=1) uniform texture2D ssao_normal_tex;
layout(binding=2) uniform texture2D ssao_noise_tex;

layout(binding=0) uniform sampler tex_sampler;

layout(binding=0) uniform fs_params {
	vec2 screen_size;
	mat4 view;
	mat4 projection;
	vec4 kernel_samples[SSAO_KERNEL_SIZE];
	int ssao_enable;
};

in vec2 uv;

out vec4 frag_color;

void main()
{
	if (ssao_enable == 0)
	{
		frag_color = vec4(1,1,1,1);
		return;
	}

	const vec2 noiseScale = screen_size / 4.0;

	vec3 position = (view * texture(sampler2D(ssao_position_tex, tex_sampler), uv)).xyz;
    vec3 normal = normalize((view * texture(sampler2D(ssao_normal_tex, tex_sampler), uv)).rgb);
    vec3 noise = normalize(texture(sampler2D(ssao_noise_tex, tex_sampler), uv * noiseScale).xyz);

    // create TBN change-of-basis matrix: from tangent-space to view-space
    vec3 tangent = normalize(noise - normal * dot(noise, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);
    // iterate over the sample kernel and calculate occlusion factor
    float occlusion = 0.0;
    for(int i = 0; i < SSAO_KERNEL_SIZE; ++i)
    {
        // get sample position
        vec3 samplePos = TBN * kernel_samples[i].xyz; // from tangent to view-space
        samplePos = position + samplePos * SSAO_RADIUS; 
 
        // project sample position (to sample texture) (to get position on screen/texture)
        vec4 offset = vec4(samplePos, 1.0);
        offset = projection * offset; // from view to clip-space
        offset.xyz /= offset.w; // perspective divide
        offset.xyz = offset.xyz * 0.5 + 0.5; // transform to range 0.0 - 1.0
		offset.y = 1.0 - offset.y;
        
        // get sample depth
        float sampleDepth = (view * texture(sampler2D(ssao_position_tex, tex_sampler), offset.xy)).z; 
        
        // range check & accumulate
        float rangeCheck = smoothstep(0.0, 1.0, SSAO_RADIUS / abs(position.z - sampleDepth));
        occlusion += (sampleDepth >= samplePos.z + SSAO_BIAS ? 1.0 : 0.0) * rangeCheck;           
    }
    occlusion = 1.0 - (occlusion / SSAO_KERNEL_SIZE);
	frag_color = vec4(vec3(occlusion), 1.0);
}

@end

@program ssao vs fs
