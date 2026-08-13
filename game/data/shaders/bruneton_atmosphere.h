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

// Bruneton's RGB coefficients convert the three reference wavelengths to
// photometric RGB. They remain physical here; the renderer's scene scale is
// applied only where radiance or irradiance enters a scene-color target.
const vec3 BRUNETON_SKY_RADIANCE_TO_LUMINANCE =
	vec3(114974.916437, 71305.954816, 65310.548555);
const vec3 BRUNETON_SOLAR_RADIANCE_TO_LUMINANCE =
	vec3(98242.786222, 69954.398112, 66475.012354);
const vec3 BRUNETON_REFERENCE_SOLAR_IRRADIANCE =
	vec3(1.474000, 1.850400, 1.911980);
const float BRUNETON_SCENE_PHOTOMETRIC_SCALE = 1.0e-5;
const float EARTH_TOA_SOLAR_IRRADIANCE_W_M2 = 1361.0;
const float MAX_FINITE_SCENE_COLOR = 65000.0;

float GetSolarIrradianceScale(float irradiance_w_m2)
{
	return max(isnan(irradiance_w_m2) || isinf(irradiance_w_m2)
		? 0.0 : irradiance_w_m2, 0.0) / EARTH_TOA_SOLAR_IRRADIANCE_W_M2;
}

vec3 GetSceneSolarIrradiance(
	vec3 sun_tint,
	float irradiance_w_m2)
{
	return BRUNETON_REFERENCE_SOLAR_IRRADIANCE
		* BRUNETON_SOLAR_RADIANCE_TO_LUMINANCE
		* BRUNETON_SCENE_PHOTOMETRIC_SCALE
		* max(sun_tint, vec3(0.0))
		* GetSolarIrradianceScale(irradiance_w_m2);
}

vec3 SanitizeSceneColor(vec3 color)
{
	bvec3 invalid = bvec3(
		isnan(color.x) || isinf(color.x),
		isnan(color.y) || isinf(color.y),
		isnan(color.z) || isinf(color.z));
	return min(max(mix(color, vec3(0.0), invalid), vec3(0.0)),
		vec3(MAX_FINITE_SCENE_COLOR));
}

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
	return GetSkyRadiance(atmosphere, transmittance_texture, scattering_texture,
		single_mie_scattering_texture, camera, view_ray, 0.0, sun_direction,
		transmittance) * BRUNETON_SKY_RADIANCE_TO_LUMINANCE;
}

// Transmittance from an arbitrary world-space point toward the Sun. Inside
// the atmosphere this is Bruneton's finite-disc horizon-aware query. Outside,
// rays which miss the atmosphere stay unattenuated; intersecting rays begin at
// the top boundary and naturally return zero when the planet occults the Sun.
vec3 GetAtmosphereSunTransmittance(
	AtmosphereParameters atmosphere,
	sampler2D transmittance_texture,
	vec3 atmosphere_position,
	vec3 sun_direction)
{
	vec3 position = atmosphere_position;
	float radius = length(position);
	float minimum_radius = atmosphere.bottom_radius + 1.0;
	if (radius < minimum_radius)
	{
		vec3 radial = radius > 1.0e-3 ? position / radius : vec3(0.0, 0.0, 1.0);
		position = radial * minimum_radius;
		radius = minimum_radius;
	}
	vec3 direction = normalize(sun_direction);
	if (radius <= atmosphere.top_radius)
	{
		return GetTransmittanceToSun(
			atmosphere, transmittance_texture, radius,
			ClampCosine(dot(position, direction) / radius));
	}

	float projected = dot(position, direction);
	float discriminant = projected * projected - radius * radius
		+ atmosphere.top_radius * atmosphere.top_radius;
	if (discriminant < 0.0)
		return vec3(1.0);
	float root = sqrt(max(discriminant, 0.0));
	float entry_distance = -projected - root;
	float exit_distance = -projected + root;
	if (exit_distance <= 0.0)
		return vec3(1.0);
	if (entry_distance <= 0.0)
		return vec3(1.0);

	vec3 entry = position + direction * (entry_distance + 1.0);
	float entry_radius = ClampRadius(atmosphere, length(entry));
	return GetTransmittanceToSun(
		atmosphere, transmittance_texture, entry_radius,
		ClampCosine(dot(entry, direction) / entry_radius));
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
