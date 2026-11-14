/// @file world.cpp
/// @brief Unified simulation World implementation.

#include "world/world.h"

#include "world/hierarchy_systems.h"

#include <algorithm>

namespace eng::world {

World::World() {
    groupActive_[static_cast<size_t>(SystemGroup::Continuous)] = true;
    groupActive_[static_cast<size_t>(SystemGroup::StateA)] = true;
    groupActive_[static_cast<size_t>(SystemGroup::StateB)] = false;
    groupActive_[static_cast<size_t>(SystemGroup::Transition)] = false;

    ecs_.component<TransformComponent>("TransformComponent");
    ecs_.component<WorldTransformComponent>("WorldTransformComponent");

    registerHierarchySystems(*this);
}

World::~World() = default;

World::World(World&& other) noexcept
    : ecs_(std::move(other.ecs_)), activeEntities_(std::move(other.activeEntities_)),
      rootEntities_(std::move(other.rootEntities_)), groupActive_(other.groupActive_),
      groupSystems_(std::move(other.groupSystems_)),
      hierarchyValidators_(std::move(other.hierarchyValidators_)),
      isTransformDirty_(other.isTransformDirty_) {}

World& World::operator=(World&& other) noexcept {
    if (this != &other) {
        ecs_ = std::move(other.ecs_);
        activeEntities_ = std::move(other.activeEntities_);
        rootEntities_ = std::move(other.rootEntities_);
        groupActive_ = other.groupActive_;
        groupSystems_ = std::move(other.groupSystems_);
        hierarchyValidators_ = std::move(other.hierarchyValidators_);
        isTransformDirty_ = other.isTransformDirty_;
    }
    return *this;
}

EntityHandle World::createEntity() {
    const flecs::entity e = ecs_.entity();
    EntityHandle handle(e.id());
    activeEntities_.push_back(handle);
    rootEntities_.push_back(handle);
    isTransformDirty_ = true;
    return handle;
}

EntityHandle World::createEntity(std::string_view name) {
    const flecs::entity e = ecs_.entity();
    EntityHandle handle(e.id());
    activeEntities_.push_back(handle);
    rootEntities_.push_back(handle);
    if (!name.empty()) {
        setEntityName(handle, name);
    }
    isTransformDirty_ = true;
    return handle;
}

void World::destroyEntity(EntityHandle entity) {
    if (!isAlive(entity)) {
        return;
    }
    isTransformDirty_ = true;

    // Collect all descendants recursively before destruction to ensure activeEntities_ consistency
    std::vector<EntityHandle> toClean;
    std::vector<EntityHandle> stack;
    stack.push_back(entity);

    while (!stack.empty()) {
        EntityHandle curr = stack.back();
        stack.pop_back();
        toClean.push_back(curr);

        if (isAlive(curr)) {
            ecs_.entity(curr.id).children(
                [&stack](flecs::entity child) { stack.push_back(EntityHandle(child.id())); });
        }
    }

    ecs_.entity(entity.id).destruct();

    for (EntityHandle h : toClean) {
        auto it = std::find(activeEntities_.begin(), activeEntities_.end(), h);
        if (it != activeEntities_.end()) {
            activeEntities_.erase(it);
        }
        auto rit = std::find(rootEntities_.begin(), rootEntities_.end(), h);
        if (rit != rootEntities_.end()) {
            rootEntities_.erase(rit);
        }
    }
}

void World::clearUserEntities() {
    for (EntityHandle h : activeEntities_) {
        if (isAlive(h)) {
            ecs_.entity(h.id).destruct();
        }
    }
    activeEntities_.clear();
    rootEntities_.clear();
    isTransformDirty_ = true;
}

std::vector<EntityHandle> World::getAllEntities() const {
    std::vector<EntityHandle> alive;
    alive.reserve(activeEntities_.size());
    for (EntityHandle h : activeEntities_) {
        if (isAlive(h)) {
            alive.push_back(h);
        }
    }
    return alive;
}

bool World::isAlive(EntityHandle entity) const {
    if (!entity.isValid()) {
        return false;
    }
    return ecs_.is_alive(entity.id);
}

std::string_view World::getEntityName(EntityHandle entity) const {
    if (!isAlive(entity)) {
        return "";
    }
    const flecs::entity fe = ecs_.entity(entity.id);
    const char* docName = fe.doc_name();
    if (docName != nullptr && docName[0] != '\0') {
        return std::string_view(docName);
    }
    const char* name = fe.name().c_str();
    if (name != nullptr) {
        std::string_view sv(name);
        auto hashPos = sv.rfind('#');
        if (hashPos != std::string_view::npos) {
            return sv.substr(0, hashPos);
        }
        return sv;
    }
    return std::string_view("");
}

void World::setEntityName(EntityHandle entity, std::string_view name) {
    if (!isAlive(entity)) {
        return;
    }
    std::array<char, 256> buf{};
    const size_t copyLen = std::min(name.size(), buf.size() - 1);
    for (size_t i = 0; i < copyLen; ++i) {
        buf[i] = name[i];
    }
    buf[copyLen] = '\0';

    flecs::entity fe = ecs_.entity(entity.id);
    fe.set_doc_name(buf.data());

    auto parent = fe.parent();
    bool exists = false;
    if (parent.is_valid() && parent.id() != 0) {
        auto existing = parent.lookup(buf.data());
        exists = (existing.is_valid() && existing.id() != 0 && existing.id() != fe.id());
    } else {
        auto existing = ecs_.lookup(buf.data());
        exists = (existing.is_valid() && existing.id() != 0 && existing.id() != fe.id());
    }

    if (!exists) {
        fe.set_name(buf.data());
    } else {
        std::array<char, 280> disambigBuf{};
        std::snprintf(disambigBuf.data(), disambigBuf.size(), "%s#%llu", buf.data(),
                      static_cast<unsigned long long>(entity.id));
        fe.set_name(disambigBuf.data());
    }
}

void World::registerSystem(SystemGroup group, flecs::system system) {
    const auto idx = static_cast<size_t>(group);
    ENG_ASSERT(idx < groupSystems_.size(), "Invalid SystemGroup index");
    if (groupActive_[idx]) {
        system.enable();
    } else {
        system.disable();
    }
    groupSystems_[idx].push_back(system);
}

void World::setSystemGroupActive(SystemGroup group, bool active) {
    const auto idx = static_cast<size_t>(group);
    ENG_ASSERT(idx < groupSystems_.size(), "Invalid SystemGroup index");
    groupActive_[idx] = active;
    for (auto& sys : groupSystems_[idx]) {
        if (active) {
            sys.enable();
        } else {
            sys.disable();
        }
    }
}

bool World::isSystemGroupActive(SystemGroup group) const noexcept {
    const auto idx = static_cast<size_t>(group);
    return idx < groupActive_.size() && groupActive_[idx];
}

void World::progress(float deltaSeconds) {
    ecs_.progress(deltaSeconds);
}

bool World::setParent(EntityHandle child, EntityHandle parent, bool preserveWorldTransform) {
    if (!isAlive(child)) {
        return false;
    }
    if (parent.isValid() && !isAlive(parent)) {
        return false;
    }
    if (child == parent) {
        return false;
    }
    if (parent.isValid() && isDescendantOf(parent, child)) {
        return false;
    }

    // Run registered hierarchy validators
    for (const auto& validator : hierarchyValidators_) {
        if (validator && !validator(*this, child, parent)) {
            return false;
        }
    }

    // Calculate new local transform to preserve world placement if requested
    if (preserveWorldTransform) {
        Mat4 childWorldMat = Mat4::identity();
        if (const auto* wt = getComponent<WorldTransformComponent>(child)) {
            childWorldMat = wt->worldMatrix;
        } else if (const auto* tc = getComponent<TransformComponent>(child)) {
            childWorldMat = tc->transform.toMatrix();
        }

        if (parent.isValid()) {
            Mat4 parentWorldMat = Mat4::identity();
            if (const auto* pwt = getComponent<WorldTransformComponent>(parent)) {
                parentWorldMat = pwt->worldMatrix;
            } else if (const auto* ptc = getComponent<TransformComponent>(parent)) {
                parentWorldMat = ptc->transform.toMatrix();
            }

            auto invParent = parentWorldMat.inverse();
            if (!invParent.has_value()) {
                // Reject reparenting if target parent's matrix is non-invertible (e.g. zero scale)
                return false;
            }

            Mat4 newLocalMat = invParent.value() * childWorldMat;
            auto decomp = newLocalMat.decompose();
            if (!decomp.has_value()) {
                return false;
            }

            if (auto* tc = getMutableComponent<TransformComponent>(child)) {
                tc->transform = decomp.value();
                tc->isDirty = true;
            }
        } else {
            // Detaching to root: child local transform becomes its current world transform
            auto decomp = childWorldMat.decompose();
            if (decomp.has_value()) {
                if (auto* tc = getMutableComponent<TransformComponent>(child)) {
                    tc->transform = decomp.value();
                    tc->isDirty = true;
                }
            }
        }
    }

    auto childEntity = ecs_.entity(child.id);
    if (parent.isValid()) {
        auto parentEntity = ecs_.entity(parent.id);
        childEntity.child_of(parentEntity);
        auto rit = std::find(rootEntities_.begin(), rootEntities_.end(), child);
        if (rit != rootEntities_.end()) {
            rootEntities_.erase(rit);
        }
    } else {
        childEntity.remove(flecs::ChildOf, flecs::Wildcard);
        if (std::find(rootEntities_.begin(), rootEntities_.end(), child) == rootEntities_.end()) {
            rootEntities_.push_back(child);
        }
    }

    if (auto* tc = getMutableComponent<TransformComponent>(child)) {
        tc->isDirty = true;
    }
    isTransformDirty_ = true;

    return true;
}

EntityHandle World::getParent(EntityHandle child) const {
    if (!isAlive(child)) {
        return EntityHandle{};
    }
    const auto p = ecs_.entity(child.id).parent();
    return p.is_valid() ? EntityHandle(p.id()) : EntityHandle{};
}

std::vector<EntityHandle> World::getChildren(EntityHandle parent) const {
    std::vector<EntityHandle> result;
    forEachChild(parent, [&result](EntityHandle child) { result.push_back(child); });
    return result;
}

bool World::isDescendantOf(EntityHandle descendant, EntityHandle ancestor) const {
    if (!isAlive(descendant) || !isAlive(ancestor) || descendant == ancestor) {
        return false;
    }
    EntityHandle curr = getParent(descendant);
    while (curr.isValid()) {
        if (curr == ancestor) {
            return true;
        }
        curr = getParent(curr);
    }
    return false;
}

std::vector<EntityHandle> World::getRootEntities() const {
    std::vector<EntityHandle> roots;
    forEachRootEntity([&roots](EntityHandle root) { roots.push_back(root); });
    return roots;
}

void World::addHierarchyValidator(HierarchyValidator validator) {
    if (validator != nullptr) {
        hierarchyValidators_.push_back(validator);
    }
}

} // namespace eng::world
