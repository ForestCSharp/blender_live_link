// Common Shader Code
#include "shader_common.h"

@vs vs

layout(binding=0) uniform vs_params {
    mat4 view;
	mat4 projection;
	mat4 model;
	mat4 rotation;
};

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 normal;
layout(location = 2) in vec2 texcoord;

out vec4 world_position;
out vec4 world_normal;
out vec2 pixel_texcoord;

void main() {
	world_position = model * position;
	world_normal = rotation * normal;
	pixel_texcoord = texcoord;

    gl_Position = projection * view * world_position;
}
@end

@fs fs
@include_block material

layout(binding=0) uniform sampler smp;

layout(binding=0) uniform textureCube cubemap_color_texture;

in vec4 world_position;
in vec4 world_normal;
in vec2 pixel_texcoord;

out vec4 out_color;
out vec4 out_position;
out vec4 out_normal;
out vec4 out_roughness_metallic_emissive;

void main()
{
	out_color = vec4(1,1,1,1);
	out_roughness_metallic_emissive = vec4(1,0,1,0);

	vec3 cubemap_dir = normalize(world_normal).xyz;
	cubemap_dir.z = -cubemap_dir.z; // LH -> RH coordinate system

	out_color = texture(samplerCube(cubemap_color_texture,smp), cubemap_dir);
	//TODO: out_roughness_metallic_emissive (need additional cubemap textures...)
	
	out_position = world_position; 
	out_normal = normalize(world_normal);
}
@end

@program cubemap_debug vs fs
