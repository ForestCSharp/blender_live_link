#version 450

// Fullscreen triangle via gl_VertexIndex: (-1,-1), (3,-1), (-1,3).
// The framework applies a negative-height (Y-flip) viewport in every pass,
// so uv.y = 0.5 - 0.5*y maps screen-top to texel row v=0 — this compensates
// exactly, no double flip.

layout(location = 0) out vec2 out_uv;

void main()
{
	const vec2 positions[3] = vec2[](
		vec2(-1.0, -1.0),
		vec2( 3.0, -1.0),
		vec2(-1.0,  3.0)
	);

	vec2 position = positions[gl_VertexIndex];
	gl_Position = vec4(position, 0.0, 1.0);
	out_uv = vec2(0.5 * position.x + 0.5, 0.5 - 0.5 * position.y);
}
