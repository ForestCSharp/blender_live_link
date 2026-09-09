#pragma once

#include <cmath>

#include "core/types.h"

// Keep these constants synchronized with bruneton_atmosphere.h. Sun energy is
// top-of-atmosphere irradiance in W/m^2; 1361 W/m^2 is the Earth reference.
inline constexpr f32 EARTH_TOA_SOLAR_IRRADIANCE_W_M2 = 1361.0f;
inline constexpr f32 SCENE_PHOTOMETRIC_SCALE = 1.0e-5f;

inline f32 solar_irradiance_scale(f32 irradiance_w_m2)
{
	return std::isfinite(irradiance_w_m2)
		? MAX(irradiance_w_m2, 0.0f) / EARTH_TOA_SOLAR_IRRADIANCE_W_M2
		: 0.0f;
}

inline HMM_Vec3 scene_solar_irradiance(HMM_Vec3 tint, f32 irradiance_w_m2)
{
	constexpr HMM_Vec3 reference_scene_irradiance = {
		1.474000f * 98242.786222f * SCENE_PHOTOMETRIC_SCALE,
		1.850400f * 69954.398112f * SCENE_PHOTOMETRIC_SCALE,
		1.911980f * 66475.012354f * SCENE_PHOTOMETRIC_SCALE,
	};
	const f32 scale = solar_irradiance_scale(irradiance_w_m2);
	return HMM_V3(
		MAX(std::isfinite(tint.X) ? tint.X : 0.0f, 0.0f)
			* reference_scene_irradiance.X * scale,
		MAX(std::isfinite(tint.Y) ? tint.Y : 0.0f, 0.0f)
			* reference_scene_irradiance.Y * scale,
		MAX(std::isfinite(tint.Z) ? tint.Z : 0.0f, 0.0f)
			* reference_scene_irradiance.Z * scale);
}
