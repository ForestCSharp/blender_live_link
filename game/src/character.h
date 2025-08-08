#pragma once

#include "physics_system.h"
#include <Jolt/Physics/Character/Character.h>

//FCS TODO: NEXT UP: Draw Debug Representation
//FCS TODO: NEXT UP: Character Movement
//FCS TODO: NEXT UP: Camera Settings

struct CharacterSettings
{
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
	JPH::CharacterSettings* jph_character_settings = new JPH::CharacterSettings();

	JPH::Character* jph_character = new JPH::Character(
		jph_character_settings, 
		JPH::RVec3::sZero(), 
		JPH::Quat::sIdentity(), 
		0, 
		&in_jolt_state.physics_system
	);

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
