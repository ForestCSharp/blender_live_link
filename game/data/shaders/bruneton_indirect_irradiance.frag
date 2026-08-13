#version 450
#include "bruneton_parameters.h"
layout(set = 0, binding = 3) uniform sampler2DArray single_rayleigh_scattering_texture;
layout(set = 0, binding = 4) uniform sampler2DArray single_mie_scattering_texture;
layout(set = 0, binding = 5) uniform sampler2DArray multiple_scattering_texture;
layout(push_constant) uniform PushConstants { int layer; int scattering_order; } pc;
layout(location = 0) out vec4 out_delta_irradiance;
layout(location = 1) out vec4 out_irradiance;
void main()
{
	vec3 value = ComputeIndirectIrradianceTexture(GetAtmosphere(),
		single_rayleigh_scattering_texture, single_mie_scattering_texture,
		multiple_scattering_texture, gl_FragCoord.xy, pc.scattering_order);
	out_delta_irradiance = vec4(value, 1.0);
	out_irradiance = vec4(value, 0.0);
}
