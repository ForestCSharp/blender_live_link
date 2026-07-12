// Common Shader Code
#include "shader_common.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.h"

@fs fs

@image_sample_type shadow_moments_texture float
layout(binding=0) uniform texture2DArray shadow_moments_texture;

@sampler_type shadow_moments_sampler filtering
layout(binding=0) uniform sampler shadow_moments_sampler;

layout(binding=0) uniform fs_params {
	int cascade_index;
	int view_mode;
};

in vec2 uv;

out vec4 frag_color;

const float EVSM_POSITIVE_EXPONENT = 5.0;

float unwarp_positive_evsm_depth(float warped_depth)
{
	float centered_depth = log(max(warped_depth, 0.00001)) / EVSM_POSITIVE_EXPONENT;
	return centered_depth * 0.5 + 0.5;
}

void main()
{
	vec4 moments = texture(sampler2DArray(shadow_moments_texture, shadow_moments_sampler), vec3(uv, float(cascade_index)));
	if (view_mode == 1)
	{
		float depth = unwarp_positive_evsm_depth(moments.x);
		frag_color = vec4(vec3(depth), 1.0);
	}
	else
	{
		frag_color = vec4(moments.rgb, 1.0);
	}
}

@end

@program shadow_cascade_debug vs fs
