
// Common Shader Code
#include "shader_common.h"

// Fullscreen Vertex Shader
#include "fullscreen_vs.glslh"

@fs fs

// Textures
layout(binding=0) uniform texture2D color_tex;

// Samplers
layout(binding=0) uniform sampler tex_sampler;

// Uniform Struct
layout(binding=0) uniform fs_params {
	vec2 screen_size;
	int dilate_size;
	float dilate_separation;
};

// Fragment Inputs
in vec2 uv;

// Fragment Outputs
out vec4 frag_color;

// Based on: https://lettier.github.io/3d-game-shaders-for-beginners/dilation.html
void main()
{	
	vec2 texel_size = 1.0 / screen_size;
	vec4 in_color = texture(sampler2D(color_tex, tex_sampler), uv);

	float mx = 0.0;
  	vec4 cmx = frag_color;

  	for (int i = -dilate_size; i <= dilate_size; ++i)
	{
    	for (int j = -dilate_size; j <= dilate_size; ++j)
		{
			// For a rectangular shape.
			//if (false);

			// For a diamond shape;
			//if (!(abs(i) <= dilate_size - abs(j))) { continue; }

			// For a circular shape.
			if (!(distance(vec2(i, j), vec2(0, 0)) <= dilate_size)) { continue; }
			
			//FCS TODO: hexagonal shape 

			vec2 offset = vec2(i, j) * dilate_separation * texel_size;
			vec4 c = texture(sampler2D(color_tex, tex_sampler), uv + offset);

			float mxt = dot(c.rgb, vec3(0.3, 0.59, 0.11));

			//FCS TODO: Compare world positions and only set if within some threshold?
			if (mxt > mx)
			{
				mx = mxt;
				cmx = c;
			}
		}
	}

	const float min_threshold = 0.2;
	const float max_threshold = 0.5;

	frag_color.rgb = mix(in_color.rgb ,cmx.rgb, smoothstep(min_threshold, max_threshold, mx));
	frag_color.a = 1.0;
}

@end

@program dilate vs fs
