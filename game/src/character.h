#pragma once

#include "handmade_math/HandmadeMath.h"

#include "physics_system.h"
#include <Jolt/Physics/Character/Character.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

//FCS TODO: NEXT UP: Draw Debug Representation
//FCS TODO: NEXT UP: Character Movement
//FCS TODO: NEXT UP: Camera Settings

struct CharacterSettings
{
	HMM_Vec4 initial_location;
	bool player_controlled = false;
	float move_speed = 20.0f;
};

struct Character
{
	// Tweakable Settings
	CharacterSettings settings;

	// Jolt State
	JPH::CharacterSettings* jph_character_settings = nullptr;
	JPH::Character* jph_character = nullptr;
};

Character character_create(JoltState& in_jolt_state, const CharacterSettings& in_settings)
{
	// Create Character's Jolt Shape
	const float character_height = 1.35f;
	const float character_radius = 0.3f;
	JPH::Vec3 initial_location(in_settings.initial_location.X, in_settings.initial_location.Y, in_settings.initial_location.Z);

	JPH::RefConst<JPH::Shape> character_shape = JPH::RotatedTranslatedShapeSettings(
		initial_location, 
		JPH::Quat::sIdentity(), 
		new JPH::CapsuleShape(0.5f * character_height, character_radius)
	).Create().Get();

	// Create and Setup Jolt Character Settings
	JPH::CharacterSettings* jph_character_settings = new JPH::CharacterSettings();

	jph_character_settings->mMaxSlopeAngle = JPH::DegreesToRadians(45.0f);
	jph_character_settings->mLayer = Layers::MOVING;
	jph_character_settings->mShape = character_shape;
	jph_character_settings->mFriction = 0.5f;
	jph_character_settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -character_radius);

	// Create Jolt Character
	JPH::Character* jph_character = new JPH::Character(
		jph_character_settings, 
		JPH::RVec3::sZero(), 
		JPH::Quat::sIdentity(), 
		0, 
		&in_jolt_state.physics_system
	);

	// Add Character to our Physics Simulation
	jph_character->AddToPhysicsSystem(JPH::EActivation::Activate);

	Character out_character = {
		.settings = in_settings,
		.jph_character_settings = jph_character_settings,
		.jph_character = jph_character,
	};

	return out_character;
}

void character_destroy(Character& in_character)
{
	if (in_character.jph_character)
	{
		in_character.jph_character->RemoveFromPhysicsSystem();
		delete in_character.jph_character;
		in_character.jph_character = nullptr;
	}

	if (in_character.jph_character_settings)
	{
		delete in_character.jph_character_settings;
		in_character.jph_character_settings = nullptr;
	}
}
