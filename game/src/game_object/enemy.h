#pragma once

#include <cmath>
#include <random>

#include "handmade_math/HandmadeMath.h"
#include "core/types.h"

struct EnemySettings
{
	f32 roam_radius = 15.0f;
	f32 engage_range = 40.0f;
};

// Runtime brain state; rebuilt whenever Blender re-sends the object.
struct Enemy
{
	EnemySettings settings;
	HMM_Vec3 wander_target = {0, 0, 0};
	bool has_wander_target = false;
	f32 pause_seconds = 0.0f;
	f32 fire_cooldown_seconds = 0.0f;
	std::mt19937 rng;
};

// Uniform point in the horizontal disc of in_radius around in_home.
HMM_Vec3 enemy_pick_wander_target(HMM_Vec3 in_home, f32 in_radius, std::mt19937& io_rng)
{
	std::uniform_real_distribution<f32> unit(0.0f, 1.0f);
	const f32 distance = MAX(in_radius, 0.0f) * sqrtf(unit(io_rng));
	const f32 angle = 2.0f * HMM_PI32 * unit(io_rng);
	return HMM_V3(in_home.X + distance * cosf(angle), in_home.Y + distance * sinf(angle), in_home.Z);
}
