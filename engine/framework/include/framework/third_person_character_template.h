/// @file third_person_character_template.h
/// @brief Reusable Third-Person Character Template instantiation helper.
///
/// Purpose:
/// Provides a clean, game-agnostic entry point for instantiating Hikari's data-driven
/// Third-Person Character Template (`.hikari_prefab`) into any shared simulation World,
/// with modular appearance overrides and transactional rollback on invalid configuration
/// or missing assets.
///
/// Ownership and Lifetime:
/// Stateless utility; spawned entities are owned by the target World.
///
/// Thread-safety:
/// Main simulation thread only (mutates World).
///
/// Cost:
/// Cold/setup path (file I/O and ECS entity creation); never call in the per-frame hot path.

#pragma once

#include "core/result.h"
#include "framework/prefab.h"
#include "world/entity.h"
#include "world/world.h"

#include <filesystem>
#include <string_view>

namespace eng::framework {

/// Default repository-relative path to the reusable Third-Person Character prefab.
inline constexpr std::string_view kDefaultThirdPersonPrefabPath =
    "assets/prefabs/third_person_character.hikari_prefab";

/// @struct ThirdPersonTemplateResult
/// @brief Handles to the primary entities created when instantiating the Third-Person template.
struct ThirdPersonTemplateResult {
    world::EntityHandle characterRoot{};
    world::EntityHandle visualMesh{};
    world::EntityHandle cameraEntity{};
};

/// @class ThirdPersonCharacterTemplate
/// @brief Loads, validates, and instantiates the reusable Third-Person Character Template.
class ThirdPersonCharacterTemplate {
public:
    /// Instantiates the Third-Person Character Template into `world`.
    ///
    /// Contract:
    /// - Resolves `prefabPath` (relative to working directory or up to 4 parent directories).
    /// - Validates all components and appearance overrides before mutating `world`.
    /// - On failure, zero partial entities remain in `world`.
    [[nodiscard]] static Result<ThirdPersonTemplateResult>
    instantiate(world::World& world, const PrefabInstantiateOptions& options = {},
                const std::filesystem::path& prefabPath = kDefaultThirdPersonPrefabPath);
};

} // namespace eng::framework
