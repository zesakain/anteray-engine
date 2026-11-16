/// @file read_only_world.h
/// @brief Read-only query facade over a shared World for isolated standby preparation.
///
/// Purpose:
/// Provides compile-time non-mutating access to the shared simulation World during standby
/// mode preparation. Exposes entity liveness, component read queries, hierarchy traversal, and
/// version counters while omitting all entity/component/system mutators and raw Flecs access.
///
/// Contract:
/// - Non-owning view; the underlying World must outlive this ReadOnlyWorld instance.
/// - Exposes zero mutation APIs and zero non-const pointers/references.
///
/// Thread-safety:
/// Safe for read-only queries on the main thread while no concurrent structural mutations occur.
///
/// Cost:
/// Single pointer wrapper; zero heap allocations for handle/component/forEach queries.

#pragma once

#include "world/world.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace eng::world {

/// @class ReadOnlyWorld
/// @brief Non-mutating query facade over World.
class ReadOnlyWorld {
public:
    explicit ReadOnlyWorld(const World& world) noexcept : world_(&world) {}

    [[nodiscard]] bool isAlive(EntityHandle entity) const { return world_->isAlive(entity); }

    [[nodiscard]] std::string_view getEntityName(EntityHandle entity) const {
        return world_->getEntityName(entity);
    }

    template <typename T> [[nodiscard]] const T* getComponent(EntityHandle entity) const {
        return world_->getComponent<T>(entity);
    }

    template <typename T> [[nodiscard]] bool hasComponent(EntityHandle entity) const {
        return world_->hasComponent<T>(entity);
    }

    [[nodiscard]] bool isSystemGroupActive(SystemGroup group) const noexcept {
        return world_->isSystemGroupActive(group);
    }

    [[nodiscard]] std::vector<EntityHandle> getAllEntities() const {
        return world_->getAllEntities();
    }

    [[nodiscard]] EntityHandle getParent(EntityHandle child) const {
        return world_->getParent(child);
    }

    [[nodiscard]] std::vector<EntityHandle> getChildren(EntityHandle parent) const {
        return world_->getChildren(parent);
    }

    template <typename Fn> void forEachChild(EntityHandle parent, Fn&& fn) const {
        world_->forEachChild(parent, static_cast<Fn&&>(fn));
    }

    [[nodiscard]] bool hasChildren(EntityHandle parent) const {
        return world_->hasChildren(parent);
    }

    [[nodiscard]] bool isDescendantOf(EntityHandle descendant, EntityHandle ancestor) const {
        return world_->isDescendantOf(descendant, ancestor);
    }

    [[nodiscard]] std::vector<EntityHandle> getRootEntities() const {
        return world_->getRootEntities();
    }

    template <typename Fn> void forEachRootEntity(Fn&& fn) const {
        world_->forEachRootEntity(static_cast<Fn&&>(fn));
    }

    [[nodiscard]] bool isTransformDirty() const noexcept { return world_->isTransformDirty(); }

    [[nodiscard]] uint64_t getTransformVersion() const noexcept {
        return world_->getTransformVersion();
    }

    [[nodiscard]] uint64_t getSceneGeneration() const noexcept {
        return world_->getSceneGeneration();
    }

    [[nodiscard]] uint64_t getInstanceId() const noexcept { return world_->getInstanceId(); }

private:
    const World* world_;
};

} // namespace eng::world
