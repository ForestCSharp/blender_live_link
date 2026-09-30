#pragma once

#include <algorithm>
#include <cmath>

#include "game_object/mech.h"

constexpr f32 PROJECTILE_RADIUS_M = 0.15f;
constexpr f32 PROJECTILE_MASS_KG = 0.1f;
constexpr f32 PROJECTILE_SPEED_M_S = 30.0f;
constexpr f32 PROJECTILE_LIFETIME_S = 5.0f;
constexpr size_t MAX_ACTIVE_PROJECTILES = 64;

void projectile_remove_oldest()
{
	if (state.projectiles.empty()) return;
	scene_remove_object(state, state.projectiles.front().object_uid);
	state.projectiles.erase(state.projectiles.begin());
}

void projectile_update_lifetimes(f32 delta_seconds)
{
	if (!state.runtime.is_simulating) return;
	for (auto& projectile : state.projectiles) projectile.age_seconds += std::max(0.0f, delta_seconds);
	while (!state.projectiles.empty() && state.projectiles.front().age_seconds >= PROJECTILE_LIFETIME_S)
		projectile_remove_oldest();
}

bool projectile_spawn(HMM_Vec3 origin, HMM_Vec3 direction)
{
	const f32 length_sq = HMM_DotV3(direction, direction);
	if (!std::isfinite(length_sq) || length_sq < 1e-8f) return false;
	direction = HMM_MulV3F(direction, 1.0f / sqrtf(length_sq));
	while (state.projectiles.size() >= MAX_ACTIVE_PROJECTILES) projectile_remove_oldest();

	const i32 uid = mech_allocate_runtime_object_uid();
	Object sphere = object_create(uid, strdup("Projectile"), true,
		HMM_V4(origin.X, origin.Y, origin.Z, 1.0f), HMM_Q(0, 0, 0, 1), HMM_V3(1, 1, 1));
	sphere.storage_kind = ObjectStorageKind::RuntimeProjectile;
	sphere.has_mesh = true;
	sphere.mesh = make_mesh(mesh_init_data_uv_sphere(PROJECTILE_RADIUS_M, 12, 16));
	sphere.has_rigid_body = true;
	sphere.rigid_body.is_dynamic = true;
	sphere.rigid_body.mass = PROJECTILE_MASS_KG;

	JPH::SphereShapeSettings shape_settings(PROJECTILE_RADIUS_M);
	const auto shape_result = shape_settings.Create();
	JPH::BodyCreationSettings settings(shape_result.Get(),
		JPH::RVec3(origin.X, origin.Y, origin.Z), JPH::Quat::sIdentity(),
		JPH::EMotionType::Dynamic, Layers::MOVING);
	JPH::MassProperties mass;
	mass.ScaleToMass(PROJECTILE_MASS_KG);
	settings.mMassPropertiesOverride = mass;
	settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
	JPH::BodyInterface& bodies = jolt_state.physics_system.GetBodyInterface();
	sphere.rigid_body.jolt_body = bodies.CreateBody(settings);
	if (!sphere.rigid_body.jolt_body)
	{
		object_cleanup(sphere);
		return false;
	}
	bodies.AddBody(sphere.rigid_body.jolt_body->GetID(), JPH::EActivation::Activate);
	bodies.SetLinearVelocity(sphere.rigid_body.jolt_body->GetID(),
		JPH::Vec3(direction.X, direction.Y, direction.Z) * PROJECTILE_SPEED_M_S);
	scene_insert_or_replace_object(state, std::move(sphere));
	state.projectiles.push_back({uid, 0.0f});
	return true;
}

void projectile_consume_fire_requests()
{
	const i32 requests = state.input.pending_fire_requests;
	state.input.pending_fire_requests = 0;
	if (!state.runtime.is_simulating || requests <= 0 || !state.scene.player_character_id) return;
	auto mech_id = state.mech.character_to_instance.find(*state.scene.player_character_id);
	if (mech_id == state.mech.character_to_instance.end()) return;
	auto mech = state.mech.instances.find(mech_id->second);
	if (mech == state.mech.instances.end()) return;
	for (i32 press = 0; press < requests; ++press)
	{
		for (const MechWeaponInstance& equipped : mech->second.weapons)
		{
			auto instance = state.scene.objects.find(equipped.instance_uid);
			auto templ = state.scene.objects.find(equipped.weapon_template_uid);
			if (instance == state.scene.objects.end() || !instance->second.visibility ||
				templ == state.scene.objects.end() || !templ->second.has_weapon ||
				!templ->second.weapon.muzzle_valid) continue;
			const HMM_Mat4 muzzle = HMM_MulM4(object_get_model_matrix(instance->second),
				templ->second.weapon.muzzle_local_transform);
			const HMM_Vec3 origin = HMM_V3(muzzle.Elements[3][0], muzzle.Elements[3][1], muzzle.Elements[3][2]);
			const HMM_Vec3 direction = HMM_V3(muzzle.Elements[1][0], muzzle.Elements[1][1], muzzle.Elements[1][2]);
			if (std::isfinite(origin.X) && std::isfinite(origin.Y) && std::isfinite(origin.Z))
				projectile_spawn(origin, direction);
		}
	}
}
