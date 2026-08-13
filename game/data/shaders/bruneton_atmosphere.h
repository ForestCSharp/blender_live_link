#ifndef BRUNETON_ATMOSPHERE_INCLUDED
#define BRUNETON_ATMOSPHERE_INCLUDED

// Vulkan/GLSL adaptation of Eric Bruneton's 2017 precomputed atmospheric
// scattering implementation. The upstream source and its BSD-3-Clause terms
// are retained in bruneton_definitions.h and bruneton_functions.h.

#define IN(x) x
#define OUT(x) out x
#define TEMPLATE(x)
#define TEMPLATE_ARGUMENT(x)
#define assert(x)

#define TRANSMITTANCE_TEXTURE_WIDTH 256
#define TRANSMITTANCE_TEXTURE_HEIGHT 64
#define SCATTERING_TEXTURE_R_SIZE 32
#define SCATTERING_TEXTURE_MU_SIZE 128
#define SCATTERING_TEXTURE_MU_S_SIZE 32
#define SCATTERING_TEXTURE_NU_SIZE 8
#define IRRADIANCE_TEXTURE_WIDTH 64
#define IRRADIANCE_TEXTURE_HEIGHT 16

#define COMBINED_SCATTERING_TEXTURES

#include "bruneton_definitions.h"

// Vulkan array textures do not interpolate between array layers. Reproduce
// sampler3D's normalized Z filtering explicitly while retaining hardware
// bilinear filtering within each 2D slice.
vec4 SampleScatteringTexture(sampler2DArray tex, vec3 uvw)
{
	float layer = clamp(uvw.z * float(SCATTERING_TEXTURE_R_SIZE) - 0.5,
		0.0, float(SCATTERING_TEXTURE_R_SIZE - 1));
	float layer0 = floor(layer);
	float layer1 = min(layer0 + 1.0, float(SCATTERING_TEXTURE_R_SIZE - 1));
	return mix(texture(tex, vec3(uvw.xy, layer0)),
		texture(tex, vec3(uvw.xy, layer1)), layer - layer0);
}

#include "bruneton_functions.h"

// Three-wavelength luminance path used by visible and probe sky rays. This is
// the Vulkan equivalent of the model's GetSkyLuminance API when precomputing
// radiance at the reference RGB wavelengths.
vec3 GetSkyLuminance(
	AtmosphereParameters atmosphere,
	sampler2D transmittance_texture,
	sampler2DArray scattering_texture,
	sampler2DArray single_mie_scattering_texture,
	vec3 camera,
	vec3 view_ray,
	vec3 sun_direction,
	out vec3 transmittance)
{
	const vec3 SKY_RADIANCE_TO_RGB =
		vec3(114974.916437, 71305.954816, 65310.548555) * 1.0e-5;
	return GetSkyRadiance(atmosphere, transmittance_texture, scattering_texture,
		single_mie_scattering_texture, camera, view_ray, 0.0, sun_direction,
		transmittance) * SKY_RADIANCE_TO_RGB;
}

// Converts the engine's Z-axis planet placement to Bruneton's planet-centered
// coordinate system. Observers below the surface are projected to a one-meter
// radius epsilon; observers above the atmosphere remain unmodified.
vec3 GetAtmosphereCameraPosition(
	AtmosphereParameters atmosphere,
	vec3 world_position,
	float planet_center_z)
{
	vec3 camera = world_position - vec3(0.0, 0.0, planet_center_z);
	float radius = length(camera);
	float minimum_radius = atmosphere.bottom_radius + 1.0;
	if (radius < minimum_radius)
	{
		vec3 radial_direction = radius > 1.0e-3
			? camera / radius : vec3(0.0, 0.0, 1.0);
		camera = radial_direction * minimum_radius;
	}
	return camera;
}

#endif
