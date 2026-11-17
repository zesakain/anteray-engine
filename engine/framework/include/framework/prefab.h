/// @file prefab.h
/// @brief Data-driven JSON prefab descriptor (.hikari_prefab) loading, validation, and
/// instantiation.
///
/// Purpose:
/// Implements ADR-0020's data-driven prefab pipeline using Hikari's native JSON schema.
/// Prefabs instantiate a single authoritative entity hierarchy into the Shared World without
/// duplicating entities across Conductor modes.
///
/// Contract:
/// - Prefab descriptors are validated before any entity is created in World.
/// - Instantiation is transactional: on failure, zero partial entities remain in World.
/// - File parsing belongs in cold/standby preparation paths, never in the hot path.
///
/// Thread-safety:
/// parseFromString() and loadFromFile() are thread-safe on distinct outputs.
/// instantiate() must be called on the main simulation thread owning World.

#pragma once

#include "core/math.h"
#include "core/result.h"
#include "framework/character_components.h"
#include "physics/physics_types.h"
#include "render/camera_component.h"
#include "world/entity.h"
#include "world/world.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eng::framework {

/// @struct PrefabEntityNode
/// @brief Staged entity definition and component defaults parsed from a .hikari_prefab file.
struct PrefabEntityNode {
    uint64_t localId{0};
    uint64_t parentLocalId{0};
    std::string name{};

    bool hasTransform{false};
    Transform transform{};

    bool hasRigidBody{false};
    physics::MotionType motionType{physics::MotionType::Dynamic};
    uint16_t collisionLayer{physics::CollisionLayer::kMoving};

    bool hasCollider{false};
    physics::ColliderDesc collider{};

    bool hasScript{false};
    std::string scriptName{};
    bool scriptEnabled{true};
    float scriptUpdateInterval{0.0F};

    bool hasMesh{false};
    std::string meshAssetPath{};
    uint32_t submeshIndex{0};
    bool meshVisible{true};
    bool meshCastShadows{true};

    bool hasCamera{false};
    render::CameraComponent camera{};

    bool hasCharacterTag{false};
    bool hasPlayerControlledTag{false};
    bool hasCharacterConfig{false};
    CharacterConfigComponent characterConfig{};
    bool hasCharacterState{false};
    CharacterStateComponent characterState{};
};

/// @struct PrefabDescriptor
/// @brief Validated in-memory representation of a .hikari_prefab JSON descriptor.
struct PrefabDescriptor {
    static constexpr uint32_t kSupportedFormatVersion = 1;

    uint32_t formatVersion{kSupportedFormatVersion};
    std::string name{};
    std::vector<PrefabEntityNode> entities{};
};

/// @struct PrefabInstantiateOptions
/// @brief Optional instance-level overrides applied when instantiating a PrefabDescriptor.
struct PrefabInstantiateOptions {
    world::EntityHandle parent{};
    std::optional<std::string> rootNameOverride{};
    std::optional<Transform> rootTransformOverride{};
    std::optional<CharacterConfigComponent> characterConfigOverride{};
    std::optional<std::string> scriptNameOverride{};
};

/// @class PrefabLoader
/// @brief Stateless utility for parsing, validating, and instantiating .hikari_prefab descriptors.
class PrefabLoader {
public:
    /// Parses and validates a .hikari_prefab JSON string without mutating any World.
    [[nodiscard]] static Result<PrefabDescriptor> parseFromString(std::string_view jsonText);

    /// Reads, parses, and validates a .hikari_prefab file from disk without mutating any World.
    [[nodiscard]] static Result<PrefabDescriptor>
    loadFromFile(const std::filesystem::path& filepath);

    /// Transactionally instantiates a validated PrefabDescriptor into the Shared World.
    /// Returns the root EntityHandle on success, or rolls back all created entities on failure.
    [[nodiscard]] static Result<world::EntityHandle>
    instantiate(world::World& world, const PrefabDescriptor& prefab,
                const PrefabInstantiateOptions& options = {});
};

} // namespace eng::framework
