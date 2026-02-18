// Common Shader Code
#include "shader_common.h"
#include "octahedral_helpers.h"
#include "gi_helpers.h"

@vs vs

@include_block gi_helpers

layout(binding=0) uniform vs_params {
    mat4 view;
	mat4 projection;
};

layout(location = 0) in vec4 position;
layout(location = 1) in vec4 normal;
layout(location = 2) in vec2 texcoord;

out vec4 world_position;
out vec4 world_normal;
out vec2 pixel_texcoord;
out flat int probe_index;

mat4 mat4_translate(vec3 offset) {
    return mat4(
        1.0, 0.0, 0.0, 0.0, // Column 1
        0.0, 1.0, 0.0, 0.0, // Column 2
        0.0, 0.0, 1.0, 0.0, // Column 3
        offset.x, offset.y, offset.z, 1.0  // Column 4
    );
}

void main() {

	vec3 probe_position = gi_probe_position_from_index(gl_InstanceIndex);
	mat4 model = mat4_translate(probe_position);

	world_position = model * position;
	world_normal = normal;
	pixel_texcoord = texcoord;

	probe_index = gl_InstanceIndex;

    gl_Position = projection * view * world_position;
}
@end

@fs fs

@include_block material
@include_block octahedral_helpers
@include_block gi_helpers
@include_block remap

layout(binding=1) uniform fs_params {
	int atlas_total_size;
	int atlas_entry_size;
	int probe_vis_mode;
};

layout(binding=0) uniform sampler linear_sampler;
layout(binding=1) uniform sampler nearest_sampler;

layout(binding=0) uniform texture2D octahedral_atlas_texture;
layout(binding=1) uniform texture2D octahedral_depth_texture;

layout(binding=2) readonly buffer ProbesBuffer {
	GI_Probe gi_probes[];
};

in vec4 world_position;
in vec4 world_normal;
in vec2 pixel_texcoord;
in flat int probe_index;

out vec4 out_color;
out vec4 out_position;
out vec4 out_normal;
out vec4 out_roughness_metallic_emissive;

void main()
{
	GI_Probe probe = gi_probes[probe_index];

	const vec3 sample_dir = normalize(world_normal).xyz;
	const vec2 octahedral_coords = padded_atlas_uv_from_normal(sample_dir, probe.atlas_idx, atlas_total_size, atlas_entry_size);

	switch (probe_vis_mode)
	{
		case 0:
		{
			out_color = texture(sampler2D(octahedral_atlas_texture,linear_sampler), octahedral_coords);
			break;
		}
		case 1:
		{
			const float radial_depth = texture(sampler2D(octahedral_depth_texture, nearest_sampler), octahedral_coords).x;
			const float adjusted_depth = remap_clamped(radial_depth, 0.0, 1000.0, 0.0, 1.0);
			out_color = vec4(vec3(adjusted_depth), 1.0);
			break;
		}
		default:
		{
			out_color = vec4(0,0,0,0);
		}
	}

	out_roughness_metallic_emissive = vec4(1,0,1,0);
	out_position = world_position; 
	out_normal = normalize(world_normal);
}
@end

@program gi_debug vs fs
