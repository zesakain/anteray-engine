/// @file third_person_camera.cpp
/// @brief Third-person camera follow, orbit, and physics raycast collision avoidance.

#include "framework/third_person_camera.h"

#include "framework/character_components.h"
#include "physics/physics_components.h"
#include "world/components.h"

#include <algorithm>
#include <cmath>

namespace eng::framework {
namespace {

[[nodiscard]] flecs::entity resolveTargetCharacter(flecs::world& ecs, uint64_t targetId) noexcept {
    if (targetId != 0) {
        const flecs::entity e = ecs.entity(targetId);
        if (e.is_valid() && e.is_alive() && e.has<world::TransformComponent>()) {
            return e;
        }
    }

    flecs::entity found{};
    ecs.each<const CharacterTag>([&found](flecs::entity e, const CharacterTag& /*tag*/) {
        if (!found.is_valid() && e.has<world::TransformComponent>()) {
            found = e;
        }
    });
    return found;
}

} // namespace

void registerThirdPersonCameraSystems(world::World& world,
                                      const physics::PhysicsWorld& physicsWorld) {
    auto& ecs = world.getRawWorld();
    ecs.component<ThirdPersonCameraComponent>("ThirdPersonCameraComponent");

    const flecs::entity postSyncPhase = ecs.lookup("PhysicsPostSyncPhase");
    const flecs::entity_t phaseId =
        postSyncPhase.is_valid() ? postSyncPhase.id() : flecs::PostUpdate;

    auto camSystem =
        ecs.system<ThirdPersonCameraComponent, world::TransformComponent>("ThirdPersonCameraFollow")
            .kind(phaseId)
            .each([&physicsWorld, &world](flecs::iter& it, size_t /*index*/,
                                          ThirdPersonCameraComponent& cam,
                                          world::TransformComponent& camTransform) {
                flecs::world rawWorld = it.world();
                flecs::entity target = resolveTargetCharacter(rawWorld, cam.targetEntityId);
                if (!target.is_valid()) {
                    return;
                }

                const auto* targetTr = target.get<world::TransformComponent>();
                if (targetTr == nullptr) {
                    return;
                }

                const Vec3 desiredPivot =
                    targetTr->transform.position + Vec3{0.0F, cam.heightOffsetMeters, 0.0F};
                if (!cam.pivotInitialized || cam.followSmoothSpeed <= 0.0F) {
                    cam.currentPivotMeters = desiredPivot;
                    cam.pivotInitialized = true;
                } else {
                    const float alpha =
                        1.0F - std::exp(-cam.followSmoothSpeed * std::max(0.0F, it.delta_time()));
                    cam.currentPivotMeters =
                        Vec3::lerp(cam.currentPivotMeters, desiredPivot, alpha);
                }

                cam.pitchRadians =
                    std::clamp(cam.pitchRadians, cam.minPitchRadians, cam.maxPitchRadians);

                const float sinYaw = std::sin(cam.yawRadians);
                const float cosYaw = std::cos(cam.yawRadians);
                const float sinPitch = std::sin(cam.pitchRadians);
                const float cosPitch = std::cos(cam.pitchRadians);

                // Forward look direction from camera toward pivot
                const Vec3 lookDir{-sinYaw * cosPitch, -sinPitch, -cosYaw * cosPitch};
                const float desiredDist = std::max(cam.minDistanceMeters, cam.distanceMeters);

                physics::BodyHandle ignoreBody{};
                if (const auto* rb = target.get<physics::RigidBodyComponent>()) {
                    ignoreBody = rb->bodyHandle;
                }

                float resolvedDist = desiredDist;
                physics::RaycastQuery ray{};
                ray.origin = cam.currentPivotMeters;
                ray.direction = -lookDir;
                ray.maxDistance = desiredDist;
                ray.layerMask = static_cast<uint16_t>(~(1U << physics::CollisionLayer::kSensor));
                ray.ignoreBody = ignoreBody;

                const physics::RaycastHit hit = physicsWorld.castRay(ray);
                if (hit.hasHit) {
                    const float hitDist =
                        std::max(0.25F, (hit.fraction * desiredDist) - cam.collisionRadiusMeters);
                    resolvedDist = std::min(desiredDist, hitDist);
                }

                camTransform.transform.position = cam.currentPivotMeters - (lookDir * resolvedDist);
                camTransform.transform.rotation =
                    Quat::fromAxisAngle(Vec3::unitY(), cam.yawRadians) *
                    Quat::fromAxisAngle(Vec3::unitX(), -cam.pitchRadians);
                camTransform.isDirty = true;
                world.setTransformDirty(true);

                if (auto* intent = target.get_mut<CharacterMovementIntentComponent>()) {
                    intent->cameraYawRadians = cam.yawRadians;
                    intent->hasCameraYaw = true;
                }
            });

    world.registerSystem(world::SystemGroup::Continuous, camSystem);
}

} // namespace eng::framework
