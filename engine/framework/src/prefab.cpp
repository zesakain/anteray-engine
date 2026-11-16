/// @file prefab.cpp
/// @brief Data-driven .hikari_prefab parsing, validation, and transactional instantiation.

#include "framework/prefab.h"

#include "core/json.h"
#include "physics/physics_components.h"
#include "render/asset_manager.h"
#include "render/mesh_component.h"
#include "script/script_components.h"
#include "world/hierarchy_systems.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace eng::framework {

namespace {

std::optional<Vec3> parseVec3(const core::JsonValue& val) {
    if (!val.isArray() || val.size() != 3) {
        return std::nullopt;
    }
    const float x = val[0].asFloat(0.0F);
    const float y = val[1].asFloat(0.0F);
    const float z = val[2].asFloat(0.0F);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return std::nullopt;
    }
    return Vec3{x, y, z};
}

std::optional<Quat> parseQuat(const core::JsonValue& val) {
    if (!val.isArray() || val.size() != 4) {
        return std::nullopt;
    }
    const float x = val[0].asFloat(0.0F);
    const float y = val[1].asFloat(0.0F);
    const float z = val[2].asFloat(0.0F);
    const float w = val[3].asFloat(1.0F);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w)) {
        return std::nullopt;
    }
    return Quat{x, y, z, w};
}

std::optional<physics::MotionType> parseMotionType(std::string_view s) noexcept {
    if (s == "Static") {
        return physics::MotionType::Static;
    }
    if (s == "Kinematic") {
        return physics::MotionType::Kinematic;
    }
    if (s == "Dynamic") {
        return physics::MotionType::Dynamic;
    }
    return std::nullopt;
}

std::optional<physics::ColliderType> parseColliderType(std::string_view s) noexcept {
    if (s == "Box") {
        return physics::ColliderType::Box;
    }
    if (s == "Sphere") {
        return physics::ColliderType::Sphere;
    }
    if (s == "Capsule") {
        return physics::ColliderType::Capsule;
    }
    return std::nullopt;
}

} // namespace

Result<PrefabDescriptor> PrefabLoader::parseFromString(std::string_view jsonText) {
    auto parseRes = core::JsonValue::parse(jsonText);
    if (!parseRes.hasValue()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = parseRes.error().message.empty() ? "Failed to parse prefab JSON"
                                                                 : parseRes.error().message};
    }

    const core::JsonValue& root = parseRes.value();
    if (!root.isObject()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Prefab root must be a JSON object"};
    }

    const int version = root.getInt("formatVersion", -1);
    if (version != static_cast<int>(PrefabDescriptor::kSupportedFormatVersion)) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Unsupported or missing prefab formatVersion"};
    }

    const auto* entitiesArr = root.getArray("entities");
    if (entitiesArr == nullptr || entitiesArr->size() == 0) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Prefab must contain a non-empty 'entities' array"};
    }

    PrefabDescriptor descriptor{};
    descriptor.formatVersion = static_cast<uint32_t>(version);
    descriptor.name = std::string(root.getString("name", ""));
    descriptor.entities.reserve(entitiesArr->size());

    std::unordered_set<uint64_t> seenIds;
    std::unordered_map<uint64_t, uint64_t> parentMap;

    for (size_t i = 0; i < entitiesArr->size(); ++i) {
        const auto& entVal = (*entitiesArr)[i];
        if (!entVal.isObject()) {
            return Error{.code = ErrorCode::InvalidArgument,
                         .message = "Prefab entity entry is not a JSON object"};
        }

        const uint64_t localId = entVal.getUInt64("id", 0);
        if (localId == 0) {
            return Error{.code = ErrorCode::InvalidArgument,
                         .message = "Prefab entity is missing required non-zero 'id'"};
        }
        if (seenIds.contains(localId)) {
            return Error{.code = ErrorCode::InvalidArgument,
                         .message = "Duplicate prefab entity 'id' " + std::to_string(localId)};
        }
        seenIds.insert(localId);

        const uint64_t parentLocalId = entVal.getUInt64("parent", 0);
        if (parentLocalId == localId) {
            return Error{.code = ErrorCode::InvalidArgument,
                         .message =
                             "Prefab entity cannot be its own parent: " + std::to_string(localId)};
        }
        parentMap[localId] = parentLocalId;

        PrefabEntityNode node{};
        node.localId = localId;
        node.parentLocalId = parentLocalId;
        node.name = std::string(entVal.getString("name", ""));

        const auto* compsObj = entVal.getObject("components");
        if (compsObj != nullptr) {
            for (const auto& [compName, compData] : compsObj->asObject()) {
                if (!compData.isObject()) {
                    return Error{.code = ErrorCode::InvalidArgument,
                                 .message = "Component '" + compName + "' must be a JSON object"};
                }

                if (compName == "Transform") {
                    node.hasTransform = true;
                    if (const auto* p = compData.find("position")) {
                        auto v = parseVec3(*p);
                        if (!v.has_value()) {
                            return Error{.code = ErrorCode::InvalidArgument,
                                         .message = "Invalid Transform.position"};
                        }
                        node.transform.position = *v;
                    }
                    if (const auto* r = compData.find("rotation")) {
                        auto q = parseQuat(*r);
                        if (!q.has_value()) {
                            return Error{.code = ErrorCode::InvalidArgument,
                                         .message = "Invalid Transform.rotation"};
                        }
                        node.transform.rotation = *q;
                    }
                    if (const auto* s = compData.find("scale")) {
                        auto v = parseVec3(*s);
                        if (!v.has_value()) {
                            return Error{.code = ErrorCode::InvalidArgument,
                                         .message = "Invalid Transform.scale"};
                        }
                        node.transform.scale = *v;
                    }
                } else if (compName == "RigidBody") {
                    node.hasRigidBody = true;
                    const auto mt = parseMotionType(compData.getString("motionType", "Dynamic"));
                    if (!mt.has_value()) {
                        return Error{.code = ErrorCode::InvalidArgument,
                                     .message = "Invalid RigidBody.motionType"};
                    }
                    node.motionType = *mt;
                    node.collisionLayer = static_cast<uint16_t>(
                        compData.getInt("collisionLayer", physics::CollisionLayer::kMoving));
                } else if (compName == "Collider") {
                    node.hasCollider = true;
                    const auto ct = parseColliderType(compData.getString("type", "Box"));
                    if (!ct.has_value()) {
                        return Error{.code = ErrorCode::InvalidArgument,
                                     .message = "Invalid Collider.type"};
                    }
                    node.collider.type = *ct;
                    if (const auto* he = compData.find("halfExtents")) {
                        if (auto v = parseVec3(*he)) {
                            node.collider.halfExtents = *v;
                        }
                    }
                    node.collider.radius = compData.getFloat("radius", node.collider.radius);
                    node.collider.halfHeight =
                        compData.getFloat("halfHeight", node.collider.halfHeight);
                    if (const auto* off = compData.find("offset")) {
                        if (auto v = parseVec3(*off)) {
                            node.collider.offset = *v;
                        }
                    }
                    if (const auto* rot = compData.find("rotation")) {
                        if (auto q = parseQuat(*rot)) {
                            node.collider.rotation = *q;
                        }
                    }
                } else if (compName == "Script") {
                    node.hasScript = true;
                    node.scriptName = std::string(compData.getString("scriptName", ""));
                    node.scriptEnabled = compData.getBool("enabled", true);
                    node.scriptUpdateInterval = compData.getFloat("updateInterval", 0.0F);
                } else if (compName == "Mesh") {
                    node.hasMesh = true;
                    node.meshAssetPath = std::string(compData.getString("assetPath", ""));
                    node.submeshIndex = static_cast<uint32_t>(compData.getInt("submeshIndex", 0));
                    node.meshVisible = compData.getBool("visible", true);
                    node.meshCastShadows = compData.getBool("castShadows", true);
                } else if (compName == "Camera") {
                    node.hasCamera = true;
                    node.camera.fovDegrees = compData.getFloat("fovDegrees", 60.0F);
                    node.camera.nearPlane = compData.getFloat("nearPlane", 0.1F);
                    node.camera.farPlane = compData.getFloat("farPlane", 1000.0F);
                    node.camera.isPrimary = compData.getBool("isPrimary", true);
                    if (node.camera.fovDegrees < 1.0F || node.camera.fovDegrees > 179.0F ||
                        node.camera.nearPlane <= 0.0F ||
                        node.camera.farPlane <= node.camera.nearPlane) {
                        return Error{.code = ErrorCode::InvalidArgument,
                                     .message = "Invalid Camera parameters in prefab"};
                    }
                } else if (compName == "CharacterTag") {
                    node.hasCharacterTag = true;
                } else if (compName == "CharacterConfig") {
                    node.hasCharacterConfig = true;
                    auto& cfg = node.characterConfig;
                    cfg.capsuleRadiusMeters =
                        compData.getFloat("capsuleRadiusMeters", cfg.capsuleRadiusMeters);
                    cfg.capsuleHalfHeightMeters =
                        compData.getFloat("capsuleHalfHeightMeters", cfg.capsuleHalfHeightMeters);
                    cfg.eyeHeightMeters = compData.getFloat("eyeHeightMeters", cfg.eyeHeightMeters);
                    cfg.maxMoveSpeedMetersPerSec =
                        compData.getFloat("maxMoveSpeedMetersPerSec", cfg.maxMoveSpeedMetersPerSec);
                    cfg.groundAccelerationMetersPerSecSq = compData.getFloat(
                        "groundAccelerationMetersPerSecSq", cfg.groundAccelerationMetersPerSecSq);
                    cfg.groundDecelerationMetersPerSecSq = compData.getFloat(
                        "groundDecelerationMetersPerSecSq", cfg.groundDecelerationMetersPerSecSq);
                    cfg.airAccelerationMetersPerSecSq = compData.getFloat(
                        "airAccelerationMetersPerSecSq", cfg.airAccelerationMetersPerSecSq);
                    cfg.rotationSpeedRadiansPerSec = compData.getFloat(
                        "rotationSpeedRadiansPerSec", cfg.rotationSpeedRadiansPerSec);
                    cfg.jumpSpeedMetersPerSec =
                        compData.getFloat("jumpSpeedMetersPerSec", cfg.jumpSpeedMetersPerSec);
                    cfg.gravityMetersPerSecSq =
                        compData.getFloat("gravityMetersPerSecSq", cfg.gravityMetersPerSecSq);
                    cfg.groundCheckDistanceMeters = compData.getFloat(
                        "groundCheckDistanceMeters", cfg.groundCheckDistanceMeters);
                    cfg.pushImpulseNewtonSeconds =
                        compData.getFloat("pushImpulseNewtonSeconds", cfg.pushImpulseNewtonSeconds);
                    cfg.maxSlopeDegrees = compData.getFloat("maxSlopeDegrees", cfg.maxSlopeDegrees);
                    cfg.stepOffsetMeters =
                        compData.getFloat("stepOffsetMeters", cfg.stepOffsetMeters);
                    cfg.massKilograms = compData.getFloat("massKilograms", cfg.massKilograms);
                    if (!cfg.isValid()) {
                        return Error{.code = ErrorCode::InvalidArgument,
                                     .message = "Invalid CharacterConfig values in prefab"};
                    }
                } else if (compName == "CharacterState") {
                    node.hasCharacterState = true;
                    auto& st = node.characterState;
                    if (const auto* vel = compData.find("velocityMetersPerSec")) {
                        if (auto v = parseVec3(*vel)) {
                            st.velocityMetersPerSec = *v;
                        }
                    }
                    st.facingYawRadians =
                        compData.getFloat("facingYawRadians", st.facingYawRadians);
                    st.isGrounded = compData.getBool("isGrounded", st.isGrounded);
                } else {
                    return Error{.code = ErrorCode::InvalidArgument,
                                 .message = "Unknown component '" + compName + "' in prefab"};
                }
            }
        }

        descriptor.entities.push_back(std::move(node));
    }

    // Validate parent references and acyclicity
    size_t rootCount = 0;
    for (const auto& node : descriptor.entities) {
        if (node.parentLocalId == 0) {
            ++rootCount;
            continue;
        }
        if (!seenIds.contains(node.parentLocalId)) {
            return Error{.code = ErrorCode::InvalidArgument,
                         .message = "Prefab entity " + std::to_string(node.localId) +
                                    " references missing parent " +
                                    std::to_string(node.parentLocalId)};
        }
        // Walk ancestors to detect cycles
        uint64_t cur = node.parentLocalId;
        for (size_t steps = 0; steps < descriptor.entities.size() && cur != 0; ++steps) {
            if (cur == node.localId) {
                return Error{.code = ErrorCode::InvalidArgument,
                             .message = "Cycle detected in prefab entity hierarchy"};
            }
            cur = parentMap[cur];
        }
        if (cur != 0) {
            return Error{.code = ErrorCode::InvalidArgument,
                         .message = "Cycle detected in prefab entity hierarchy"};
        }
    }

    if (rootCount != 1) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Prefab must have exactly one root entity (parent == 0)"};
    }

    return descriptor;
}

Result<PrefabDescriptor> PrefabLoader::loadFromFile(const std::filesystem::path& filepath) {
    std::ifstream ifs(filepath, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        return Error{.code = ErrorCode::InvalidArgument, .message = "Failed to open prefab file"};
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return parseFromString(oss.str());
}

Result<world::EntityHandle> PrefabLoader::instantiate(world::World& world,
                                                      const PrefabDescriptor& prefab,
                                                      const PrefabInstantiateOptions& options) {
    if (prefab.entities.empty()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Cannot instantiate empty PrefabDescriptor"};
    }
    if (options.characterConfigOverride.has_value() &&
        !options.characterConfigOverride->isValid()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Invalid characterConfigOverride"};
    }
    if (options.parent.isValid() && !world.isAlive(options.parent)) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Prefab instantiation target parent entity is not alive"};
    }

    registerCharacterComponents(world);

    std::vector<world::EntityHandle> created;
    created.reserve(prefab.entities.size());
    std::unordered_map<uint64_t, world::EntityHandle> idToHandle;
    world::EntityHandle rootHandle{};

    auto rollback = [&]() {
        // Destroy in reverse or root-safe order
        for (auto it = created.rbegin(); it != created.rend(); ++it) {
            if (world.isAlive(*it)) {
                world.destroyEntity(*it);
            }
        }
    };

    for (const auto& node : prefab.entities) {
        const bool isRoot = (node.parentLocalId == 0);
        const std::string& entName = (isRoot && options.rootNameOverride.has_value())
                                         ? *options.rootNameOverride
                                         : node.name;
        world::EntityHandle h =
            entName.empty() ? world.createEntity() : world.createEntity(entName);
        created.push_back(h);
        idToHandle[node.localId] = h;
        if (isRoot) {
            rootHandle = h;
        }

        if (isRoot && options.rootTransformOverride.has_value()) {
            world.setComponent(h, world::TransformComponent{*options.rootTransformOverride});
            world.setComponent(
                h, physics::PhysicsTransformComponent{.transform = *options.rootTransformOverride});
        } else if (node.hasTransform) {
            world.setComponent(h, world::TransformComponent{node.transform});
            world.setComponent(h, physics::PhysicsTransformComponent{.transform = node.transform});
        }

        if (node.hasRigidBody) {
            world.setComponent(h, physics::RigidBodyComponent{
                                      .bodyHandle = {},
                                      .motionType = node.motionType,
                                      .collisionLayer = node.collisionLayer,
                                  });
        }

        if (node.hasCollider) {
            world.setComponent(h, physics::ColliderComponent{.desc = node.collider});
        }

        if (node.hasScript || (isRoot && options.scriptNameOverride.has_value())) {
            script::ScriptComponent sc{};
            const std::string& srcName = (isRoot && options.scriptNameOverride.has_value())
                                             ? *options.scriptNameOverride
                                             : node.scriptName;
            const size_t len = std::min(srcName.size(), sizeof(sc.scriptName) - 1);
            for (size_t k = 0; k < len; ++k) {
                sc.scriptName[k] = srcName[k];
            }
            sc.scriptName[len] = '\0';
            sc.enabled = node.scriptEnabled;
            sc.updateInterval = node.scriptUpdateInterval;
            sc.timeSinceLastUpdate = 0.0F;
            world.setComponent(h, sc);
        }

        if (node.hasMesh) {
            render::MeshComponent mc{};
            mc.assetPath = node.meshAssetPath;
            mc.meshAsset = render::makeAssetIdFromPath(mc.assetPath);
            mc.submeshIndex = node.submeshIndex;
            mc.visible = node.meshVisible;
            mc.castShadows = node.meshCastShadows;
            world.setComponent(h, mc);
        }

        if (node.hasCamera) {
            world.setComponent(h, node.camera);
        }

        if (node.hasCharacterTag) {
            world.addTag<CharacterTag>(h);
        }

        if (isRoot && options.characterConfigOverride.has_value()) {
            world.setComponent(h, *options.characterConfigOverride);
        } else if (node.hasCharacterConfig) {
            world.setComponent(h, node.characterConfig);
        }

        if (node.hasCharacterState) {
            world.setComponent(h, node.characterState);
        }
    }

    // Wire hierarchy
    if (options.parent.isValid() && rootHandle.isValid()) {
        if (!world.setParent(rootHandle, options.parent, false)) {
            rollback();
            return Error{.code = ErrorCode::InvalidState,
                         .message = "Failed to parent instantiated prefab root"};
        }
    }
    for (const auto& node : prefab.entities) {
        if (node.parentLocalId != 0) {
            auto parentIt = idToHandle.find(node.parentLocalId);
            auto childIt = idToHandle.find(node.localId);
            if (parentIt == idToHandle.end() || childIt == idToHandle.end() ||
                !world.setParent(childIt->second, parentIt->second, false)) {
                rollback();
                return Error{.code = ErrorCode::InvalidState,
                             .message = "Failed to establish prefab child hierarchy"};
            }
        }
    }

    world::updateHierarchyTransforms(world);
    return rootHandle;
}

} // namespace eng::framework
