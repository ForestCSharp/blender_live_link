#version 450
#include "bruneton_parameters.h"
layout(set = 0, binding = 1) uniform sampler2D transmittance_texture;
layout(push_constant) uniform PushConstants { int layer; int scattering_order; } pc;
layout(location = 0) out vec4 out_delta_rayleigh;
layout(location = 1) out vec4 out_delta_mie;
layout(location = 2) out vec4 out_scattering;
void main()
{
	vec3 rayleigh;
	vec3 mie_value;
	ComputeSingleScatteringTexture(GetAtmosphere(), transmittance_texture,
		vec3(gl_FragCoord.xy, float(pc.layer) + 0.5), rayleigh, mie_value);
	out_delta_rayleigh = vec4(rayleigh, 1.0);
	out_delta_mie = vec4(mie_value, 1.0);
	out_scattering = vec4(rayleigh, mie_value.r);
}
