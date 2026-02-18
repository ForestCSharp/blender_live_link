
// Common Shader Code
#include "shader_common.h"

#include "octahedral_helpers.h"

// Fullscreen Vertex Shader, drawn at max depth
#define FULLSCREEN_MAX_DEPTH
#include "fullscreen_vs.h"

@fs fs

@include_block octahedral_helpers

#include "sky_atmosphere.h"

layout(binding=0) uniform fs_params {
	mat4 inv_view_proj;
	vec3 camera_position;
};

layout(binding=0) uniform texture2D octahedral_sky_texture;
layout(binding=0) uniform sampler tex_sampler;

in vec2 uv;

out vec4 out_color;
out vec4 out_position;
out vec4 out_normal;
out vec4 out_roughness_metallic_emissive;

void main()
{
	// Convert UV to NDC (Remapping Y to account for top-down UVs)
    vec4 clip_pos = vec4(
        uv.x * 2.0 - 1.0, 
        1.0 - uv.y * 2.0, 
        MAX_DEPTH, 
        1.0
    );

    // Transform to world space
    vec4 world_pos_h = inv_view_proj * clip_pos;
    
    // We normalize the result to get a unit vector for the sky dome
    vec3 view_dir = normalize(world_pos_h.xyz / world_pos_h.w);

	vec2 octahedral_coords = octahedral_encode(view_dir) * 0.5 + 0.5;
	const vec3 sky_color = texture(sampler2D(octahedral_sky_texture, tex_sampler), octahedral_coords).rgb;
    out_color = vec4(sky_color, 1.0);
	out_normal = vec4(0,0,0,0);
	out_roughness_metallic_emissive = vec4(0,0,1,0);
}

@end

@program sky_pass vs fs
