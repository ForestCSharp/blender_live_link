#version 450

// Exposure + Reinhard (port of game/data/shaders/tonemapping.glsl; the
// alternate filmic/uncharted2 operators can return with a UI toggle).
// No shader-side gamma — the sRGB target view encodes on write.

layout(set = 0, binding = 0) uniform sampler2D scene_color;

layout(push_constant) uniform PushConstants
{
	float exposure_bias;
} pc;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

vec3 tonemap_reinhard(vec3 x)
{
	return x / (x + vec3(1.0));
}

void main()
{
	vec3 color = texture(scene_color, uv).xyz;

	// Exposure
	vec3 exposed_color = color * pow(2.0, pc.exposure_bias);

	// Tonemapping
	vec3 tonemapped_color = tonemap_reinhard(exposed_color);

	frag_color = vec4(tonemapped_color, 1.0);
}
