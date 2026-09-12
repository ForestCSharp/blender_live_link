// Headless reference using the actual game/ controller, without modifying it.
#include <cstdio>
#include <thread>
#include "game_object/character.h"
int main() {
  jolt_init();
  CharacterSettings settings{};
  settings.initial_location=HMM_V4(0,0,100,1);
  settings.initial_rotation=HMM_Q(0,0,0,1);
  settings.height=2;settings.radius=.5;settings.move_speed=20;settings.jump_speed=10;
  Character character=character_create(jolt_state,settings);
  character.jph_character->SetLinearVelocity(JPH::Vec3(1,2,3));
  character_move(character,HMM_V3(0,3,0),true,1.0f/60);
  auto v=character.jph_character->GetLinearVelocity();
  character_turn_heading(character.body_rotation,HMM_V3(1,0,0),1.0f/60);
  printf("REFERENCE velocity %.9f %.9f %.9f heading %.9f %.9f\n",v.GetX(),v.GetY(),v.GetZ(),character.body_rotation.Z,character.body_rotation.W);
  character.jph_character->SetLinearVelocity(JPH::Vec3(0,0,0));
  for(int i=0;i<60;i++) {character_move(character,HMM_V3(0,1,0),false,1.0f/60);jolt_update(1.0f/60);}
  auto p=character.jph_character->GetPosition();
  printf("REFERENCE position %.9f %.9f %.9f\n",p.GetX(),p.GetY(),p.GetZ());
  character_destroy(character);
}
