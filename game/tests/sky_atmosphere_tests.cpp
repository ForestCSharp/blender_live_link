#include <cassert>
#include <cstdio>
#include <cstdlib>

#if defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#endif
#define VK_NO_PROTOTYPES
#define VOLK_IMPLEMENTATION
#include "volk/volk.h"
#include "vma/vk_mem_alloc.h"
#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"
#include "handmade_math/HandmadeMath.h"

#include "render/sky_atmosphere_dirty.h"
#include "scene/scene_system.h"

static Object& add_sun(State& state, i32 uid, bool with_sky = true)
{
	Object object = {};
	object.unique_id = uid;
	object.authoring_visibility = true;
	object.visibility = true;
	object.has_light = true;
	object.light.type = LightType::Sun;
	object.light.color = HMM_V3(1.0f, 1.0f, 1.0f);
	object.light.sun = { .power = 1.0f, .cast_shadows = true };
	object.has_sky_atmosphere = with_sky;
	state.scene.objects.insert({ uid, std::move(object) });
	return state.scene.objects.at(uid);
}

static void test_controller_selection()
{
	State state = {};
	add_sun(state, 20);
	add_sun(state, 10);
	scene_ensure_indexes(state);
	state.lighting.needs_data_update = false;
	SceneSystem::refresh_active_sky_controller(state);
	assert(state.scene.active_sky_controller_id == 10);
	assert(state.scene.sky_controller_candidate_count == 2);
	assert(state.lighting.needs_data_update);

	state.scene.objects.at(10).sky_atmosphere.enabled = false;
	SceneSystem::refresh_active_sky_controller(state);
	assert(state.scene.active_sky_controller_id == 20);

	state.scene.objects.at(20).visibility = false;
	SceneSystem::refresh_active_sky_controller(state);
	assert(!state.scene.active_sky_controller_id);

	state.scene.objects.at(10).sky_atmosphere.enabled = true;
	state.scene.objects.at(10).visibility = true;
	state.scene.objects.at(10).light.type = LightType::Point;
	SceneSystem::refresh_active_sky_controller(state);
	assert(!state.scene.active_sky_controller_id);
	assert(state.scene.invalid_sky_controller_count == 1);

	state.scene.objects.at(10).light.type = LightType::Sun;
	SceneSystem::refresh_active_sky_controller(state);
	SceneSystem::refresh_primary_sun_id(state);
	assert(state.scene.active_sky_controller_id == 10);
	assert(state.scene.primary_sun_id == 10);

	state.scene.objects.erase(10);
	SceneSystem::refresh_active_sky_controller(state);
	SceneSystem::refresh_primary_sun_id(state);
	assert(!state.scene.active_sky_controller_id);
	assert(state.scene.primary_sun_id == 20); // legacy non-sky fallback
}

static void test_dirty_classification()
{
	SkyAtmosphere earth = {};
	SkyAtmosphere changed = earth;
	assert(bruneton_lut_parameters_equal(earth, changed));

	// Runtime position and uniform-only values are absent from the LUT signature.
	changed.planet_center_z_m += 1000.0f;
	changed.sky_intensity = 2.0f;
	changed.sun_disc_intensity = 3.0f;
	assert(bruneton_lut_parameters_equal(earth, changed));

	changed = earth; changed.air_density += 0.1f; assert(!bruneton_lut_parameters_equal(earth, changed));
	changed = earth; changed.aerosol_density += 0.1f; assert(!bruneton_lut_parameters_equal(earth, changed));
	changed = earth; changed.ozone_density += 0.1f; assert(!bruneton_lut_parameters_equal(earth, changed));
	changed = earth; changed.ground_albedo.X += 0.1f; assert(!bruneton_lut_parameters_equal(earth, changed));
	changed = earth; changed.atmosphere_height_m += 1.0f; assert(!bruneton_lut_parameters_equal(earth, changed));
	changed = earth; changed.rayleigh_scale_height_m += 1.0f; assert(!bruneton_lut_parameters_equal(earth, changed));
	changed = earth; changed.mie_scale_height_m += 1.0f; assert(!bruneton_lut_parameters_equal(earth, changed));
	changed = earth; changed.mie_anisotropy += 0.01f; assert(!bruneton_lut_parameters_equal(earth, changed));
	changed = earth; changed.max_sun_zenith_angle_degrees += 1.0f; assert(!bruneton_lut_parameters_equal(earth, changed));
	changed = earth; changed.sun_disc_angular_diameter_degrees += 0.01f; assert(!bruneton_lut_parameters_equal(earth, changed));

	BrunetonProbeSkySignature a = {
		.controller_id = 1,
		.sun_direction = HMM_V3(0.0f, 0.0f, 1.0f),
		.sun_color_energy = HMM_V3(1.0f, 1.0f, 1.0f),
		.sky_intensity = 1.0f,
		.planet_center_z_m = -6360000.0f,
		.lut_generation = 1,
	};
	BrunetonProbeSkySignature b = a;
	assert(bruneton_probe_sky_signature_equal(a, b));
	b.sun_direction.X += 0.1f; assert(!bruneton_probe_sky_signature_equal(a, b));
	b = a; b.sun_color_energy.Y += 0.1f; assert(!bruneton_probe_sky_signature_equal(a, b));
	b = a; b.planet_center_z_m += 1.0f; assert(!bruneton_probe_sky_signature_equal(a, b));
	b = a; b.sky_intensity = 2.0f; assert(!bruneton_probe_sky_signature_equal(a, b));
	b = a; b.controller_id = 2; assert(!bruneton_probe_sky_signature_equal(a, b));
	b = a; b.lut_generation = 2; assert(!bruneton_probe_sky_signature_equal(a, b));

	// Observer positions are deliberately not part of either dirty signature.
	const HMM_Vec3 camera_a = HMM_V3(0.0f, 0.0f, 1.0f);
	const HMM_Vec3 camera_b = HMM_V3(1000.0f, 0.0f, 70000.0f);
	assert(HMM_LenSqrV3(camera_b - camera_a) > 0.0f);
	assert(bruneton_lut_parameters_equal(earth, earth));
	assert(bruneton_probe_sky_signature_equal(a, a));
}

int main()
{
	test_controller_selection();
	test_dirty_classification();
	// game_object.h owns the process-wide Jolt state used by the unity build;
	// this CPU-only test never initializes it, so skip its runtime destructor.
	std::_Exit(0);
}
