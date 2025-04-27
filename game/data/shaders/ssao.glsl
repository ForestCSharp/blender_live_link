@ctype mat4 HMM_Mat4

@vs vs

out vec4 color;

void main()
{
	vec2 quad_positions[4] = vec2[]
	(
		vec2(0,0),
		vec2(0,1),
		vec2(1,0),
		vec2(1,1)
	);

	gl_Position = vec4(quad_positions[gl_VertexIndex], 0, 1);
}
@end

@fs fs

//in vec4 world_position;
//in vec4 world_normal;
in vec4 color;

out vec4 frag_color;

void main()
{
	gl_FragColor = frag_color;
}

@end
