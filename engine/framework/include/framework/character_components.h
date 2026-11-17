/// @file character_components.h
/// @brief Generic character ECS components and reflection metadata for the Shared World.
///
/// Purpose:
/// Defines plain-data character entity tags, configuration parameters, and runtime state
/// components stored in the single shared Flecs World.
///
/// Contract:
/// - Components are small POD structs with no behavior and no owning pointers.
/// - Units are explicit in field names (meters, seconds, degrees, kilograms).
/// - registerCharacterComponents() must be called before creating or serializing character
///   entities.
///
/// Thread-safety:
/// Main thread for component registration and structural ECS mutation.
///
/// Cost:
/// Zero heap allocations; suitable for contiguous Flecs archetype storage.

#pragma once

#include "core/math.h"
#include "world/world.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace eng::framework {

/// @struct CharacterTag
/// @brief Zero-sized ECS tag identifying an authoritative character root entity.
struct CharacterTag {};

/// @struct PlayerControlledTag
/// @brief Zero-sized ECS tag explicitly designating the locally player-controlled character.
///
/// Must be independent of Editor Inspector selection.
struct PlayerControlledTag {};

/// @struct CharacterConfigComponent
/// @brief Generic physical and spatial configuration parameters for a character entity.
struct CharacterConfigComponent {
    float capsuleRadiusMeters{0.35F};
    float capsuleHalfHeightMeters{0.70F};
    float eyeHeightMeters{1.55F};
    float maxMoveSpeedMetersPerSec{6.0F};
    float groundAccelerationMetersPerSecSq{35.0F};
    float groundDecelerationMetersPerSecSq{40.0F};
    float airAccelerationMetersPerSecSq{12.0F};
    float rotationSpeedRadiansPerSec{12.0F};
    float jumpSpeedMetersPerSec{5.5F};
    float gravityMetersPerSecSq{18.0F};
    float groundCheckDistanceMeters{0.08F};
    float pushImpulseNewtonSeconds{120.0F};
    float maxSlopeDegrees{45.0F};
    float stepOffsetMeters{0.35F};
    float massKilograms{70.0F};

    /// Validates physical and spatial parameter ranges.
    [[nodiscard]] constexpr bool isValid() const noexcept {
        return capsuleRadiusMeters > 0.0F && capsuleHalfHeightMeters >= 0.0F &&
               eyeHeightMeters >= 0.0F && maxMoveSpeedMetersPerSec >= 0.0F &&
               groundAccelerationMetersPerSecSq >= 0.0F &&
               groundDecelerationMetersPerSecSq >= 0.0F && airAccelerationMetersPerSecSq >= 0.0F &&
               rotationSpeedRadiansPerSec >= 0.0F && jumpSpeedMetersPerSec >= 0.0F &&
               gravityMetersPerSecSq >= 0.0F && groundCheckDistanceMeters > 0.0F &&
               pushImpulseNewtonSeconds >= 0.0F && maxSlopeDegrees >= 0.0F &&
               maxSlopeDegrees <= 89.0F && stepOffsetMeters >= 0.0F && massKilograms > 0.0F;
    }
};

/// @struct CharacterStateComponent
/// @brief Generic runtime locomotion state for a character entity in the Shared World.
struct CharacterStateComponent {
    Vec3 velocityMetersPerSec{Vec3::zero()};
    Vec3 groundNormal{0.0F, 1.0F, 0.0F};
    Vec3 authoritativePositionMeters{Vec3::zero()};
    Vec3 previousPhysicsPositionMeters{Vec3::zero()};
    float facingYawRadians{0.0F};
    bool isGrounded{false};
    bool wasJumpPressed{false};
    bool physicsPositionInitialized{false};
};

/// @struct CharacterMovementIntentComponent
/// @brief Decoupled per-frame movement intent consumed by the physics character controller.
///
/// Populated by active gameplay state, tests, or input adapters (replaced in Milestone 12C).
struct CharacterMovementIntentComponent {
    /// Horizontal movement axis in camera space: x = right [-1, 1], y = forward [-1, 1].
    Vec2 moveAxis{0.0F, 0.0F};
    bool jumpRequested{false};
    bool scriptJumpRequested{false};
    float cameraYawRadians{0.0F};
    bool hasCameraYaw{false};
};

/// @brief Reflection field metadata shared across serialization, editor, and scripting.
struct CharacterFieldDescriptor {
    std::string_view name;
    std::string_view typeName;
    size_t offsetBytes{0};
};

/// Returns static reflection metadata for CharacterConfigComponent fields.
[[nodiscard]] std::span<const CharacterFieldDescriptor>
getCharacterConfigReflectionFields() noexcept;

/// Returns static reflection metadata for CharacterStateComponent fields.
[[nodiscard]] std::span<const CharacterFieldDescriptor>
getCharacterStateReflectionFields() noexcept;

/// Returns static reflection metadata for CharacterMovementIntentComponent fields.
[[nodiscard]] std::span<const CharacterFieldDescriptor>
getCharacterMovementIntentReflectionFields() noexcept;

/// Registers CharacterTag, CharacterConfigComponent, CharacterStateComponent, and
/// CharacterMovementIntentComponent with Flecs.
void registerCharacterComponents(world::World& world);

} // namespace eng::framework
