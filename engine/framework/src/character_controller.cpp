/// @file character_controller.cpp
/// @brief Physics-based character controller implementation using Jolt PhysicsWorld sweeps.

#include "framework/character_controller.h"

#include "core/math.h"
#include "framework/character_components.h"
#include "physics/physics_components.h"
#include "world/components.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace eng::framework {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTwoPi = 2.0F * kPi;
constexpr float kSkinWidthMeters = 0.015F;
constexpr float kMinStepDistMeters = 0.0005F;
constexpr int kMaxSlideIterations = 3;

[[nodiscard]] float degreesToRadians(float deg) noexcept {
    return deg * (kPi / 180.0F);
}

[[nodiscard]] float wrapAngleRadians(float angle) noexcept {
    float res = std::fmod(angle + kPi, kTwoPi);
    if (res < 0.0F) {
        res += kTwoPi;
    }
    return res - kPi;
}

[[nodiscard]] Vec3 moveTowardsHoriz(const Vec3& current, const Vec3& target,
                                    float maxDelta) noexcept {
    const float dx = target.x - current.x;
    const float dz = target.z - current.z;
    const float distSq = (dx * dx) + (dz * dz);
    if (distSq <= maxDelta * maxDelta || distSq < 1e-12F) {
        return Vec3{target.x, current.y, target.z};
    }
    const float invDist = 1.0F / std::sqrt(distSq);
    return Vec3{current.x + (dx * invDist * maxDelta), current.y,
                current.z + (dz * invDist * maxDelta)};
}

[[nodiscard]] physics::ColliderDesc
resolveCharacterShape(const CharacterConfigComponent& cfg,
                      const physics::ColliderComponent* collider) noexcept {
    if (collider != nullptr && collider->desc.type == physics::ColliderType::Capsule) {
        return collider->desc;
    }
    return physics::ColliderDesc::makeCapsule(
        cfg.capsuleRadiusMeters, cfg.capsuleHalfHeightMeters,
        Vec3{0.0F, cfg.capsuleRadiusMeters + cfg.capsuleHalfHeightMeters, 0.0F});
}

[[nodiscard]] bool isWalkableNormal(const Vec3& normal, float minWalkableNormalY) noexcept {
    return normal.y >= minWalkableNormalY;
}

struct GroundProbeResult {
    bool grounded{false};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    float snapDownMeters{0.0F};
};

[[nodiscard]] GroundProbeResult probeGround(const physics::PhysicsWorld& physicsWorld,
                                            const physics::ColliderDesc& shape,
                                            const Vec3& position, physics::BodyHandle ignoreBody,
                                            float probeDistance,
                                            float minWalkableNormalY) noexcept {
    GroundProbeResult out{};
    const float castDist = kSkinWidthMeters + probeDistance;
    physics::ShapeCastQuery query{};
    query.shape = shape;
    query.startTransform =
        Transform{position + Vec3{0.0F, kSkinWidthMeters, 0.0F}, Quat::identity(), Vec3::one()};
    query.direction = Vec3{0.0F, -1.0F, 0.0F};
    query.maxDistance = castDist;
    query.layerMask = static_cast<uint16_t>(~(1U << physics::CollisionLayer::kSensor));
    query.ignoreBody = ignoreBody;

    const physics::ShapeCastHit hit = physicsWorld.castShape(query);
    if (!hit.hasHit) {
        return out;
    }

    const Vec3 normal = hit.contactNormal.normalized();
    out.normal = normal;
    if (isWalkableNormal(normal, minWalkableNormalY)) {
        out.grounded = true;
        const float hitDist = hit.fraction * castDist;
        out.snapDownMeters = std::max(0.0F, hitDist - kSkinWidthMeters);
    }
    return out;
}

[[nodiscard]] bool tryStepUp(const physics::PhysicsWorld& physicsWorld,
                             const physics::ColliderDesc& shape, Vec3& position,
                             const Vec3& remainingHoriz, physics::BodyHandle ignoreBody,
                             float stepOffsetMeters, float minWalkableNormalY) noexcept {
    if (stepOffsetMeters <= 0.01F) {
        return false;
    }
    const float horizLen = remainingHoriz.length();
    if (horizLen < kMinStepDistMeters) {
        return false;
    }

    const uint16_t layerMask = static_cast<uint16_t>(~(1U << physics::CollisionLayer::kSensor));

    // 1. Check headroom above current position
    physics::ShapeCastQuery upQuery{};
    upQuery.shape = shape;
    upQuery.startTransform = Transform{position, Quat::identity(), Vec3::one()};
    upQuery.direction = Vec3{0.0F, 1.0F, 0.0F};
    upQuery.maxDistance = stepOffsetMeters + kSkinWidthMeters;
    upQuery.layerMask = layerMask;
    upQuery.ignoreBody = ignoreBody;

    const physics::ShapeCastHit upHit = physicsWorld.castShape(upQuery);
    const float availableUp =
        upHit.hasHit ? std::max(0.0F, (upHit.fraction * upQuery.maxDistance) - kSkinWidthMeters)
                     : stepOffsetMeters;
    if (availableUp < 0.02F) {
        return false;
    }

    const Vec3 raisedPos = position + Vec3{0.0F, availableUp, 0.0F};

    // 2. Sweep horizontally from raised position (probe at least 0.15m so capsule hemisphere
    // clears the 90-degree step edge onto the flat top surface).
    const float probeHorizDist = std::max(horizLen, 0.15F);
    const Vec3 fwdDir = remainingHoriz / horizLen;
    physics::ShapeCastQuery fwdQuery{};
    fwdQuery.shape = shape;
    fwdQuery.startTransform = Transform{raisedPos, Quat::identity(), Vec3::one()};
    fwdQuery.direction = fwdDir;
    fwdQuery.maxDistance = probeHorizDist;
    fwdQuery.layerMask = layerMask;
    fwdQuery.ignoreBody = ignoreBody;

    const physics::ShapeCastHit fwdHit = physicsWorld.castShape(fwdQuery);
    const float movedFrac =
        fwdHit.hasHit ? std::max(0.0F, fwdHit.fraction - (kSkinWidthMeters / probeHorizDist))
                      : 1.0F;
    if (movedFrac <= 0.05F) {
        return false;
    }

    const float actualHorizMove = std::min(horizLen, movedFrac * probeHorizDist);
    const Vec3 fwdPos = raisedPos + (fwdDir * (movedFrac * probeHorizDist));

    // 3. Sweep down to land on step surface
    physics::ShapeCastQuery downQuery{};
    downQuery.shape = shape;
    downQuery.startTransform = Transform{fwdPos, Quat::identity(), Vec3::one()};
    downQuery.direction = Vec3{0.0F, -1.0F, 0.0F};
    downQuery.maxDistance = availableUp + kSkinWidthMeters;
    downQuery.layerMask = layerMask;
    downQuery.ignoreBody = ignoreBody;

    const physics::ShapeCastHit downHit = physicsWorld.castShape(downQuery);
    if (!downHit.hasHit) {
        return false;
    }

    const Vec3 landNormal = downHit.contactNormal.normalized();
    if (!isWalkableNormal(landNormal, minWalkableNormalY)) {
        return false;
    }

    const float downDist =
        std::max(0.0F, (downHit.fraction * downQuery.maxDistance) - kSkinWidthMeters);
    const float steppedY = fwdPos.y - downDist;
    if (steppedY <= position.y + 0.01F) {
        return false;
    }
    position = Vec3{position.x + (fwdDir.x * actualHorizMove), steppedY,
                    position.z + (fwdDir.z * actualHorizMove)};
    return true;
}

void stepCharacterFixed(physics::PhysicsWorld& physicsWorld, const CharacterConfigComponent& cfg,
                        CharacterStateComponent& state,
                        const CharacterMovementIntentComponent& intent,
                        const physics::ColliderDesc& shape, physics::BodyHandle bodyHandle,
                        float dt) noexcept {
    if (!state.physicsPositionInitialized) {
        state.previousPhysicsPositionMeters = state.authoritativePositionMeters;
        state.physicsPositionInitialized = true;
    }
    state.previousPhysicsPositionMeters = state.authoritativePositionMeters;

    Vec3 pos = state.authoritativePositionMeters;
    const float minWalkableNormalY = std::cos(degreesToRadians(cfg.maxSlopeDegrees));

    // 1. Ground detection
    const GroundProbeResult groundBefore = probeGround(
        physicsWorld, shape, pos, bodyHandle, cfg.groundCheckDistanceMeters, minWalkableNormalY);
    if (groundBefore.grounded && state.velocityMetersPerSec.y <= 0.05F) {
        state.isGrounded = true;
        state.groundNormal = groundBefore.normal;
        if (groundBefore.snapDownMeters > 0.0F &&
            groundBefore.snapDownMeters <= cfg.groundCheckDistanceMeters) {
            pos.y -= groundBefore.snapDownMeters;
        }
        if (state.velocityMetersPerSec.y < 0.0F) {
            state.velocityMetersPerSec.y = 0.0F;
        }
    } else {
        state.isGrounded = false;
        state.groundNormal = groundBefore.normal;
    }

    // 2. Camera-relative horizontal movement intent (vertical camera pitch never moves vertically)
    Vec2 axis = intent.moveAxis;
    const float axisLenSq = (axis.x * axis.x) + (axis.y * axis.y);
    if (axisLenSq > 1.0F) {
        const float invLen = 1.0F / std::sqrt(axisLenSq);
        axis.x *= invLen;
        axis.y *= invLen;
    }

    const float refYaw = intent.hasCameraYaw ? intent.cameraYawRadians : state.facingYawRadians;
    const float sinYaw = std::sin(refYaw);
    const float cosYaw = std::cos(refYaw);
    // Right-handed Y-up: yaw=0 faces -Z, right is +X
    const Vec3 camForwardHoriz{-sinYaw, 0.0F, -cosYaw};
    const Vec3 camRightHoriz{cosYaw, 0.0F, -sinYaw};
    const Vec3 wishDir = (camRightHoriz * axis.x) + (camForwardHoriz * axis.y);

    const float wishMag = wishDir.length();
    const Vec3 desiredHorizVel = wishDir * cfg.maxMoveSpeedMetersPerSec;
    const float accel = state.isGrounded
                            ? (wishMag > 0.01F ? cfg.groundAccelerationMetersPerSecSq
                                               : cfg.groundDecelerationMetersPerSecSq)
                            : (wishMag > 0.01F ? cfg.airAccelerationMetersPerSecSq : 0.0F);
    if (accel > 0.0F) {
        state.velocityMetersPerSec =
            moveTowardsHoriz(state.velocityMetersPerSec, desiredHorizVel, accel * dt);
    }

    // 3. Smooth character facing rotation toward movement direction
    if (wishMag > 0.01F) {
        const float targetYaw = std::atan2(-wishDir.x, -wishDir.z);
        const float deltaYaw = wrapAngleRadians(targetYaw - state.facingYawRadians);
        const float maxStep = cfg.rotationSpeedRadiansPerSec * dt;
        state.facingYawRadians = wrapAngleRadians(
            state.facingYawRadians +
            (std::abs(deltaYaw) <= maxStep ? deltaYaw : (deltaYaw > 0.0F ? maxStep : -maxStep)));
    }

    // 4. Jump (edge-triggered when grounded)
    if (intent.jumpRequested && !state.wasJumpPressed && state.isGrounded) {
        state.velocityMetersPerSec.y = cfg.jumpSpeedMetersPerSec;
        state.isGrounded = false;
    }
    state.wasJumpPressed = intent.jumpRequested;

    // 5. Gravity integration
    if (!state.isGrounded) {
        state.velocityMetersPerSec.y -= cfg.gravityMetersPerSecSq * dt;
    }

    // 6. Horizontal collision-aware sweep-and-slide + step-up + dynamic push
    const uint16_t solidMask = static_cast<uint16_t>(~(1U << physics::CollisionLayer::kSensor));
    Vec3 remainingHoriz{state.velocityMetersPerSec.x * dt, 0.0F, state.velocityMetersPerSec.z * dt};

    for (int iter = 0; iter < kMaxSlideIterations; ++iter) {
        const float dist = remainingHoriz.length();
        if (dist < kMinStepDistMeters) {
            break;
        }
        const Vec3 dir = remainingHoriz / dist;

        physics::ShapeCastQuery query{};
        query.shape = shape;
        // Lift slightly (0.05m) so flat floor contact / convex radius underneath does not trigger
        // a t=0 hit during horizontal sweeps.
        query.startTransform =
            Transform{pos + Vec3{0.0F, 0.05F, 0.0F}, Quat::identity(), Vec3::one()};
        query.direction = dir;
        query.maxDistance = dist + kSkinWidthMeters;
        query.layerMask = solidMask;
        query.ignoreBody = bodyHandle;

        const physics::ShapeCastHit hit = physicsWorld.castShape(query);
        if (!hit.hasHit) {
            pos += remainingHoriz;
            break;
        }

        const float hitDist = hit.fraction * query.maxDistance;
        const float safeDist = std::max(0.0F, hitDist - kSkinWidthMeters);
        pos += dir * safeDist;
        Vec3 leftover = remainingHoriz - (dir * safeDist);

        const Vec3 hitNormal = hit.contactNormal.normalized();

        // Push dynamic bodies on contact
        if (hit.body.isValid() && cfg.pushImpulseNewtonSeconds > 0.0F) {
            auto mtRes = physicsWorld.getMotionType(hit.body);
            if (mtRes.hasValue() && mtRes.value() == physics::MotionType::Dynamic) {
                const Vec3 pushDir = Vec3{dir.x, 0.0F, dir.z}.normalized();
                (void)physicsWorld.addImpulse(hit.body,
                                              pushDir * (cfg.pushImpulseNewtonSeconds * dt));
            }
        }

        if (isWalkableNormal(hitNormal, minWalkableNormalY)) {
            // Walkable slope: slide along slope surface
            const float intoPlane = Vec3::dot(leftover, hitNormal);
            if (intoPlane < 0.0F) {
                leftover -= hitNormal * intoPlane;
            }
            remainingHoriz = leftover;
        } else {
            // Steep obstacle or wall: try step-up if grounded, otherwise slide horizontally
            if (state.isGrounded && tryStepUp(physicsWorld, shape, pos, leftover, bodyHandle,
                                              cfg.stepOffsetMeters, minWalkableNormalY)) {
                break;
            }

            Vec3 wallNormal{hitNormal.x, 0.0F, hitNormal.z};
            const float wallLen = wallNormal.length();
            if (wallLen > 1e-4F) {
                wallNormal /= wallLen;
                const float intoWall = Vec3::dot(leftover, wallNormal);
                if (intoWall < 0.0F) {
                    leftover -= wallNormal * intoWall;
                }
                const float velIntoWall = Vec3::dot(
                    Vec3{state.velocityMetersPerSec.x, 0.0F, state.velocityMetersPerSec.z},
                    wallNormal);
                if (velIntoWall < 0.0F) {
                    state.velocityMetersPerSec.x -= wallNormal.x * velIntoWall;
                    state.velocityMetersPerSec.z -= wallNormal.z * velIntoWall;
                }
            } else {
                leftover = Vec3::zero();
            }
            remainingHoriz = leftover;
        }
    }

    // 7. Vertical sweep (gravity / jumping / landing)
    const float dy = state.velocityMetersPerSec.y * dt;
    if (std::abs(dy) >= kMinStepDistMeters) {
        const float vertDist = std::abs(dy);
        const Vec3 vertDir = dy > 0.0F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{0.0F, -1.0F, 0.0F};

        physics::ShapeCastQuery vertQuery{};
        vertQuery.shape = shape;
        vertQuery.startTransform = Transform{pos, Quat::identity(), Vec3::one()};
        vertQuery.direction = vertDir;
        vertQuery.maxDistance = vertDist + kSkinWidthMeters;
        vertQuery.layerMask = solidMask;
        vertQuery.ignoreBody = bodyHandle;

        const physics::ShapeCastHit vertHit = physicsWorld.castShape(vertQuery);
        if (!vertHit.hasHit) {
            pos.y += dy;
        } else {
            const float safeVert =
                std::max(0.0F, (vertHit.fraction * vertQuery.maxDistance) - kSkinWidthMeters);
            pos.y += vertDir.y * safeVert;

            const Vec3 vertNormal = vertHit.contactNormal.normalized();
            if (dy < 0.0F && isWalkableNormal(vertNormal, minWalkableNormalY)) {
                state.isGrounded = true;
                state.groundNormal = vertNormal;
                state.velocityMetersPerSec.y = 0.0F;
            } else if (dy > 0.0F) {
                state.velocityMetersPerSec.y = 0.0F;
            } else if (dy < 0.0F) {
                // Landed on steep unwalkable slope: deflect downward velocity along slope
                const float intoSlope = Vec3::dot(state.velocityMetersPerSec, vertNormal);
                if (intoSlope < 0.0F) {
                    state.velocityMetersPerSec -= vertNormal * intoSlope;
                }
                state.isGrounded = false;
                state.groundNormal = vertNormal;
            }
        }
    }

    state.authoritativePositionMeters = pos;
}

struct ControllerAccumulator {
    float accumulatedSeconds{0.0F};
    float interpolationAlpha{1.0F};
};

} // namespace

void registerCharacterControllerSystems(world::World& world, physics::PhysicsWorld& physicsWorld) {
    auto& ecs = world.getRawWorld();

    // Shared accumulator resource owned by the ECS world (zero heap allocation in hot path).
    ecs.set<ControllerAccumulator>({});

    auto stepSystem =
        ecs.system<const CharacterConfigComponent, CharacterStateComponent,
                   world::TransformComponent>("CharacterControllerStep")
            .kind(flecs::OnUpdate)
            .with<CharacterTag>()
            .run([&physicsWorld, &world](flecs::iter& it) {
                auto* acc = it.world().get_mut<ControllerAccumulator>();
                const auto& settings = physicsWorld.getSettings();
                const float fixedDt = settings.fixedDeltaTime;
                const uint32_t maxSubSteps = settings.maxSubSteps;

                acc->accumulatedSeconds += it.delta_time();
                uint32_t substeps = 0;

                // Count substeps once per frame for all characters
                float tempAcc = acc->accumulatedSeconds;
                while (tempAcc >= fixedDt && substeps < maxSubSteps) {
                    tempAcc -= fixedDt;
                    ++substeps;
                }
                const float maxAcc = fixedDt * static_cast<float>(maxSubSteps);
                if (tempAcc > maxAcc) {
                    tempAcc = 0.0F;
                }
                acc->accumulatedSeconds = tempAcc;
                acc->interpolationAlpha =
                    (fixedDt > 1e-6F) ? std::clamp(tempAcc / fixedDt, 0.0F, 1.0F) : 1.0F;

                while (it.next()) {
                    auto cfgCol = it.field<const CharacterConfigComponent>(0);
                    auto stateCol = it.field<CharacterStateComponent>(1);
                    auto trCol = it.field<world::TransformComponent>(2);

                    for (size_t i = 0; i < it.count(); ++i) {
                        flecs::entity e = it.entity(i);
                        auto& st = stateCol[i];
                        if (!st.physicsPositionInitialized) {
                            st.authoritativePositionMeters = trCol[i].transform.position;
                            st.previousPhysicsPositionMeters = trCol[i].transform.position;
                            st.physicsPositionInitialized = true;
                        }

                        const auto* intentPtr = e.get<CharacterMovementIntentComponent>();
                        const CharacterMovementIntentComponent intent =
                            (intentPtr != nullptr) ? *intentPtr
                                                   : CharacterMovementIntentComponent{};

                        const auto* rb = e.get<physics::RigidBodyComponent>();
                        const physics::BodyHandle bodyHandle =
                            (rb != nullptr) ? rb->bodyHandle : physics::BodyHandle{};
                        const auto* col = e.get<physics::ColliderComponent>();
                        const physics::ColliderDesc shape = resolveCharacterShape(cfgCol[i], col);

                        for (uint32_t s = 0; s < substeps; ++s) {
                            stepCharacterFixed(physicsWorld, cfgCol[i], st, intent, shape,
                                               bodyHandle, fixedDt);
                        }

                        const Quat facingQuat =
                            Quat::fromAxisAngle(Vec3::unitY(), st.facingYawRadians);
                        trCol[i].transform.position = st.authoritativePositionMeters;
                        trCol[i].transform.rotation = facingQuat;
                        trCol[i].isDirty = true;

                        if (auto* pt = e.get_mut<physics::PhysicsTransformComponent>()) {
                            pt->transform.position = st.authoritativePositionMeters;
                            pt->transform.rotation = facingQuat;
                        }
                        world.setTransformDirty(true);
                    }
                }
            });
    world.registerSystem(world::SystemGroup::Continuous, stepSystem);

    // Render interpolation runs in PhysicsPostSyncPhase (after PhysicsStep, before PreStore)
    const flecs::entity postSyncPhase = ecs.lookup("PhysicsPostSyncPhase");
    const flecs::entity_t postSyncPhaseId =
        postSyncPhase.is_valid() ? postSyncPhase.id() : flecs::PostUpdate;

    auto interpSystem =
        ecs.system<const CharacterStateComponent, world::TransformComponent>(
               "CharacterRenderInterpolate")
            .kind(postSyncPhaseId)
            .with<CharacterTag>()
            .each([&world](flecs::iter& it, size_t /*index*/, const CharacterStateComponent& st,
                           world::TransformComponent& tr) {
                if (!st.physicsPositionInitialized) {
                    return;
                }
                const auto* acc = it.world().get<ControllerAccumulator>();
                const float alpha = (acc != nullptr) ? acc->interpolationAlpha : 1.0F;
                tr.transform.position = Vec3::lerp(st.previousPhysicsPositionMeters,
                                                   st.authoritativePositionMeters, alpha);
                tr.isDirty = true;
                world.setTransformDirty(true);
            });
    world.registerSystem(world::SystemGroup::Continuous, interpSystem);
}

} // namespace eng::framework
