// Common Shader Code
#include "shader_common.h"

@vs vs

layout(binding=0) uniform vs_params {
    mat4 view;
	mat4 projection;
};

@include_block object_data

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 normal;
layout(location = 2) in vec2 texcoord;

void main() {
	vec4 world_position = object_data_array[0].model_matrix * position;
    gl_Position = projection * view * world_position;
}
@end

@fs fs
void main() {
	// Depth-only shader has no outputs
}
@end

@program shadow_depth vs fs
