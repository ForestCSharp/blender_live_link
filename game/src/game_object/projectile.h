#pragma once

#include <algorithm>
#include <cmath>

#include "game_object/mech.h"

constexpr f32 PROJECTILE_RADIUS_M = 0.15f;
constexpr f32 PROJECTILE_MASS_KG = 0.1f;
constexpr f32 PROJECTILE_SPEED_M_S = 30.0f;
constexpr f32 PROJECTILE_LIFETIME_S = 5.0f;
constexpr size_t MAX_ACTIVE_PROJECTILES = 64;
// Negative so it can't collide with Blender material ids
constexpr i32 PROJECTILE_MATERIAL_ID = -1000;

// Registers the projectile material on first use (and again after a scene
// reset clears materials). Returns its index, or -1 if the table is full.
i32 projectile_material_index()
{
	auto found = state.materials.id_to_index.find(PROJECTILE_MATERIAL_ID);
	if (found != state.materials.id_to_index.end())
	{
		return found->second;
	}
	if (state.materials.items.length() >= MAX_MATERIALS)
	{
		return -1;
	}
	const i32 index = (i32) state.materials.items.length();
	state.materials.items.add((Material) {
		.base_color = HMM_V4(1.0f, 0.4f, 0.05f, 1.0f),
		.emission_color = HMM_V4(1.0f, 0.4f, 0.05f, 1.0f),
		.metallic = 0.0f,
		.roughness = 0.5f,
		.emission_strength = 15.0f,
		.base_color_image_index = -1,
		.emission_color_image_index = -1,
		.metallic_image_index = -1,
		.roughness_image_index = -1,
	});
	state.materials.id_to_index[PROJECTILE_MATERIAL_ID] = index;
	// Appending only, so in-flight frames still read identical prefix bytes
	update_materials_buffer(state);
	return index;
}

void projectile_remove_oldest()
{
	if (state.projectiles.empty())
	{
		return;
	}
	scene_remove_object(state, state.projectiles.front().object_uid);
	state.projectiles.erase(state.projectiles.begin());
}

void projectile_update_lifetimes(f32 in_delta_seconds)
{
	if (!state.runtime.is_simulating)
	{
		return;
	}
	for (auto& projectile : state.projectiles)
	{
		projectile.age_seconds += std::max(0.0f, in_delta_seconds);
	}
	// Spawn order is age order, so expired projectiles are always at the front.
	while (!state.projectiles.empty() && state.projectiles.front().age_seconds >= PROJECTILE_LIFETIME_S)
	{
		projectile_remove_oldest();
	}
}

bool projectile_spawn(HMM_Vec3 in_origin, HMM_Vec3 in_direction)
{
	const f32 length_sq = HMM_LenSqrV3(in_direction);
	if (!std::isfinite(length_sq) || length_sq < 1e-8f)
	{
		return false;
	}
	const HMM_Vec3 direction = in_direction / sqrtf(length_sq);
	while (state.projectiles.size() >= MAX_ACTIVE_PROJECTILES)
	{
		projectile_remove_oldest();
	}

	const i32 uid = mech_allocate_runtime_object_uid();
	Object sphere = object_create(uid, strdup("Projectile"), true,
		HMM_V4V(in_origin, 1.0f), HMM_Q(0, 0, 0, 1), HMM_V3(1, 1, 1));
	sphere.storage_kind = ObjectStorageKind::RuntimeProjectile;
	sphere.has_mesh = true;
	sphere.mesh = make_mesh(mesh_init_data_uv_sphere(PROJECTILE_RADIUS_M, 12, 16));
	sphere.mesh.material_indices_count = 1;
	sphere.mesh.material_indices = (i32*) malloc(sizeof(i32));
	sphere.mesh.material_indices[0] = projectile_material_index();
	sphere.has_rigid_body = true;
	sphere.rigid_body.is_dynamic = true;
	sphere.rigid_body.mass = PROJECTILE_MASS_KG;

	JPH::SphereShapeSettings shape_settings(PROJECTILE_RADIUS_M);
	const auto shape_result = shape_settings.Create();
	JPH::BodyCreationSettings settings(shape_result.Get(),
		JPH::RVec3(in_origin.X, in_origin.Y, in_origin.Z), JPH::Quat::sIdentity(),
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
