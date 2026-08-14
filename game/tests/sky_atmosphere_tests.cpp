#include <cassert>
#include <cmath>
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
#include "render/solar_calibration.h"
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
	object.light.sun = {
		.power = EARTH_TOA_SOLAR_IRRADIANCE_W_M2,
		.cast_shadows = true,
	};
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

static void test_cloud_controller_selection()
{
	State state = {};
	Object& primary = add_sun(state, 10);
	primary.has_cloud_system = true;
	primary.cloud_system.enabled = true;
	primary.cloud_system.layer_count = 2;
	primary.cloud_system.layers[0].enabled = true;
	primary.cloud_system.layers[1].enabled = true;
	Object& secondary = add_sun(state, 20);
	secondary.has_cloud_system = true;
	secondary.cloud_system.enabled = true;
	secondary.cloud_system.layer_count = 1;
	secondary.cloud_system.layers[0].enabled = true;

	scene_ensure_indexes(state);
	SceneSystem::refresh_active_sky_controller(state);
	SceneSystem::refresh_active_cloud_controller(state);
	assert(state.scene.active_sky_controller_id == 10);
	assert(state.scene.active_cloud_controller_id == 10);
	assert(state.clouds.active);
	assert(state.clouds.active_layer_count == 2);
	assert(state.scene.invalid_cloud_controller_count == 1);

	state.scene.objects.at(10).visibility = false;
	SceneSystem::refresh_active_sky_controller(state);
	SceneSystem::refresh_active_cloud_controller(state);
	assert(state.scene.active_sky_controller_id == 20);
	assert(state.scene.active_cloud_controller_id == 20);

	state.scene.objects.at(20).cloud_system.layers[0].enabled = false;
	SceneSystem::refresh_active_cloud_controller(state);
	assert(!state.scene.active_cloud_controller_id);
	assert(!state.clouds.active);

	state.scene.objects.at(20).cloud_system.layers[0].enabled = true;
	state.scene.objects.at(20).light.type = LightType::Point;
	SceneSystem::refresh_active_sky_controller(state);
	SceneSystem::refresh_active_cloud_controller(state);
	assert(!state.scene.active_cloud_controller_id);
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

static void test_physical_sun_calibration()
{
	assert(solar_irradiance_scale(0.0f) == 0.0f);
	assert(solar_irradiance_scale(-1.0f) == 0.0f);
	assert(solar_irradiance_scale(NAN) == 0.0f);
	assert(solar_irradiance_scale(INFINITY) == 0.0f);
	assert(std::abs(solar_irradiance_scale(1361.0f) - 1.0f) < 1.0e-6f);
	assert(std::abs(solar_irradiance_scale(2722.0f) - 2.0f) < 1.0e-6f);
	const HMM_Vec3 reference = scene_solar_irradiance(
		HMM_V3(1.0f, 1.0f, 1.0f), 1361.0f);
	assert(reference.X > reference.Y && reference.Y > reference.Z);
	assert(reference.Z > 1.0f && reference.X < 2.0f);
}

int main()
{
	test_controller_selection();
	test_cloud_controller_selection();
	test_dirty_classification();
	test_physical_sun_calibration();
	// game_object.h owns the process-wide Jolt state used by the unity build;
	// this CPU-only test never initializes it, so skip its runtime destructor.
	std::_Exit(0);
}
