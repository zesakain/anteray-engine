/// @file character_components.cpp
/// @brief Registration and reflection metadata for generic character ECS components.

#include "framework/character_components.h"

#include <array>
#include <cstddef>

namespace eng::framework {

std::span<const CharacterFieldDescriptor> getCharacterConfigReflectionFields() noexcept {
    static constexpr std::array<CharacterFieldDescriptor, 15> kFields{{
        {"capsuleRadiusMeters", "float", offsetof(CharacterConfigComponent, capsuleRadiusMeters)},
        {"capsuleHalfHeightMeters", "float",
         offsetof(CharacterConfigComponent, capsuleHalfHeightMeters)},
        {"eyeHeightMeters", "float", offsetof(CharacterConfigComponent, eyeHeightMeters)},
        {"maxMoveSpeedMetersPerSec", "float",
         offsetof(CharacterConfigComponent, maxMoveSpeedMetersPerSec)},
        {"groundAccelerationMetersPerSecSq", "float",
         offsetof(CharacterConfigComponent, groundAccelerationMetersPerSecSq)},
        {"groundDecelerationMetersPerSecSq", "float",
         offsetof(CharacterConfigComponent, groundDecelerationMetersPerSecSq)},
        {"airAccelerationMetersPerSecSq", "float",
         offsetof(CharacterConfigComponent, airAccelerationMetersPerSecSq)},
        {"rotationSpeedRadiansPerSec", "float",
         offsetof(CharacterConfigComponent, rotationSpeedRadiansPerSec)},
        {"jumpSpeedMetersPerSec", "float",
         offsetof(CharacterConfigComponent, jumpSpeedMetersPerSec)},
        {"gravityMetersPerSecSq", "float",
         offsetof(CharacterConfigComponent, gravityMetersPerSecSq)},
        {"groundCheckDistanceMeters", "float",
         offsetof(CharacterConfigComponent, groundCheckDistanceMeters)},
        {"pushImpulseNewtonSeconds", "float",
         offsetof(CharacterConfigComponent, pushImpulseNewtonSeconds)},
        {"maxSlopeDegrees", "float", offsetof(CharacterConfigComponent, maxSlopeDegrees)},
        {"stepOffsetMeters", "float", offsetof(CharacterConfigComponent, stepOffsetMeters)},
        {"massKilograms", "float", offsetof(CharacterConfigComponent, massKilograms)},
    }};
    return kFields;
}

std::span<const CharacterFieldDescriptor> getCharacterStateReflectionFields() noexcept {
    static constexpr std::array<CharacterFieldDescriptor, 8> kFields{{
        {"velocityMetersPerSec", "Vec3", offsetof(CharacterStateComponent, velocityMetersPerSec)},
        {"groundNormal", "Vec3", offsetof(CharacterStateComponent, groundNormal)},
        {"authoritativePositionMeters", "Vec3",
         offsetof(CharacterStateComponent, authoritativePositionMeters)},
        {"previousPhysicsPositionMeters", "Vec3",
         offsetof(CharacterStateComponent, previousPhysicsPositionMeters)},
        {"facingYawRadians", "float", offsetof(CharacterStateComponent, facingYawRadians)},
        {"isGrounded", "bool", offsetof(CharacterStateComponent, isGrounded)},
        {"wasJumpPressed", "bool", offsetof(CharacterStateComponent, wasJumpPressed)},
        {"physicsPositionInitialized", "bool",
         offsetof(CharacterStateComponent, physicsPositionInitialized)},
    }};
    return kFields;
}

std::span<const CharacterFieldDescriptor> getCharacterMovementIntentReflectionFields() noexcept {
    static constexpr std::array<CharacterFieldDescriptor, 4> kFields{{
        {"moveAxis", "Vec2", offsetof(CharacterMovementIntentComponent, moveAxis)},
        {"jumpRequested", "bool", offsetof(CharacterMovementIntentComponent, jumpRequested)},
        {"cameraYawRadians", "float", offsetof(CharacterMovementIntentComponent, cameraYawRadians)},
        {"hasCameraYaw", "bool", offsetof(CharacterMovementIntentComponent, hasCameraYaw)},
    }};
    return kFields;
}

void registerCharacterComponents(world::World& world) {
    auto& ecs = world.getRawWorld();

    ecs.component<CharacterTag>("CharacterTag");
    ecs.component<CharacterConfigComponent>("CharacterConfigComponent");
    ecs.component<CharacterStateComponent>("CharacterStateComponent");
    ecs.component<CharacterMovementIntentComponent>("CharacterMovementIntentComponent");
}

} // namespace eng::framework
