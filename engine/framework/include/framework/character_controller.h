/// @file character_controller.h
/// @brief Physics-based third-person character controller systems for the Shared World.
///
/// Purpose:
/// Drives authoritative character entities tagged with CharacterTag using PhysicsWorld capsule
/// sweeps, slope/step validation, gravity, jumping, dynamic body pushing, and render
/// interpolation.
///
/// Contract:
/// - Register under SystemGroup::Continuous via registerCharacterControllerSystems().
/// - Consumes CharacterMovementIntentComponent without reading OS/SDL input directly.
/// - Never mutates PhysicsWorld from standby Mode::prepare().
///
/// Ownership and Lifetime:
/// Systems are owned by the Flecs World. Non-owning reference to PhysicsWorld must remain valid
/// for the lifetime of the registered systems.
///
/// Thread-safety:
/// Main simulation thread only.
///
/// Cost:
/// Hot-path safe: zero heap allocations, zero std::vector growth, zero locks.

#pragma once

#include "physics/physics_world.h"
#include "world/world.h"

namespace eng::framework {

/// @brief Registers fixed-step character controller and render-interpolation systems.
///
/// Scheduling:
/// - CharacterControllerStep: flecs::OnUpdate (SystemGroup::Continuous) before PhysicsPreSync.
/// - CharacterRenderInterpolate: PhysicsPostSyncPhase (SystemGroup::Continuous) before PreStore.
void registerCharacterControllerSystems(world::World& world, physics::PhysicsWorld& physicsWorld);

} // namespace eng::framework
