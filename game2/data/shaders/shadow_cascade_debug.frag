#version 450

layout(set = 0, binding = 0) uniform sampler2DArray shadow_moments_texture;

layout(push_constant) uniform PushConstants
{
	int cascade_index;
	int view_mode;
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag_color;

const float EVSM_POSITIVE_EXPONENT = 5.0;

void main()
{
	vec4 moments = texture(shadow_moments_texture, vec3(uv, float(pc.cascade_index)));
	if (pc.view_mode == 1)
	{
		float centered_depth = log(max(moments.x, 0.00001)) / EVSM_POSITIVE_EXPONENT;
		float depth = centered_depth * 0.5 + 0.5;
		frag_color = vec4(vec3(depth), 1.0);
	}
	else
	{
		frag_color = vec4(moments.rgb, 1.0);
	}
}
