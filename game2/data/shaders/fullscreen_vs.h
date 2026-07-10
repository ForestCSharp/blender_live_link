#ifndef FULLSCREEN_VS_H
#define FULLSCREEN_VS_H

// Shared fullscreen-triangle vertex shader (gl_VertexIndex, 3 verts).
// Include from a .vert after `#version 450`; optionally
// `#define FULLSCREEN_MAX_DEPTH` first to draw at the far plane
// (z = 0 under reverse-Z) for sky-style depth-tested quads.
//
// The framework applies a negative-height (Y-flip) viewport in every pass,
// so uv.y = 0.5 - 0.5*y maps screen-top to texel row v=0 — this compensates
// exactly, no double flip (same formula as the original copy pass).

layout(location = 0) out vec2 uv;

void main()
{
	const vec2 positions[3] = vec2[](
		vec2(-1.0, -1.0),
		vec2( 3.0, -1.0),
		vec2(-1.0,  3.0)
	);

	vec2 pos = positions[gl_VertexIndex];

#if defined(FULLSCREEN_MAX_DEPTH)
	// Far plane under reverse-Z (sky-style depth-tested quads)
	gl_Position = vec4(pos, 0.0, 1.0);
#else
	// Near plane (irrelevant for passes without a depth attachment)
	gl_Position = vec4(pos, 1.0, 1.0);
#endif

	uv = vec2(0.5 * pos.x + 0.5, 0.5 - 0.5 * pos.y);
}

#endif // FULLSCREEN_VS_H
