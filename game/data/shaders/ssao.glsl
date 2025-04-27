@ctype mat4 HMM_Mat4

@vs vs

out vec2 uv;

struct Vertex
{
	vec2 position;
	vec2 uv;
};

void main()
{
	Vertex vertices[4] = Vertex[4](
		Vertex(vec2(-1.0, -1.0), vec2(0.0, 1.0)),
		Vertex(vec2(1.0, -1.0), vec2(1.0, 1.0)),
		Vertex(vec2(-1.0, 1.0), vec2(0.0, 0.0)),
		Vertex(vec2(1.0, 1.0), vec2(1.0, 0.0))
	);

	int indices[6] = {0,1,2,1,2,3};

	Vertex vertex = vertices[indices[gl_VertexIndex]];

	uv = vertex.uv;
	gl_Position = vec4(vertex.position, 0, 1);
}
@end

@fs fs

layout(binding=0) uniform texture2D tex;
layout(binding=0) uniform sampler smp;

in vec2 uv;

out vec4 frag_color;

void main()
{
 	vec4 sampled_color = texture(sampler2D(tex, smp), uv);
    frag_color = sampled_color;
}

@end

@program ssao vs fs
