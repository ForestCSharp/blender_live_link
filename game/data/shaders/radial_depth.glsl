// Common Shader Code
#include "shader_common.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.h"

@fs fs

@include_block remap

layout(binding=0) uniform fs_params {
	mat4 inverse_view_projection;
	vec3 capture_location;
};

@image_sample_type depth_texture unfilterable_float
layout(binding=0) uniform texture2D depth_texture;

@sampler_type tex_sampler nonfiltering
layout(binding=0) uniform sampler tex_sampler;

in vec2 uv;

out float frag_color;

vec3 world_pos_from_depth(vec2 in_uv, float in_depth) {
    vec2 xy = in_uv * 2.0 - 1.0;

    // 2. Convert Raw Depth to NDC Z
    float z = in_depth;

    #if USE_INVERSE_DEPTH
        // If depth is inversed, we don't need to do anything special for 
        // [0, 1] ranges other than ensuring the math below respects 1->0.
        // However, if we are on GL, we still need to map [1, 0] to [1, -1]
        #if defined(SOKOL_GLCORE33) || defined(SOKOL_GLES3)
            z = in_depth * 2.0 - 1.0;
        #endif
    #else
        // Standard depth mapping
        #if defined(SOKOL_GLCORE33) || defined(SOKOL_GLES3)
            z = in_depth * 2.0 - 1.0;
        #endif
    #endif

    vec4 clip_pos = vec4(xy, z, 1.0);

    // 4. Transform to World Space
    vec4 world_pos_h = inverse_view_projection * clip_pos;

    // 5. Perspective Divide
    return world_pos_h.xyz / world_pos_h.w;
}

void main()
{
	const float depth = texture(sampler2D(depth_texture, tex_sampler), uv).x;

	const vec3 world_position = world_pos_from_depth(uv, depth);

    // Calculate Radial Depth
	// FCS TODO: can probably just get this from depth buffer and view/proj...

    const float radial_depth = length(world_position - capture_location);
    frag_color = radial_depth;
}

@end

@program radial_depth vs fs
