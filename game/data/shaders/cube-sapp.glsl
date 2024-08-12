@ctype mat4 hmm_mat4

@vs vs
uniform vs_params {
    mat4 vp;
};

struct ObjectData
{
	vec4 location;
};

readonly buffer ObjectDataBuffer {
	ObjectData object_data_array[];
};

in vec4 position;
in vec4 color0;

out vec4 color;

mat4 BuildTranslation(vec4 in_translation)
{
    return mat4(
        vec4(1.0, 0.0, 0.0, 0.0),
        vec4(0.0, 1.0, 0.0, 0.0),
        vec4(0.0, 0.0, 1.0, 0.0),
        vec4(in_translation.x, in_translation.y, in_translation.z, 1.0));
}

void main() {
    gl_Position = vp * BuildTranslation(object_data_array[0].location) * position;
    color = color0;
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
