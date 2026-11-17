/// @file character_animation.h
/// @brief Locomotion animation state machine driving skeletal animation from
/// CharacterStateComponent.
///
/// Purpose:
/// Maps physical character state (horizontal speed, grounded state, vertical velocity, jump/land
/// transitions) onto semantic skeletal animation clips (Idle, Walk, Run, Jump, InAir, Landing)
/// via smooth cross-fade blending without coupling physics to animation playback.
///
/// Ownership and Lifetime:
/// CharacterAnimationComponent holds configuration and lightweight state. Loaded Skeleton and
/// AnimationClip assets are owned by the World-scoped CharacterAnimationAssetCache singleton.
///
/// Thread-safety:
/// Simulation thread only during World progression.

#pragma once

#include "world/world.h"

#include <cstdint>
#include <string>

namespace eng::framework {

/// @enum LocomotionAnimState
/// @brief Semantic locomotion states for character skeletal animation.
enum class LocomotionAnimState : uint8_t {
    Idle = 0,
    Walk = 1,
    Run = 2,
    Jump = 3,
    InAir = 4,
    Landing = 5
};

/// @struct CharacterAnimationComponent
/// @brief Configurable skeletal animation and locomotion blending parameters for a character.
struct CharacterAnimationComponent {
    std::string skeletonMeshPath{"assets/characters/player/Superhero_Male_FullBody.gltf"};
    std::string animationLibraryPath{"assets/animations/player/UAL1_Standard.glb"};

    std::string idleClip{"Idle_Loop"};
    std::string walkClip{"Walk_Loop"};
    std::string runClip{"Jog_Fwd_Loop"};
    std::string jumpClip{"Jump_Start"};
    std::string inAirClip{"Jump_Loop"};
    std::string landClip{"Jump_Land"};

    float walkSpeedThreshold{0.25F};
    float runSpeedThreshold{3.20F};
    float blendDurationSeconds{0.14F};
    float landingDurationSeconds{0.22F};

    // Runtime state (reset on Play)
    LocomotionAnimState currentState{LocomotionAnimState::Idle};
    float landingTimerSeconds{0.0F};
    bool wasGrounded{true};
    bool initialized{false};
    const void* cachedBundle{nullptr};
};

/// Registers CharacterAnimationComponent in the Flecs World.
void registerCharacterAnimationComponents(world::World& world);

/// Registers continuous locomotion animation resolution and state-machine systems.
void registerCharacterAnimationSystems(world::World& world);

/// Preloads and binds SkeletonComponent, AnimationComponent, and SkinningPaletteComponent
/// for all entities with CharacterAnimationComponent in the given World.
void resolveCharacterAnimationAssets(world::World& world);

} // namespace eng::framework
