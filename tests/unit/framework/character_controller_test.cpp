/// @file character_controller_test.cpp
/// @brief Milestone 12B headless verification tests for CharacterController, ThirdPersonCamera,
///        Conductor state authority, and CharacterController_TPP_Test scene/prefab integration.

#include "core/log.h"
#include "core/math.h"
#include "framework/character_components.h"
#include "framework/character_controller.h"
#include "framework/conductor.h"
#include "framework/mode.h"
#include "framework/prefab.h"
#include "framework/runtime_session.h"
#include "framework/scene_serializer.h"
#include "framework/third_person_camera.h"
#include "physics/physics_components.h"
#include "physics/physics_system.h"
#include "physics/physics_world.h"
#include "render/camera_component.h"
#include "render/gltf_importer.h"
#include "render/mesh_component.h"
#include "world/components.h"
#include "world/hierarchy_systems.h"
#include "world/world.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <numbers>

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL: %s at %s:%d\n", #cond, __FILE__, __LINE__);                \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

using namespace eng;
using namespace eng::framework;

namespace {

constexpr float kFixedDt = 1.0F / 60.0F;
constexpr float kPi = std::numbers::pi_v<float>;

[[nodiscard]] std::filesystem::path resolveRepoPath(const char* relPath) {
    std::filesystem::path p(relPath);
    for (int depth = 0; depth < 4; ++depth) {
        if (std::filesystem::exists(p)) {
            return p;
        }
        p = std::filesystem::path("..") / p;
    }
    return std::filesystem::path(relPath);
}

struct TestRig {
    world::World world;
    physics::PhysicsWorld physicsWorld;

    TestRig() : physicsWorld(physics::PhysicsWorldSettings{}) {
        world::registerHierarchySystems(world);
        physics::registerPhysicsComponents(world);
        physics::registerPhysicsSystems(world, physicsWorld);
        registerCharacterComponents(world);
        registerCharacterControllerSystems(world, physicsWorld);
        registerThirdPersonCameraSystems(world, physicsWorld);
    }

    world::EntityHandle addStaticBox(const char* name, const Vec3& pos, const Vec3& halfExtents,
                                     const Quat& rot = Quat::identity()) {
        auto h = world.createEntity(name);
        world.setComponent(h, world::TransformComponent(Transform{pos, rot, Vec3::one()}));
        physics::BodyDesc bd{};
        bd.motionType = physics::MotionType::Static;
        bd.collisionLayer = physics::CollisionLayer::kNonMoving;
        bd.collider = physics::ColliderDesc::makeBox(halfExtents);
        bd.position = pos;
        bd.rotation = rot;
        auto bodyRes = physicsWorld.createBody(bd);
        TEST_CHECK(bodyRes.hasValue());
        world.setComponent(h,
                           physics::RigidBodyComponent{bodyRes.value(), physics::MotionType::Static,
                                                       physics::CollisionLayer::kNonMoving});
        world.setComponent(h, physics::ColliderComponent{bd.collider});
        world.setComponent(h, physics::PhysicsTransformComponent{Transform{pos, rot, Vec3::one()}});
        return h;
    }

    world::EntityHandle addDynamicBox(const char* name, const Vec3& pos, const Vec3& halfExtents,
                                      float massKg) {
        auto h = world.createEntity(name);
        world.setComponent(
            h, world::TransformComponent(Transform{pos, Quat::identity(), Vec3::one()}));
        physics::BodyDesc bd{};
        bd.motionType = physics::MotionType::Dynamic;
        bd.collisionLayer = physics::CollisionLayer::kMoving;
        bd.collider = physics::ColliderDesc::makeBox(halfExtents);
        bd.position = pos;
        bd.mass = massKg;
        auto bodyRes = physicsWorld.createBody(bd);
        TEST_CHECK(bodyRes.hasValue());
        world.setComponent(h, physics::RigidBodyComponent{bodyRes.value(),
                                                          physics::MotionType::Dynamic,
                                                          physics::CollisionLayer::kMoving});
        world.setComponent(h, physics::ColliderComponent{bd.collider});
        world.setComponent(
            h, physics::PhysicsTransformComponent{Transform{pos, Quat::identity(), Vec3::one()}});
        return h;
    }

    world::EntityHandle addCharacter(const Vec3& spawnPos,
                                     const CharacterConfigComponent& cfg = {}) {
        auto h = world.createEntity("TestCharacter");
        world.addTag<CharacterTag>(h);
        world.setComponent(h, cfg);
        world.setComponent(h, CharacterStateComponent{});
        world.setComponent(h, CharacterMovementIntentComponent{});
        world.setComponent(
            h, world::TransformComponent(Transform{spawnPos, Quat::identity(), Vec3::one()}));

        const physics::ColliderDesc col = physics::ColliderDesc::makeCapsule(
            cfg.capsuleRadiusMeters, cfg.capsuleHalfHeightMeters,
            Vec3{0.0F, cfg.capsuleRadiusMeters + cfg.capsuleHalfHeightMeters, 0.0F});
        physics::BodyDesc bd{};
        bd.motionType = physics::MotionType::Kinematic;
        bd.collisionLayer = physics::CollisionLayer::kPlayer;
        bd.collider = col;
        bd.position = spawnPos;
        auto bodyRes = physicsWorld.createBody(bd);
        TEST_CHECK(bodyRes.hasValue());

        world.setComponent(h, physics::RigidBodyComponent{bodyRes.value(),
                                                          physics::MotionType::Kinematic,
                                                          physics::CollisionLayer::kPlayer});
        world.setComponent(h, physics::ColliderComponent{col});
        world.setComponent(h, physics::PhysicsTransformComponent{
                                  Transform{spawnPos, Quat::identity(), Vec3::one()}});
        return h;
    }

    void stepFrames(int count, float dt = kFixedDt) {
        for (int i = 0; i < count; ++i) {
            world.progress(dt);
        }
    }
};

void testGroundAccelerationDecelerationAndRotation() {
    TestRig rig;
    rig.addStaticBox("Ground", Vec3{0.0F, -0.5F, 0.0F}, Vec3{30.0F, 0.5F, 30.0F});
    auto ch = rig.addCharacter(Vec3{0.0F, 0.02F, 0.0F});

    rig.stepFrames(5);
    const auto* st0 = rig.world.getComponent<CharacterStateComponent>(ch);
    TEST_CHECK(st0 != nullptr && st0->isGrounded);

    // Move right (+X) with camera yaw = 0
    rig.world.setComponent(ch, CharacterMovementIntentComponent{
                                   .moveAxis = Vec2{1.0F, 0.0F},
                                   .jumpRequested = false,
                                   .cameraYawRadians = 0.0F,
                                   .hasCameraYaw = true,
                               });
    rig.stepFrames(30);

    const auto* st1 = rig.world.getComponent<CharacterStateComponent>(ch);
    TEST_CHECK(st1->isGrounded);
    TEST_CHECK(st1->authoritativePositionMeters.x > 1.5F);
    TEST_CHECK(std::abs(st1->authoritativePositionMeters.z) < 0.05F);
    TEST_CHECK(st1->velocityMetersPerSec.x > 4.0F);
    // Facing yaw should have rotated toward +X (-pi/2 in right-handed Y-up)
    TEST_CHECK(std::abs(st1->facingYawRadians - (-kPi * 0.5F)) < 0.15F);

    // Release input -> decelerate to stop
    rig.world.setComponent(ch, CharacterMovementIntentComponent{
                                   .moveAxis = Vec2{0.0F, 0.0F},
                                   .jumpRequested = false,
                                   .cameraYawRadians = 0.0F,
                                   .hasCameraYaw = true,
                               });
    rig.stepFrames(30);
    const auto* st2 = rig.world.getComponent<CharacterStateComponent>(ch);
    TEST_CHECK(std::abs(st2->velocityMetersPerSec.x) < 0.01F);
    TEST_CHECK(std::abs(st2->velocityMetersPerSec.z) < 0.01F);

    std::printf("[PASS] testGroundAccelerationDecelerationAndRotation\n");
}

void testCameraRelativeMovementAndPitchIndependence() {
    TestRig rig;
    rig.addStaticBox("Ground", Vec3{0.0F, -0.5F, 0.0F}, Vec3{30.0F, 0.5F, 30.0F});
    auto ch = rig.addCharacter(Vec3{0.0F, 0.02F, 0.0F});

    auto camH = rig.world.createEntity("Camera");
    rig.world.setComponent(camH, world::TransformComponent(Transform::identity()));
    rig.world.setComponent(camH, render::CameraComponent{});
    rig.world.setComponent(camH, ThirdPersonCameraComponent{
                                     .yawRadians = kPi * 0.5F, // Looking along -X
                                     .pitchRadians = 0.9F,     // Steep downward pitch
                                     .distanceMeters = 5.0F,
                                 });

    // Let camera initialize yaw onto character intent, then press forward
    rig.stepFrames(1);
    if (auto* intent =
            rig.world.getRawWorld().entity(ch.id).get_mut<CharacterMovementIntentComponent>()) {
        intent->moveAxis = Vec2{0.0F, 1.0F};
        intent->jumpRequested = false;
    }
    rig.stepFrames(25);

    const auto* st = rig.world.getComponent<CharacterStateComponent>(ch);
    TEST_CHECK(st->authoritativePositionMeters.x < -1.0F);
    TEST_CHECK(std::abs(st->authoritativePositionMeters.z) < 0.05F);
    TEST_CHECK(std::abs(st->authoritativePositionMeters.y) < 0.05F);

    // Camera should follow character without mutating character's PhysicsTransformComponent
    const auto* camTr = rig.world.getComponent<world::TransformComponent>(camH);
    TEST_CHECK(camTr != nullptr);
    TEST_CHECK(camTr->transform.position.x > st->authoritativePositionMeters.x);

    std::printf("[PASS] testCameraRelativeMovementAndPitchIndependence\n");
}

void testJumpGravityAndEdgeFalling() {
    TestRig rig;
    // Small platform ending at z = -2.0
    rig.addStaticBox("Platform", Vec3{0.0F, -0.5F, 0.0F}, Vec3{4.0F, 0.5F, 2.0F});
    auto ch = rig.addCharacter(Vec3{0.0F, 0.02F, 0.0F});

    rig.stepFrames(5);
    TEST_CHECK(rig.world.getComponent<CharacterStateComponent>(ch)->isGrounded);

    // Jump
    rig.world.setComponent(ch, CharacterMovementIntentComponent{
                                   .moveAxis = Vec2{0.0F, 0.0F},
                                   .jumpRequested = true,
                                   .cameraYawRadians = 0.0F,
                                   .hasCameraYaw = true,
                               });
    rig.stepFrames(10);
    const auto* midJump = rig.world.getComponent<CharacterStateComponent>(ch);
    TEST_CHECK(!midJump->isGrounded);
    TEST_CHECK(midJump->authoritativePositionMeters.y > 0.3F);

    // Release jump and land back on platform
    rig.world.setComponent(ch, CharacterMovementIntentComponent{});
    rig.stepFrames(60);
    const auto* landed = rig.world.getComponent<CharacterStateComponent>(ch);
    TEST_CHECK(landed->isGrounded);
    TEST_CHECK(std::abs(landed->authoritativePositionMeters.y) < 0.05F);

    // Walk forward off the edge (z < -2.0)
    rig.world.setComponent(ch, CharacterMovementIntentComponent{
                                   .moveAxis = Vec2{0.0F, 1.0F},
                                   .cameraYawRadians = 0.0F,
                                   .hasCameraYaw = true,
                               });
    rig.stepFrames(60);
    const auto* fallen = rig.world.getComponent<CharacterStateComponent>(ch);
    TEST_CHECK(!fallen->isGrounded);
    TEST_CHECK(fallen->authoritativePositionMeters.y < -0.5F);

    std::printf("[PASS] testJumpGravityAndEdgeFalling\n");
}

void testSlopeLimitsAndStepUp() {
    // 1. Walkable 30 deg ramp vs steep 60 deg ramp
    {
        TestRig rig;
        rig.addStaticBox("Ground", Vec3{0.0F, -0.5F, 0.0F}, Vec3{20.0F, 0.5F, 20.0F});
        const Quat rot30 = Quat::fromAxisAngle(Vec3::unitX(), 30.0F * (kPi / 180.0F));
        rig.addStaticBox("Ramp30", Vec3{0.0F, 0.9F, -4.0F}, Vec3{2.0F, 0.2F, 3.0F}, rot30);

        auto ch = rig.addCharacter(Vec3{0.0F, 0.02F, 0.0F});
        rig.stepFrames(5);

        rig.world.setComponent(ch, CharacterMovementIntentComponent{
                                       .moveAxis = Vec2{0.0F, 1.0F},
                                       .cameraYawRadians = 0.0F,
                                       .hasCameraYaw = true,
                                   });
        rig.stepFrames(60);
        const auto* st = rig.world.getComponent<CharacterStateComponent>(ch);
        TEST_CHECK(st->authoritativePositionMeters.y > 0.4F);
    }

    {
        TestRig rig;
        rig.addStaticBox("Ground", Vec3{0.0F, -0.5F, 0.0F}, Vec3{20.0F, 0.5F, 20.0F});
        const Quat rot60 = Quat::fromAxisAngle(Vec3::unitX(), 60.0F * (kPi / 180.0F));
        rig.addStaticBox("Ramp60", Vec3{0.0F, 1.6F, -3.0F}, Vec3{2.0F, 0.2F, 2.5F}, rot60);

        auto ch = rig.addCharacter(Vec3{0.0F, 0.02F, -0.5F});
        rig.stepFrames(5);

        rig.world.setComponent(ch, CharacterMovementIntentComponent{
                                       .moveAxis = Vec2{0.0F, 1.0F},
                                       .cameraYawRadians = 0.0F,
                                       .hasCameraYaw = true,
                                   });
        rig.stepFrames(45);
        const auto* st = rig.world.getComponent<CharacterStateComponent>(ch);
        // Should not climb steep 60-degree slope above 0.4m
        TEST_CHECK(st->authoritativePositionMeters.y < 0.4F);
    }

    // 2. Step-up within 0.35m limit vs blocked by 0.55m step
    {
        TestRig rig;
        rig.addStaticBox("Ground", Vec3{0.0F, -0.5F, 0.0F}, Vec3{20.0F, 0.5F, 20.0F});
        rig.addStaticBox("Step25", Vec3{0.0F, 0.125F, -3.0F}, Vec3{2.0F, 0.125F, 1.5F});

        auto ch = rig.addCharacter(Vec3{0.0F, 0.02F, 0.0F});
        rig.stepFrames(5);
        rig.world.setComponent(ch, CharacterMovementIntentComponent{
                                       .moveAxis = Vec2{0.0F, 1.0F},
                                       .cameraYawRadians = 0.0F,
                                       .hasCameraYaw = true,
                                   });
        rig.stepFrames(45);
        const auto* st = rig.world.getComponent<CharacterStateComponent>(ch);
        TEST_CHECK(st->authoritativePositionMeters.y > 0.20F);
        TEST_CHECK(st->authoritativePositionMeters.z < -2.0F);
    }

    {
        TestRig rig;
        rig.addStaticBox("Ground", Vec3{0.0F, -0.5F, 0.0F}, Vec3{20.0F, 0.5F, 20.0F});
        rig.addStaticBox("Tall55", Vec3{0.0F, 0.275F, -3.0F}, Vec3{2.0F, 0.275F, 1.5F});

        auto ch = rig.addCharacter(Vec3{0.0F, 0.02F, 0.0F});
        rig.stepFrames(5);
        rig.world.setComponent(ch, CharacterMovementIntentComponent{
                                       .moveAxis = Vec2{0.0F, 1.0F},
                                       .cameraYawRadians = 0.0F,
                                       .hasCameraYaw = true,
                                   });
        rig.stepFrames(45);
        const auto* st = rig.world.getComponent<CharacterStateComponent>(ch);
        TEST_CHECK(st->authoritativePositionMeters.z > -1.8F);
        TEST_CHECK(st->authoritativePositionMeters.y < 0.1F);
    }

    std::printf("[PASS] testSlopeLimitsAndStepUp\n");
}

void testWallCollisionDynamicPushAndCameraOcclusion() {
    TestRig rig;
    rig.addStaticBox("Ground", Vec3{0.0F, -0.5F, 0.0F}, Vec3{30.0F, 0.5F, 30.0F});
    rig.addStaticBox("SolidWall", Vec3{0.0F, 1.5F, -3.0F}, Vec3{4.0F, 1.5F, 0.5F});
    auto dynBox =
        rig.addDynamicBox("PushBox", Vec3{2.5F, 0.5F, 0.0F}, Vec3{0.5F, 0.5F, 0.5F}, 15.0F);
    auto ch = rig.addCharacter(Vec3{0.0F, 0.02F, 0.0F});

    // Camera placed behind character at +Z, with an occluding wall at z = +2.0
    rig.addStaticBox("CameraOccluder", Vec3{0.0F, 1.5F, 2.0F}, Vec3{3.0F, 1.5F, 0.25F});
    auto camH = rig.world.createEntity("Cam");
    rig.world.setComponent(camH, world::TransformComponent(Transform::identity()));
    rig.world.setComponent(camH, render::CameraComponent{});
    rig.world.setComponent(camH, ThirdPersonCameraComponent{
                                     .yawRadians = 0.0F,
                                     .pitchRadians = 0.0F,
                                     .distanceMeters = 6.0F,
                                     .followSmoothSpeed = 0.0F,
                                 });

    // Walk +X toward dynamic box at x = 2.5 (along z = 0.0)
    const Vec3 boxBefore =
        rig.world.getComponent<world::TransformComponent>(dynBox)->transform.position;
    rig.world.setComponent(ch, CharacterMovementIntentComponent{
                                   .moveAxis = Vec2{1.0F, 0.0F},
                                   .cameraYawRadians = 0.0F,
                                   .hasCameraYaw = true,
                               });
    rig.stepFrames(50);
    const Vec3 boxAfter =
        rig.world.getComponent<world::TransformComponent>(dynBox)->transform.position;
    TEST_CHECK(boxAfter.x > boxBefore.x + 0.1F);

    // Walk toward solid wall (-Z)
    rig.world.setComponent(ch, CharacterMovementIntentComponent{
                                   .moveAxis = Vec2{0.0F, 1.0F},
                                   .cameraYawRadians = 0.0F,
                                   .hasCameraYaw = true,
                               });
    rig.stepFrames(45);
    const auto* stWall = rig.world.getComponent<CharacterStateComponent>(ch);
    // Wall front face is at z = -2.5; capsule radius = 0.35 -> character cannot penetrate past
    // -2.15
    TEST_CHECK(stWall->authoritativePositionMeters.z > -2.30F);

    // Camera raycast should have pulled camera forward in front of occluder (z < 2.0)
    const auto* camTr = rig.world.getComponent<world::TransformComponent>(camH);
    TEST_CHECK(camTr->transform.position.z < 2.0F);

    std::printf("[PASS] testWallCollisionDynamicPushAndCameraOcclusion\n");
}

void testFixedTimestepDeterminism() {
    auto runSim = []() -> CharacterStateComponent {
        TestRig rig;
        rig.addStaticBox("Ground", Vec3{0.0F, -0.5F, 0.0F}, Vec3{30.0F, 0.5F, 30.0F});
        auto ch = rig.addCharacter(Vec3{0.0F, 0.02F, 0.0F});
        rig.world.setComponent(ch, CharacterMovementIntentComponent{
                                       .moveAxis = Vec2{0.6F, 0.8F},
                                       .jumpRequested = true,
                                       .cameraYawRadians = 0.4F,
                                       .hasCameraYaw = true,
                                   });
        rig.stepFrames(40);
        return *rig.world.getComponent<CharacterStateComponent>(ch);
    };

    const CharacterStateComponent a = runSim();
    const CharacterStateComponent b = runSim();
    TEST_CHECK(std::abs(a.authoritativePositionMeters.x - b.authoritativePositionMeters.x) < 1e-6F);
    TEST_CHECK(std::abs(a.authoritativePositionMeters.y - b.authoritativePositionMeters.y) < 1e-6F);
    TEST_CHECK(std::abs(a.authoritativePositionMeters.z - b.authoritativePositionMeters.z) < 1e-6F);
    TEST_CHECK(std::abs(a.velocityMetersPerSec.x - b.velocityMetersPerSec.x) < 1e-6F);

    std::printf("[PASS] testFixedTimestepDeterminism\n");
}

class ControllerStateMode final : public Mode {
public:
    ControllerStateMode(std::string_view name, Vec2 axis, bool failCommit)
        : name_(name), axis_(axis), failCommit_(failCommit) {}

    [[nodiscard]] std::string_view getName() const noexcept override { return name_; }

    [[nodiscard]] Result<void> enter(world::World& /*world*/) override {
        return Result<void>::success();
    }

    void update(world::World& world, Duration /*delta*/) override {
        world.getRawWorld().each([this](flecs::entity /*e*/, const CharacterTag&,
                                        CharacterMovementIntentComponent& intent) {
            intent.moveAxis = axis_;
            intent.cameraYawRadians = 0.0F;
            intent.hasCameraYaw = true;
        });
    }

    [[nodiscard]] Result<void> exit(world::World& /*world*/) override {
        return Result<void>::success();
    }

    [[nodiscard]] Result<PreparationStatus> prepare(const world::ReadOnlyWorld& readWorld,
                                                    StateContext& ctx,
                                                    const Budget& /*budget*/) override {
        // ReadOnlyWorld allows inspection without mutating physics or entities
        const auto entities = readWorld.getAllEntities();
        if (!entities.empty()) {
            ctx.cache.preparedEntities.push_back(entities.front());
            ctx.cache.isReady = true;
        }
        return PreparationStatus::Ready;
    }

    [[nodiscard]] Result<void> commit(world::World& /*world*/) override {
        if (failCommit_) {
            return Error{ErrorCode::CommitValidationFailed, "Simulated commit failure"};
        }
        return Result<void>::success();
    }

    void cancelPrepare() override {}
    [[nodiscard]] ModeState getModeState() const noexcept override { return ModeState::Active; }

private:
    std::string_view name_;
    Vec2 axis_;
    bool failCommit_;
};

void testConductorAuthorityAndSharedWorldInvariants() {
    world::World world;
    physics::PhysicsWorld physicsWorld;
    world::registerHierarchySystems(world);
    physics::registerPhysicsComponents(world);
    physics::registerPhysicsSystems(world, physicsWorld);
    registerCharacterComponents(world);
    registerCharacterControllerSystems(world, physicsWorld);

    // Static floor
    auto ground = world.createEntity("Ground");
    world.setComponent(ground, world::TransformComponent(
                                   Transform{Vec3{0, -0.5F, 0}, Quat::identity(), Vec3::one()}));
    physics::BodyDesc gbd{};
    gbd.motionType = physics::MotionType::Static;
    gbd.collider = physics::ColliderDesc::makeBox(Vec3{20, 0.5F, 20});
    gbd.position = Vec3{0, -0.5F, 0};
    auto gb = physicsWorld.createBody(gbd);
    TEST_CHECK(gb.hasValue());
    world.setComponent(ground,
                       physics::RigidBodyComponent{gb.value(), physics::MotionType::Static, 0});

    // Authoritative character entity
    auto ch = world.createEntity("Hero");
    world.addTag<CharacterTag>(ch);
    world.setComponent(ch, CharacterConfigComponent{});
    world.setComponent(ch, CharacterStateComponent{});
    world.setComponent(ch, CharacterMovementIntentComponent{});
    world.setComponent(
        ch, world::TransformComponent(Transform{Vec3{0, 0.02F, 0}, Quat::identity(), Vec3::one()}));

    auto modeA = std::make_unique<ControllerStateMode>("StateA", Vec2{0.0F, 1.0F}, false);
    auto modeB = std::make_unique<ControllerStateMode>("StateB", Vec2{1.0F, 0.0F}, false);

    Conductor conductor(world, std::move(modeA), std::move(modeB), {});
    TEST_CHECK(conductor.initialize(ActiveSlot::SlotA).hasValue());

    const size_t initialEntities = world.getAllEntities().size();
    for (int i = 0; i < 15; ++i) {
        conductor.update(Duration::fromMilliseconds(16));
    }
    const auto* stA = world.getComponent<CharacterStateComponent>(ch);
    TEST_CHECK(stA->authoritativePositionMeters.z < -0.5F);
    TEST_CHECK(world.getAllEntities().size() == initialEntities);

    // Transition A -> B
    TEST_CHECK(conductor.requestTransition().hasValue());
    conductor.update(Duration::fromMilliseconds(16));
    TEST_CHECK(conductor.executeTransition().hasValue());
    TEST_CHECK(conductor.getActiveMode().getName() == "StateB");
    TEST_CHECK(world.getAllEntities().size() == initialEntities);

    for (int i = 0; i < 15; ++i) {
        conductor.update(Duration::fromMilliseconds(16));
    }
    const auto* stB = world.getComponent<CharacterStateComponent>(ch);
    TEST_CHECK(stB->authoritativePositionMeters.x > 0.5F);

    // Failed transition B -> failing A preserves active StateB
    auto failingA = std::make_unique<ControllerStateMode>("FailingA", Vec2{0.0F, 0.0F}, true);
    Conductor conductor2(world, std::make_unique<ControllerStateMode>("KeepB", Vec2{0, 0}, false),
                         std::move(failingA), {});
    TEST_CHECK(conductor2.initialize(ActiveSlot::SlotA).hasValue());
    TEST_CHECK(conductor2.requestTransition().hasValue());
    conductor2.update(Duration::fromMilliseconds(16));
    TEST_CHECK(!conductor2.executeTransition().hasValue());
    TEST_CHECK(conductor2.getActiveMode().getName() == "KeepB");

    std::printf("[PASS] testConductorAuthorityAndSharedWorldInvariants\n");
}

void testDedicatedScenePrefabAndRuntimeSessionPIE() {
    const auto scenePath = resolveRepoPath("assets/scenes/CharacterController_TPP_Test.hikari");
    const auto prefabPath = resolveRepoPath("assets/prefabs/third_person_character.hikari_prefab");
    const auto charModelPath =
        resolveRepoPath("assets/characters/player/Superhero_Male_FullBody.gltf");
    const auto animLibPath = resolveRepoPath("assets/animations/player/UAL1_Standard.glb");
    const auto charLicensePath = resolveRepoPath("assets/characters/player/LICENSE_CC0.txt");
    const auto animLicensePath = resolveRepoPath("assets/animations/player/LICENSE_CC0.txt");
    TEST_CHECK(std::filesystem::exists(scenePath));
    TEST_CHECK(std::filesystem::exists(prefabPath));
    TEST_CHECK(std::filesystem::exists(charModelPath));
    TEST_CHECK(std::filesystem::exists(animLibPath));
    TEST_CHECK(std::filesystem::exists(charLicensePath));
    TEST_CHECK(std::filesystem::exists(animLicensePath));

    // 0. Import real humanoid glTF character model and verify geometry, submeshes, and materials
    {
        auto importRes = render::GltfImporter::importFromFile(charModelPath.string());
        TEST_CHECK(importRes.hasValue());
        const auto& mesh = importRes.value();
        TEST_CHECK(mesh.vertices.size() > 1000);
        TEST_CHECK(mesh.submeshes.size() >= 3);
        TEST_CHECK(!mesh.materials.empty());
        const float heightMeters = mesh.bounds.maxBound.y - mesh.bounds.minBound.y;
        TEST_CHECK(heightMeters > 1.5F && heightMeters < 2.2F);
    }

    // 1. Prefab load & instantiate
    {
        world::World w;
        world::registerHierarchySystems(w);
        physics::registerPhysicsComponents(w);
        registerCharacterComponents(w);

        auto pfRes = PrefabLoader::loadFromFile(prefabPath);
        TEST_CHECK(pfRes.hasValue());
        auto instRes = PrefabLoader::instantiate(w, pfRes.value(), {});
        TEST_CHECK(instRes.hasValue());
        TEST_CHECK(w.getRawWorld().entity(instRes.value().id).has<CharacterTag>());
        TEST_CHECK(w.getComponent<CharacterConfigComponent>(instRes.value()) != nullptr);
    }

    // 2. Load dedicated test scene into Edit World and run PIE via RuntimeSession
    world::World editWorld;
    world::registerHierarchySystems(editWorld);
    physics::registerPhysicsComponents(editWorld);
    registerCharacterComponents(editWorld);

    auto loadRes = SceneSerializer::deserializeFromFile(editWorld, scenePath);
    TEST_CHECK(loadRes.hasValue());
    const size_t editEntityCount = editWorld.getAllEntities().size();
    TEST_CHECK(editEntityCount >= 20);

    // Round-trip serialization check
    auto roundTripJson = SceneSerializer::serializeToString(editWorld);
    TEST_CHECK(roundTripJson.hasValue());

    RuntimeSession session(RuntimeSessionConfig{.isolateWorld = true});
    session.setEditWorld(&editWorld);
    TEST_CHECK(session.play().hasValue());

    auto* rtWorld = session.getRuntimeWorld();
    TEST_CHECK(rtWorld != nullptr);
    TEST_CHECK(rtWorld->getAllEntities().size() == editEntityCount);

    // Find PlayerCharacter in RuntimeWorld and apply movement + jump intent
    world::EntityHandle rtChar{};
    rtWorld->getRawWorld().each<const CharacterTag>(
        [&rtChar](flecs::entity e, const CharacterTag&) { rtChar = world::EntityHandle{e.id()}; });
    TEST_CHECK(rtChar.isValid());
    TEST_CHECK(rtWorld->getComponent<physics::RigidBodyComponent>(rtChar) != nullptr);
    TEST_CHECK(rtWorld->getComponent<physics::ColliderComponent>(rtChar) != nullptr);

    platform::InputSnapshot snap{};
    snap.addKeyEvent(platform::KeyCode::W, true);
    snap.addKeyEvent(platform::KeyCode::Space, true);
    session.setInputSnapshot(snap);
    for (int i = 0; i < 30; ++i) {
        session.update(Duration::fromMilliseconds(16));
    }

    const auto* stAfter = rtWorld->getComponent<CharacterStateComponent>(rtChar);
    TEST_CHECK(stAfter != nullptr);
    TEST_CHECK(stAfter->authoritativePositionMeters.z < -0.5F);

    // Verify PlayerVisual mesh is parented to PlayerCharacter and follows its transform
    const auto children = rtWorld->getChildren(rtChar);
    TEST_CHECK(children.size() == 1);
    const flecs::entity rtVisual = rtWorld->getRawWorld().entity(children[0].id);
    TEST_CHECK(rtVisual.is_valid());
    const auto* visMesh = rtVisual.get<render::MeshComponent>();
    TEST_CHECK(visMesh != nullptr);
    TEST_CHECK(visMesh->assetPath == "assets/characters/player/Superhero_Male_FullBody.gltf");
    TEST_CHECK(visMesh->submeshIndex == render::kAllSubmeshes);
    const auto* visWorldTr = rtVisual.get<world::WorldTransformComponent>();
    TEST_CHECK(visWorldTr != nullptr);
    TEST_CHECK(visWorldTr->worldTransform.position.z < -0.5F);

    TEST_CHECK(session.stop().hasValue());
    TEST_CHECK(editWorld.getAllEntities().size() == editEntityCount);

    std::printf("[PASS] testDedicatedScenePrefabAndRuntimeSessionPIE\n");
}

} // namespace

int main() {
    TEST_CHECK(physics::PhysicsSystem::initialize().hasValue());

    testGroundAccelerationDecelerationAndRotation();
    testCameraRelativeMovementAndPitchIndependence();
    testJumpGravityAndEdgeFalling();
    testSlopeLimitsAndStepUp();
    testWallCollisionDynamicPushAndCameraOcclusion();
    testFixedTimestepDeterminism();
    testConductorAuthorityAndSharedWorldInvariants();
    testDedicatedScenePrefabAndRuntimeSessionPIE();

    physics::PhysicsSystem::shutdown();

    std::printf("All Milestone 12B character_controller_test cases PASSED!\n");
    return 0;
}
