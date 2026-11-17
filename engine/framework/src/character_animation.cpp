/// @file character_animation.cpp
/// @brief Locomotion animation state machine and asset cache implementation.

#include "framework/character_animation.h"

#include "anim/animation_clip.h"
#include "anim/animation_components.h"
#include "anim/skeleton.h"
#include "core/log.h"
#include "framework/character_components.h"
#include "render/gltf_importer.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace eng::framework {
namespace {

struct LoadedAnimationBundle {
    anim::Skeleton skeleton;
    std::vector<anim::AnimationClip> clips;
    const anim::AnimationClip* idle{nullptr};
    const anim::AnimationClip* walk{nullptr};
    const anim::AnimationClip* run{nullptr};
    const anim::AnimationClip* jump{nullptr};
    const anim::AnimationClip* inAir{nullptr};
    const anim::AnimationClip* land{nullptr};
    bool valid{false};
};

struct CharacterAnimationAssetCache {
    std::unordered_map<std::string, LoadedAnimationBundle> bundles;
};

[[nodiscard]] const anim::AnimationClip*
findClipByName(const std::vector<anim::AnimationClip>& clips, std::string_view name) noexcept {
    if (name.empty()) {
        return nullptr;
    }
    for (const auto& c : clips) {
        if (c.getName() == name) {
            return &c;
        }
    }
    return nullptr;
}

[[nodiscard]] const LoadedAnimationBundle* getOrLoadBundle(CharacterAnimationAssetCache& cache,
                                                           const CharacterAnimationComponent& cfg) {
    if (cfg.skeletonMeshPath.empty()) {
        return nullptr;
    }
    const std::string key = cfg.skeletonMeshPath + "|" + cfg.animationLibraryPath;
    auto it = cache.bundles.find(key);
    if (it != cache.bundles.end()) {
        return it->second.valid ? &it->second : nullptr;
    }

    auto [insertedIt, _] = cache.bundles.emplace(key, LoadedAnimationBundle{});
    LoadedAnimationBundle& bundle = insertedIt->second;

    auto meshRes = render::GltfImporter::importFromFile(cfg.skeletonMeshPath);
    if (!meshRes.hasValue() || !meshRes.value().skeleton.has_value()) {
        ENG_LOG_WARN("Animation", "Failed to load character skeleton from glTF asset");
        return nullptr;
    }

    bundle.skeleton = std::move(*meshRes.value().skeleton);

    if (!cfg.animationLibraryPath.empty()) {
        auto animRes = render::GltfImporter::importAnimationsFromFile(cfg.animationLibraryPath,
                                                                      bundle.skeleton);
        if (animRes.hasValue()) {
            bundle.clips = std::move(animRes.value());
        } else {
            ENG_LOG_WARN("Animation", "Failed to load animation library from glTF/GLB");
        }
    } else if (!meshRes.value().animations.empty()) {
        bundle.clips = std::move(meshRes.value().animations);
    }

    bundle.idle = findClipByName(bundle.clips, cfg.idleClip);
    bundle.walk = findClipByName(bundle.clips, cfg.walkClip);
    bundle.run = findClipByName(bundle.clips, cfg.runClip);
    bundle.jump = findClipByName(bundle.clips, cfg.jumpClip);
    bundle.inAir = findClipByName(bundle.clips, cfg.inAirClip);
    bundle.land = findClipByName(bundle.clips, cfg.landClip);
    bundle.valid = true;

    return &bundle;
}

[[nodiscard]] const anim::AnimationClip* selectClipForState(const LoadedAnimationBundle& bundle,
                                                            LocomotionAnimState state) noexcept {
    switch (state) {
    case LocomotionAnimState::Idle:
        return bundle.idle;
    case LocomotionAnimState::Walk:
        return (bundle.walk != nullptr) ? bundle.walk : bundle.idle;
    case LocomotionAnimState::Run:
        return (bundle.run != nullptr) ? bundle.run
                                       : ((bundle.walk != nullptr) ? bundle.walk : bundle.idle);
    case LocomotionAnimState::Jump:
        return (bundle.jump != nullptr) ? bundle.jump : bundle.inAir;
    case LocomotionAnimState::InAir:
        return (bundle.inAir != nullptr) ? bundle.inAir : bundle.jump;
    case LocomotionAnimState::Landing:
        return (bundle.land != nullptr) ? bundle.land : bundle.idle;
    }
    return bundle.idle;
}

void bindEntityAnimation(flecs::entity e, CharacterAnimationComponent& charAnim,
                         const LoadedAnimationBundle& bundle) {
    if (!e.has<anim::SkeletonComponent>()) {
        e.set<anim::SkeletonComponent>(anim::SkeletonComponent{.skeleton = &bundle.skeleton});
    } else {
        e.get_mut<anim::SkeletonComponent>()->skeleton = &bundle.skeleton;
    }

    if (!e.has<anim::AnimationComponent>()) {
        e.set<anim::AnimationComponent>({});
    }
    if (!e.has<anim::SkinningPaletteComponent>()) {
        e.set<anim::SkinningPaletteComponent>({});
    }

    auto* animComp = e.get_mut<anim::AnimationComponent>();
    if (animComp != nullptr && bundle.idle != nullptr) {
        animComp->controller.play(bundle.idle, true, 1.0F);
    }
    charAnim.currentState = LocomotionAnimState::Idle;
    charAnim.landingTimerSeconds = 0.0F;
    charAnim.wasGrounded = true;
    charAnim.initialized = true;
    charAnim.cachedBundle = &bundle;
}

} // namespace

void registerCharacterAnimationComponents(world::World& world) {
    auto& ecs = world.getRawWorld();
    ecs.component<CharacterAnimationComponent>("CharacterAnimationComponent");
}

void resolveCharacterAnimationAssets(world::World& world) {
    auto& ecs = world.getRawWorld();
    if (!ecs.has<CharacterAnimationAssetCache>()) {
        ecs.set<CharacterAnimationAssetCache>({});
    }
    auto* cache = ecs.get_mut<CharacterAnimationAssetCache>();
    if (cache == nullptr) {
        return;
    }

    ecs.each<CharacterAnimationComponent>(
        [cache](flecs::entity e, CharacterAnimationComponent& ca) {
            if (!ca.initialized) {
                if (const auto* bundle = getOrLoadBundle(*cache, ca)) {
                    bindEntityAnimation(e, ca, *bundle);
                }
            }
        });
}

void registerCharacterAnimationSystems(world::World& world) {
    registerCharacterAnimationComponents(world);
    auto& ecs = world.getRawWorld();
    if (!ecs.has<CharacterAnimationAssetCache>()) {
        ecs.set<CharacterAnimationAssetCache>({});
    }

    auto locoSys =
        ecs.system<CharacterAnimationComponent, const CharacterStateComponent,
                   anim::AnimationComponent>("CharacterLocomotionAnimation")
            .kind(flecs::OnUpdate)
            .each([](flecs::iter& it, size_t index, CharacterAnimationComponent& ca,
                     const CharacterStateComponent& chState, anim::AnimationComponent& animComp) {
                const auto* bundle = static_cast<const LoadedAnimationBundle*>(ca.cachedBundle);
                if (bundle == nullptr) {
                    return;
                }

                const float dt = it.delta_time();
                const float horizSpeed =
                    std::sqrt((chState.velocityMetersPerSec.x * chState.velocityMetersPerSec.x) +
                              (chState.velocityMetersPerSec.z * chState.velocityMetersPerSec.z));

                if (ca.landingTimerSeconds > 0.0F) {
                    ca.landingTimerSeconds = std::max(0.0F, ca.landingTimerSeconds - dt);
                }

                LocomotionAnimState desiredState = ca.currentState;
                if (!chState.isGrounded) {
                    if (ca.wasGrounded && chState.velocityMetersPerSec.y > 0.5F) {
                        desiredState = LocomotionAnimState::Jump;
                    } else if (ca.currentState != LocomotionAnimState::Jump ||
                               chState.velocityMetersPerSec.y < 0.0F) {
                        desiredState = LocomotionAnimState::InAir;
                    }
                } else {
                    if (!ca.wasGrounded) {
                        ca.landingTimerSeconds = ca.landingDurationSeconds;
                        desiredState = LocomotionAnimState::Landing;
                    } else if (ca.landingTimerSeconds > 0.0F &&
                               horizSpeed < ca.walkSpeedThreshold) {
                        desiredState = LocomotionAnimState::Landing;
                    } else if (horizSpeed >= ca.runSpeedThreshold) {
                        desiredState = LocomotionAnimState::Run;
                    } else if (horizSpeed >= ca.walkSpeedThreshold) {
                        desiredState = LocomotionAnimState::Walk;
                    } else {
                        desiredState = LocomotionAnimState::Idle;
                    }
                }

                ca.wasGrounded = chState.isGrounded;

                if (desiredState != ca.currentState) {
                    const anim::AnimationClip* targetClip =
                        selectClipForState(*bundle, desiredState);
                    if (targetClip != nullptr) {
                        const bool loop = (desiredState != LocomotionAnimState::Jump &&
                                           desiredState != LocomotionAnimState::Landing);
                        animComp.controller.crossFade(targetClip, ca.blendDurationSeconds, loop,
                                                      1.0F);
                    }
                    ca.currentState = desiredState;
                }

                (void)index;
            });

    world.registerSystem(world::SystemGroup::Continuous, locoSys);
}

} // namespace eng::framework
