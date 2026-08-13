#version 450
#include "bruneton_parameters.h"
layout(location = 0) out vec4 out_transmittance;
void main()
{
	out_transmittance = vec4(ComputeTransmittanceToTopAtmosphereBoundaryTexture(
		GetAtmosphere(), gl_FragCoord.xy), 1.0);
}
