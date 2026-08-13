#version 450
#include "bruneton_parameters.h"
layout(set = 0, binding = 1) uniform sampler2D transmittance_texture;
layout(set = 0, binding = 6) uniform sampler2DArray scattering_density_texture;
layout(push_constant) uniform PushConstants { int layer; int scattering_order; } pc;
layout(location = 0) out vec4 out_delta_multiple_scattering;
layout(location = 2) out vec4 out_scattering;
void main()
{
	float nu;
	vec3 value = ComputeMultipleScatteringTexture(GetAtmosphere(),
		transmittance_texture, scattering_density_texture,
		vec3(gl_FragCoord.xy, float(pc.layer) + 0.5), nu);
	out_delta_multiple_scattering = vec4(value, 1.0);
	out_scattering = vec4(value / max(RayleighPhaseFunction(nu), 1.0e-6), 0.0);
}
