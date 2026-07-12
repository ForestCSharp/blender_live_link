#version 450

// Hemisphere-kernel SSAO over the G-buffer (port of game/data/shaders/
// ssao.glsl fs). Runs at half render resolution; output is blurred before
// lighting samples it.

#include "ssao_constants.h"

layout(set = 0, binding = 0) uniform fs_params
{
	vec2 screen_size;
	mat4 view;
	mat4 projection;
	vec4 kernel_samples[SSAO_KERNEL_SIZE];
	int ssao_enable;
};

layout(set = 0, binding = 1) uniform sampler2D ssao_position_tex;
layout(set = 0, binding = 2) uniform sampler2D ssao_normal_tex;
layout(set = 0, binding = 3) uniform sampler2D ssao_noise_tex;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

void main()
{
	if (ssao_enable == 0)
	{
		frag_color = vec4(1, 1, 1, 1);
		return;
	}

	const vec2 noise_scale = screen_size / 4.0;

	vec3 pixel_position = (view * texture(ssao_position_tex, uv)).xyz;
	vec3 pixel_normal = normalize((view * texture(ssao_normal_tex, uv)).xyz);
	vec3 noise = normalize(texture(ssao_noise_tex, uv * noise_scale).xyz);

	// TBN change-of-basis matrix: from tangent-space to view-space
	vec3 tangent = normalize(noise - pixel_normal * dot(noise, pixel_normal));
	vec3 bitangent = cross(pixel_normal, tangent);
	mat3 TBN = mat3(tangent, bitangent, pixel_normal);

	// Iterate over the sample kernel and calculate the occlusion factor
	float occlusion = 0.0;
	for (int i = 0; i < SSAO_KERNEL_SIZE; ++i)
	{
		// From tangent to view-space
		vec3 sample_pos = TBN * kernel_samples[i].xyz;
		sample_pos = pixel_position + sample_pos * SSAO_RADIUS;

		// Project the sample position to get its uv on screen
		vec4 offset = vec4(sample_pos, 1.0);
		offset = projection * offset;
		offset.xyz /= offset.w;
		offset.xyz = offset.xyz * 0.5 + 0.5;
		offset.y = 1.0 - offset.y;

		float sample_depth = (view * texture(ssao_position_tex, offset.xy)).z;

		// Range check & accumulate
		float range_check = smoothstep(0.0, 1.0, SSAO_RADIUS / abs(pixel_position.z - sample_depth));
		occlusion += (sample_depth >= sample_pos.z + SSAO_BIAS ? 1.0 : 0.0) * range_check;
	}
	occlusion = 1.0 - (occlusion / SSAO_KERNEL_SIZE);
	frag_color = vec4(vec3(occlusion), 1.0);
}
