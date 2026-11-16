/// @file third_person_camera.h
/// @brief Third-person follow camera component and collision-aware orbit system.
///
/// Purpose:
/// Provides a reusable third-person camera rig that follows an authoritative CharacterTag
/// entity without modifying physics-authoritative transforms, and performs PhysicsWorld
/// raycasts to avoid geometry obstruction.
///
/// Contract:
/// - Register via registerThirdPersonCameraSystems() under SystemGroup::Continuous.
/// - Runs after CharacterRenderInterpolate and before PreStore hierarchy propagation.
///
/// Ownership and Lifetime:
/// Component is a POD value type stored in Flecs archetypes.
///
/// Thread-safety:
/// Main simulation thread only.
///
/// Cost:
/// Hot-path safe: zero heap allocations, one narrow PhysicsWorld::castRay per camera.

#pragma once

#include "core/math.h"
#include "physics/physics_world.h"
#include "world/world.h"

#include <cstdint>

namespace eng::framework {

/// @struct ThirdPersonCameraComponent
/// @brief Configuration and orbit state for a third-person follow camera entity.
struct ThirdPersonCameraComponent {
    /// Optional target entity ID; 0 automatically binds to the first CharacterTag entity.
    uint64_t targetEntityId{0};

    float yawRadians{0.0F};
    float pitchRadians{0.25F};
    float minPitchRadians{-0.60F};
    float maxPitchRadians{1.20F};

    float distanceMeters{5.0F};
    float minDistanceMeters{1.0F};
    float heightOffsetMeters{1.55F};
    float collisionRadiusMeters{0.20F};
    float followSmoothSpeed{18.0F};

    Vec3 currentPivotMeters{Vec3::zero()};
    bool pivotInitialized{false};
};

/// @brief Registers ThirdPersonCameraComponent and its continuous follow/collision system.
void registerThirdPersonCameraSystems(world::World& world,
                                      const physics::PhysicsWorld& physicsWorld);

} // namespace eng::framework
