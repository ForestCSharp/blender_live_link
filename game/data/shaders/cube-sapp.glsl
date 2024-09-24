@ctype mat4 HMM_Mat4

@vs vs
uniform vs_params {
    mat4 vp;
};

// TODO: should probably just pass in a Model matrix...
struct ObjectData
{
	mat4 model_matrix;
};

readonly buffer ObjectDataBuffer {
	ObjectData object_data_array[];
};

in vec4 position;
in vec4 normal;

out vec4 color;

void main() {
    gl_Position = vp * object_data_array[0].model_matrix * position;
    color = normal;
}
@end

@fs fs
in vec4 color;
out vec4 frag_color;

void main() {
    frag_color = color;
}
@end

@program cube vs fs
