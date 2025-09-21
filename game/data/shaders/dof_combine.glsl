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
	int dof_enabled;
	PADDING(1);
};

in vec2 uv;

out vec4 frag_color;

void main()
{
	// Depth of Field Combine
 	vec4 color = texture(sampler2D(tex, smp), uv);

	// just return sampled non-blurred color if feature is disabled
	if (dof_enabled == 0)
	{
		frag_color = color;
		return;
	}

	vec4 blurred_color = texture(sampler2D(blur_tex, smp), uv);
	vec4 world_position	= texture(sampler2D(position_tex, smp), uv);

	const float min_distance_squared = pow(min_distance, 2);
	const float max_distance_squared = pow(max_distance, 2);

	vec4 cam_pos_to_world_pos = world_position - cam_pos;
	float distance_squared = dot(cam_pos_to_world_pos, cam_pos_to_world_pos);

	float blur_amount = smoothstep(min_distance_squared, max_distance_squared, distance_squared);

	vec4 dof_combine_color = mix(color, blurred_color, blur_amount);

	frag_color = dof_combine_color;
}

@end

@program dof_combine vs fs
