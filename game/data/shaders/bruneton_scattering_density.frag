#version 450
#include "bruneton_parameters.h"
layout(set = 0, binding = 1) uniform sampler2D transmittance_texture;
layout(set = 0, binding = 3) uniform sampler2DArray single_rayleigh_scattering_texture;
layout(set = 0, binding = 4) uniform sampler2DArray single_mie_scattering_texture;
layout(set = 0, binding = 5) uniform sampler2DArray multiple_scattering_texture;
layout(set = 0, binding = 7) uniform sampler2D irradiance_texture;
layout(push_constant) uniform PushConstants { int layer; int scattering_order; } pc;
layout(location = 0) out vec4 out_scattering_density;
void main()
{
	vec3 value = ComputeScatteringDensityTexture(GetAtmosphere(),
		transmittance_texture, single_rayleigh_scattering_texture,
		single_mie_scattering_texture, multiple_scattering_texture,
		irradiance_texture, vec3(gl_FragCoord.xy, float(pc.layer) + 0.5),
		pc.scattering_order);
	out_scattering_density = vec4(value, 1.0);
}
