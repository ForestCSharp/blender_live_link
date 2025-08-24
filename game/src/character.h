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
	HMM_Quat initial_rotation;
	bool player_controlled = false;
	float move_speed = 20.0f;
	float jump_speed = 10.0f;
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
	const float character_height = 6.0f;
	const float character_radius = 0.5f;

	JPH::RefConst<JPH::Shape> character_shape = JPH::RotatedTranslatedShapeSettings(
		JPH::Vec3::sZero(), 
		JPH::Quat::sIdentity(), 
		new JPH::CapsuleShape(0.5f * character_height, character_radius)
	).Create().Get();

	// Create and Setup Jolt Character Settings
	JPH::CharacterSettings* jph_character_settings = new JPH::CharacterSettings();
	jph_character_settings->mMaxSlopeAngle = JPH::DegreesToRadians(45.0f);
	jph_character_settings->mLayer = Layers::MOVING;
	jph_character_settings->mShape = character_shape;
	jph_character_settings->mFriction = 0.5f;
	jph_character_settings->mUp = JPH::Vec3::sAxisZ();
	jph_character_settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisZ(), -1.0e10f);

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


	{	// Set Initial Position/Rotation

		JPH::Vec3 initial_location(
			in_settings.initial_location.X, 
			in_settings.initial_location.Y, 
			in_settings.initial_location.Z
		);

		JPH::Quat initial_rotation(
			in_settings.initial_rotation.X, 
			in_settings.initial_rotation.Y, 
			in_settings.initial_rotation.Z, 
			in_settings.initial_rotation.W
		);

		jph_character->SetPositionAndRotation(initial_location, initial_rotation);
	}

	// Return our Character struct
	return (Character) {
		.settings = in_settings,
		.jph_character_settings = jph_character_settings,
		.jph_character = jph_character,
	};
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

void character_move(Character& in_character, HMM_Vec3 in_move_vec, bool in_jump)
{
	JPH::Character* mCharacter = in_character.jph_character;

	JPH::Vec3 movement_direction = JPH::Vec3(in_move_vec.X, in_move_vec.Y, in_move_vec.Z);

	// Cancel movement in opposite direction of normal when touching something we can't walk up
	JPH::Character::EGroundState ground_state = mCharacter->GetGroundState();
	if (	ground_state == JPH::Character::EGroundState::OnSteepGround
		||	ground_state == JPH::Character::EGroundState::NotSupported)
	{
		JPH::Vec3 normal = mCharacter->GetGroundNormal();
		normal.SetY(0.0f);
		float dot = normal.Dot(movement_direction);
		if (dot < 0.0f)
		{
			movement_direction -= (dot * normal) / normal.LengthSq();
		}
	}

	// Update velocity
	JPH::Vec3 current_velocity = mCharacter->GetLinearVelocity();
	JPH::Vec3 desired_velocity = in_character.settings.move_speed * movement_direction;
	if (!desired_velocity.IsNearZero() || current_velocity.GetY() < 0.0f || !mCharacter->IsSupported())
	{
		desired_velocity.SetZ(current_velocity.GetZ());
	}

	JPH::Vec3 new_velocity = 0.75f * current_velocity + 0.25f * desired_velocity;

	// Jump
	if (in_jump && ground_state == JPH::Character::EGroundState::OnGround)
	{
		new_velocity += JPH::Vec3(0, 0, in_character.settings.jump_speed);
	}

	// Update the velocity
	mCharacter->SetLinearVelocity(new_velocity);
}
