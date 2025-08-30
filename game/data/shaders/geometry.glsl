// Common Shader Code
#include "shader_common.h"

@vs vs

layout(binding=0) uniform vs_params {
    mat4 view;
	mat4 projection;
};

struct ObjectData
{
	mat4 model_matrix;
	mat4 rotation_matrix;
	int material_index;
	PADDING(3);
};

layout(binding=0) readonly buffer ObjectDataBuffer {
	ObjectData object_data_array[];
};

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 normal;

out vec4 world_position;
out vec4 world_normal;
out flat int material_index;

void main() {
	world_position = object_data_array[0].model_matrix * position;
	world_normal = object_data_array[0].rotation_matrix * normal;
	material_index = object_data_array[0].material_index;

    gl_Position = projection * view * world_position;
}
@end

@fs fs
@include_block material

layout(binding=1) readonly buffer MaterialDataBuffer {
	Material material_data_array[];
};

in vec4 world_position;
in vec4 world_normal;
in flat int material_index;

out vec4 out_color;
out vec4 out_position;
out vec4 out_normal;

void main()
{
	if (material_index >= 0)
	{	
		out_color = material_data_array[material_index].base_color;
	}
	else
	{
		out_color = vec4(1,0,1,1);
	}
	out_position = world_position; 
	out_normal = normalize(world_normal);
}
@end

@program geometry vs fs
