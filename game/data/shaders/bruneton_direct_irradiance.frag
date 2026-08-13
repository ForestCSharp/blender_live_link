#version 450
#include "bruneton_parameters.h"
layout(set = 0, binding = 1) uniform sampler2D transmittance_texture;
layout(location = 0) out vec4 out_delta_irradiance;
layout(location = 1) out vec4 out_irradiance;
void main()
{
	vec3 value = ComputeDirectIrradianceTexture(
		GetAtmosphere(), transmittance_texture, gl_FragCoord.xy);
	out_delta_irradiance = vec4(value, 1.0);
	out_irradiance = vec4(0.0);
}
