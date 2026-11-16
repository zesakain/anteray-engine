/// @file character_components.cpp
/// @brief Registration and reflection metadata for generic character ECS components.

#include "framework/character_components.h"

#include <array>
#include <cstddef>

namespace eng::framework {

std::span<const CharacterFieldDescriptor> getCharacterConfigReflectionFields() noexcept {
    static constexpr std::array<CharacterFieldDescriptor, 7> kFields{{
        {"capsuleRadiusMeters", "float", offsetof(CharacterConfigComponent, capsuleRadiusMeters)},
        {"capsuleHalfHeightMeters", "float",
         offsetof(CharacterConfigComponent, capsuleHalfHeightMeters)},
        {"eyeHeightMeters", "float", offsetof(CharacterConfigComponent, eyeHeightMeters)},
        {"maxMoveSpeedMetersPerSec", "float",
         offsetof(CharacterConfigComponent, maxMoveSpeedMetersPerSec)},
        {"maxSlopeDegrees", "float", offsetof(CharacterConfigComponent, maxSlopeDegrees)},
        {"stepOffsetMeters", "float", offsetof(CharacterConfigComponent, stepOffsetMeters)},
        {"massKilograms", "float", offsetof(CharacterConfigComponent, massKilograms)},
    }};
    return kFields;
}

std::span<const CharacterFieldDescriptor> getCharacterStateReflectionFields() noexcept {
    static constexpr std::array<CharacterFieldDescriptor, 3> kFields{{
        {"velocityMetersPerSec", "Vec3", offsetof(CharacterStateComponent, velocityMetersPerSec)},
        {"facingYawRadians", "float", offsetof(CharacterStateComponent, facingYawRadians)},
        {"isGrounded", "bool", offsetof(CharacterStateComponent, isGrounded)},
    }};
    return kFields;
}

void registerCharacterComponents(world::World& world) {
    auto& ecs = world.getRawWorld();

    ecs.component<CharacterTag>("CharacterTag");
    ecs.component<CharacterConfigComponent>("CharacterConfigComponent");
    ecs.component<CharacterStateComponent>("CharacterStateComponent");
}

} // namespace eng::framework
