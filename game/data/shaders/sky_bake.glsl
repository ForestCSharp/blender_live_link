// Common Shader Code
#include "shader_common.h"

#include "octahedral_helpers.h"

#include "fullscreen_vs.h"

@fs fs

@include_block octahedral_helpers
@include_block dither_noise

#include "sky_atmosphere.h"

layout(binding=0) uniform fs_params {
	vec3 sun_dir;
};

in vec2 uv;

out vec4 out_color;

void main()
{
	const vec3 camera_position = vec3(0,0,0);
	const vec2 oct_uv = uv * 2.0 - 1.0;
	const vec3 view_dir = octahedral_decode(oct_uv);
	const float ray_length = INFINITY;
	const vec3 light_dir = normalize(sun_dir);
	const vec3 light_color = vec3(1,1,1);

	vec3 out_transmittance;
	const vec3 sky_color = IntegrateScattering(
		camera_position,
		view_dir, 
		ray_length, 
		light_dir, 
		light_color, 
		out_transmittance
	);  
    out_color = vec4(sky_color, 1.0) + dither_noise();
}

@end

@program sky_bake vs fs
