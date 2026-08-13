#ifndef BRUNETON_PARAMETERS_INCLUDED
#define BRUNETON_PARAMETERS_INCLUDED

#include "bruneton_atmosphere.h"

#ifndef BRUNETON_DESCRIPTOR_SET
#define BRUNETON_DESCRIPTOR_SET 0
#endif

layout(std140, set = BRUNETON_DESCRIPTOR_SET, binding = 0) uniform BrunetonAtmosphereBlock
{
	vec4 radii;                 // bottom, top, sun angular radius, mu_s_min
	vec4 rayleigh_scattering;   // rgb coefficient, inverse scale height
	vec4 mie_scattering;        // rgb coefficient, phase g
	vec4 mie_extinction;        // rgb coefficient, inverse scale height
	vec4 absorption_extinction; // rgb coefficient
	vec4 ground_albedo;         // rgb
	vec4 solar_irradiance;      // rgb
} bruneton;

AtmosphereParameters GetAtmosphere()
{
	AtmosphereParameters atmosphere;
	atmosphere.solar_irradiance = bruneton.solar_irradiance.rgb;
	atmosphere.sun_angular_radius = bruneton.radii.z;
	atmosphere.bottom_radius = bruneton.radii.x;
	atmosphere.top_radius = bruneton.radii.y;

	DensityProfileLayer empty_layer = DensityProfileLayer(0.0, 0.0, 0.0, 0.0, 0.0);
	atmosphere.rayleigh_density.layers[0] = empty_layer;
	atmosphere.rayleigh_density.layers[1] = DensityProfileLayer(
		0.0, 1.0, bruneton.rayleigh_scattering.w, 0.0, 0.0);
	atmosphere.rayleigh_scattering = bruneton.rayleigh_scattering.rgb;
	atmosphere.mie_density.layers[0] = empty_layer;
	atmosphere.mie_density.layers[1] = DensityProfileLayer(
		0.0, 1.0, bruneton.mie_extinction.w, 0.0, 0.0);
	atmosphere.mie_scattering = bruneton.mie_scattering.rgb;
	atmosphere.mie_extinction = bruneton.mie_extinction.rgb;
	atmosphere.mie_phase_function_g = bruneton.mie_scattering.w;

	// Approximate 300-Dobson-unit ozone profile: rises from 10km to 25km,
	// then falls to zero at 40km.
	atmosphere.absorption_density.layers[0] = DensityProfileLayer(
		25000.0, 0.0, 0.0, 1.0 / 15000.0, -2.0 / 3.0);
	atmosphere.absorption_density.layers[1] = DensityProfileLayer(
		0.0, 0.0, 0.0, -1.0 / 15000.0, 8.0 / 3.0);
	atmosphere.absorption_extinction = bruneton.absorption_extinction.rgb;
	atmosphere.ground_albedo = bruneton.ground_albedo.rgb;
	atmosphere.mu_s_min = bruneton.radii.w;
	return atmosphere;
}

#endif
