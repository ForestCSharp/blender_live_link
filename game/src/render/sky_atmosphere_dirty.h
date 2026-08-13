#pragma once

#include "core/types.h"
#include "game_object/game_object.h"

// CPU-only dirty signatures, kept separate from Vulkan execution so their
// classification contract can be tested without a GPU.
inline bool bruneton_lut_parameters_equal(const SkyAtmosphere& a, const SkyAtmosphere& b)
{
	return a.air_density == b.air_density
		&& a.aerosol_density == b.aerosol_density
		&& a.ozone_density == b.ozone_density
		&& a.ground_albedo.X == b.ground_albedo.X
		&& a.ground_albedo.Y == b.ground_albedo.Y
		&& a.ground_albedo.Z == b.ground_albedo.Z
		&& a.sun_disc_angular_diameter_degrees == b.sun_disc_angular_diameter_degrees
		&& a.atmosphere_height_m == b.atmosphere_height_m
		&& a.rayleigh_scale_height_m == b.rayleigh_scale_height_m
		&& a.mie_scale_height_m == b.mie_scale_height_m
		&& a.mie_anisotropy == b.mie_anisotropy
		&& a.max_sun_zenith_angle_degrees == b.max_sun_zenith_angle_degrees;
}

struct BrunetonProbeSkySignature
{
	i32 controller_id = -1;
	HMM_Vec3 sun_direction = HMM_V3(0.0f, 0.0f, 0.0f);
	HMM_Vec3 sun_color_energy = HMM_V3(0.0f, 0.0f, 0.0f);
	f32 sky_intensity = -1.0f;
	f32 planet_center_z_m = 0.0f;
	u64 lut_generation = 0;
};

inline bool bruneton_probe_sky_signature_equal(
	const BrunetonProbeSkySignature& a,
	const BrunetonProbeSkySignature& b)
{
	return a.controller_id == b.controller_id
		&& HMM_LenSqrV3(a.sun_direction - b.sun_direction) <= 1.0e-8f
		&& HMM_LenSqrV3(a.sun_color_energy - b.sun_color_energy) <= 1.0e-8f
		&& a.sky_intensity == b.sky_intensity
		&& a.planet_center_z_m == b.planet_center_z_m
		&& a.lut_generation == b.lut_generation;
}
