
// Helper to send uv data for use in a fullscreen pass, fragment stage should expect a single input 'uv' to be passed along
@vs vs

out vec2 uv;

struct FullscreenVertex
{
	vec2 position;
	vec2 uv;
};

void main()
{
	const FullscreenVertex vertices[4] = FullscreenVertex[4](
		FullscreenVertex(vec2(-1.0, -1.0), vec2(0.0, 1.0)),
		FullscreenVertex(vec2(1.0, -1.0), vec2(1.0, 1.0)),
		FullscreenVertex(vec2(-1.0, 1.0), vec2(0.0, 0.0)),
		FullscreenVertex(vec2(1.0, 1.0), vec2(1.0, 0.0))
	);

	const int indices[6] = {0,1,2,1,2,3};

	FullscreenVertex vertex = vertices[indices[gl_VertexIndex]];

	uv = vertex.uv;

#ifdef FULLSCREEN_MAX_DEPTH
	gl_Position = vec4(vertex.position, MAX_DEPTH, 1.0);
#else
	gl_Position = vec4(vertex.position, MIN_DEPTH, 1.0);
#endif
}
@end
