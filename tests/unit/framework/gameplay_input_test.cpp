/// @file gameplay_input_test.cpp
/// @brief Milestone 12C headless unit & integration tests for GameplayInputSystem,
///        character controller input routing, and Conductor invariants.

#include "core/math.h"
#include "framework/character_components.h"
#include "framework/character_controller.h"
#include "framework/conductor.h"
#include "framework/gameplay_input.h"
#include "framework/mode.h"
#include "framework/runtime_session.h"
#include "framework/third_person_camera.h"
#include "physics/physics_components.h"
#include "physics/physics_system.h"
#include "physics/physics_world.h"
#include "platform/input_snapshot.h"
#include "render/camera_component.h"
#include "world/components.h"
#include "world/hierarchy_systems.h"
#include "world/world.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

void testActionPressHoldReleaseAndRebinding() {
    GameplayInputSystem input;
    TEST_CHECK(input.registerContext("Gameplay", 0).hasValue());
    TEST_CHECK(input.bindAction("Gameplay", "Jump", InputBinding::fromKey(platform::KeyCode::Space))
                   .hasValue());

    platform::InputSnapshot snap{};

    // Frame 1: Space pressed
    snap.addKeyEvent(platform::KeyCode::Space, true);
    input.update(snap);
    TEST_CHECK(input.isActionPressed("Jump"));
    TEST_CHECK(input.isActionHeld("Jump"));
    TEST_CHECK(!input.isActionReleased("Jump"));
    TEST_CHECK(std::abs(input.getActionValue("Jump") - 1.0F) < 1e-5F);

    // Frame 2: Space held
    input.update(snap);
    TEST_CHECK(!input.isActionPressed("Jump"));
    TEST_CHECK(input.isActionHeld("Jump"));
    TEST_CHECK(!input.isActionReleased("Jump"));

    // Frame 3: Space released
    snap.reset();
    input.update(snap);
    TEST_CHECK(!input.isActionPressed("Jump"));
    TEST_CHECK(!input.isActionHeld("Jump"));
    TEST_CHECK(input.isActionReleased("Jump"));
    TEST_CHECK(std::abs(input.getActionValue("Jump")) < 1e-5F);

    // Frame 4: Idle
    input.update(snap);
    TEST_CHECK(!input.isActionReleased("Jump"));

    // Rebind Jump to K
    TEST_CHECK(input.rebindAction("Gameplay", "Jump", InputBinding::fromKey(platform::KeyCode::K))
                   .hasValue());
    snap.addKeyEvent(platform::KeyCode::Space, true);
    input.update(snap);
    TEST_CHECK(!input.isActionHeld("Jump")); // Old key no longer triggers Jump

    snap.reset();
    snap.addKeyEvent(platform::KeyCode::K, true);
    input.update(snap);
    TEST_CHECK(input.isActionPressed("Jump"));
    TEST_CHECK(input.isActionHeld("Jump"));

    std::printf("[PASS] testActionPressHoldReleaseAndRebinding\n");
}

void testAnalogDeadZoneMouseAndContexts() {
    GameplayInputSystem input;
    TEST_CHECK(input.registerContext("Gameplay", 0).hasValue());
    TEST_CHECK(input.registerContext("Menu", 10).hasValue());
    TEST_CHECK(input.setContextEnabled("Menu", false).hasValue());

    TEST_CHECK(
        input
            .bindAction("Gameplay", "MoveX",
                        InputBinding::fromGamepadAxis(platform::GamepadAxis::LeftX, 0, 0.20F, 1.0F))
            .hasValue());
    TEST_CHECK(input
                   .bindAction("Gameplay", "LookX",
                               InputBinding::fromMouseDeltaX(-0.01F, platform::MouseButton::Right))
                   .hasValue());
    TEST_CHECK(input.bindAction("Gameplay", "Jump", InputBinding::fromKey(platform::KeyCode::Space))
                   .hasValue());
    // Higher-priority Menu context binds Jump to Return (shadows Gameplay when enabled)
    TEST_CHECK(input.bindAction("Menu", "Jump", InputBinding::fromKey(platform::KeyCode::Return))
                   .hasValue());

    platform::InputSnapshot snap{};
    snap.gamepads[0].connected = true;

    // Inside dead zone (0.15 <= 0.20) -> 0.0
    snap.gamepads[0].axes[static_cast<size_t>(platform::GamepadAxis::LeftX)] = 0.15F;
    snap.mouseDeltaX = 10.0F; // Right mouse not held -> gated to 0
    input.update(snap);
    TEST_CHECK(std::abs(input.getActionValue("MoveX")) < 1e-5F);
    TEST_CHECK(std::abs(input.getActionValue("LookX")) < 1e-5F);

    // Outside dead zone (0.60 -> (0.60 - 0.20) / 0.80 = 0.50) + Right mouse held
    snap.gamepads[0].axes[static_cast<size_t>(platform::GamepadAxis::LeftX)] = 0.60F;
    snap.mouseButtons.set(static_cast<size_t>(platform::MouseButton::Right), true);
    snap.mouseDeltaX = 10.0F;
    input.update(snap);
    TEST_CHECK(std::abs(input.getActionValue("MoveX") - 0.50F) < 1e-4F);
    TEST_CHECK(std::abs(input.getActionValue("LookX") - (-0.10F)) < 1e-4F);

    // Context priority shadowing: enable Menu -> Space (in Gameplay) is shadowed by Menu's Return
    TEST_CHECK(input.setContextEnabled("Menu", true).hasValue());
    snap.addKeyEvent(platform::KeyCode::Space, true);
    input.update(snap);
    TEST_CHECK(!input.isActionHeld("Jump"));

    snap.addKeyEvent(platform::KeyCode::Return, true);
    input.update(snap);
    TEST_CHECK(input.isActionHeld("Jump"));

    // Disable Menu -> Gameplay Space works again
    TEST_CHECK(input.setContextEnabled("Menu", false).hasValue());
    input.update(snap);
    TEST_CHECK(input.isActionHeld("Jump"));

    std::printf("[PASS] testAnalogDeadZoneMouseAndContexts\n");
}

void testJsonMappingAndValidationErrors() {
    GameplayInputSystem input;

    // Invalid context / binding checks
    TEST_CHECK(!input.registerContext("").hasValue());
    TEST_CHECK(!input.bindAction("MissingCtx", "Jump", InputBinding::fromKey(platform::KeyCode::W))
                    .hasValue());
    TEST_CHECK(input.registerContext("Gameplay", 0).hasValue());
    TEST_CHECK(
        !input.bindAction("Gameplay", "", InputBinding::fromKey(platform::KeyCode::W)).hasValue());

    InputBinding badDeadZone = InputBinding::fromGamepadAxis(platform::GamepadAxis::LeftX, 0, 1.0F);
    TEST_CHECK(!input.bindAction("Gameplay", "MoveX", badDeadZone).hasValue());

    // Valid JSON mapping load
    constexpr std::string_view kJson = R"({
        "contexts": [
            {
                "name": "Custom",
                "priority": 5,
                "enabled": true,
                "bindings": [
                    { "action": "MoveForward", "source": "Key", "key": "W", "scale": 1.0 },
                    { "action": "CameraLookX", "source": "MouseDeltaX", "scale": -0.005, "requireRightMouse": true }
                ]
            }
        ]
    })";
    TEST_CHECK(input.loadMappingsFromJsonString(kJson).hasValue());
    TEST_CHECK(input.isContextEnabled("Custom"));

    platform::InputSnapshot snap{};
    snap.addKeyEvent(platform::KeyCode::W, true);
    input.update(snap);
    TEST_CHECK(input.isActionHeld("MoveForward"));

    // Invalid JSON rejected
    TEST_CHECK(!input.loadMappingsFromJsonString("{bad json").hasValue());

    std::printf("[PASS] testJsonMappingAndValidationErrors\n");
}

void testCharacterInputIntegrationAndNoStaleIntent() {
    world::World world;
    physics::PhysicsWorld physicsWorld;
    world::registerHierarchySystems(world);
    physics::registerPhysicsComponents(world);
    physics::registerPhysicsSystems(world, physicsWorld);
    registerCharacterComponents(world);

    GameplayInputSystem input;
    input.installDefaultThirdPersonBindings();
    registerCharacterInputSystem(world, input);
    registerCharacterControllerSystems(world, physicsWorld);
    registerThirdPersonCameraSystems(world, physicsWorld);

    // Static ground
    auto ground = world.createEntity("Ground");
    world.setComponent(ground, world::TransformComponent(
                                   Transform{Vec3{0, -0.5F, 0}, Quat::identity(), Vec3::one()}));
    physics::BodyDesc gbd{};
    gbd.motionType = physics::MotionType::Static;
    gbd.collider = physics::ColliderDesc::makeBox(Vec3{25, 0.5F, 25});
    gbd.position = Vec3{0, -0.5F, 0};
    auto gb = physicsWorld.createBody(gbd);
    TEST_CHECK(gb.hasValue());
    world.setComponent(ground,
                       physics::RigidBodyComponent{gb.value(), physics::MotionType::Static, 0});

    // Character + Camera
    auto ch = world.createEntity("Player");
    world.addTag<CharacterTag>(ch);
    world.addTag<PlayerControlledTag>(ch);
    world.setComponent(ch, CharacterConfigComponent{});
    world.setComponent(ch, CharacterStateComponent{});
    world.setComponent(ch, CharacterMovementIntentComponent{});
    world.setComponent(
        ch, world::TransformComponent(Transform{Vec3{0, 0.02F, 0}, Quat::identity(), Vec3::one()}));

    // Second character WITHOUT PlayerControlledTag must NOT receive player input
    auto npc = world.createEntity("Npc");
    world.addTag<CharacterTag>(npc);
    world.setComponent(npc, CharacterConfigComponent{});
    world.setComponent(npc, CharacterStateComponent{});
    world.setComponent(npc, CharacterMovementIntentComponent{});
    world.setComponent(npc, world::TransformComponent(
                                Transform{Vec3{5, 0.02F, 0}, Quat::identity(), Vec3::one()}));

    auto cam = world.createEntity("ThirdPersonCamera");
    world.setComponent(cam, world::TransformComponent(Transform::identity()));
    world.setComponent(cam, render::CameraComponent{});
    world.setComponent(cam, ThirdPersonCameraComponent{});

    // Hold W + Q (orbit camera) + Space
    platform::InputSnapshot snap{};
    snap.addKeyEvent(platform::KeyCode::W, true);
    snap.addKeyEvent(platform::KeyCode::Q, true);
    snap.addKeyEvent(platform::KeyCode::Space, true);

    for (int i = 0; i < 15; ++i) {
        input.update(snap);
        world.progress(1.0F / 60.0F);
    }

    const auto* intentDuring = world.getComponent<CharacterMovementIntentComponent>(ch);
    const auto* stateDuring = world.getComponent<CharacterStateComponent>(ch);
    const auto* camDuring = world.getComponent<ThirdPersonCameraComponent>(cam);
    TEST_CHECK(std::abs(intentDuring->moveAxis.y - 1.0F) < 1e-4F);
    TEST_CHECK(intentDuring->jumpRequested);
    TEST_CHECK(stateDuring->authoritativePositionMeters.z < -0.2F);
    TEST_CHECK(camDuring->yawRadians > 0.2F);

    const auto* npcIntent = world.getComponent<CharacterMovementIntentComponent>(npc);
    TEST_CHECK(npcIntent != nullptr);
    TEST_CHECK(std::abs(npcIntent->moveAxis.x) < 1e-5F);
    TEST_CHECK(std::abs(npcIntent->moveAxis.y) < 1e-5F);
    TEST_CHECK(!npcIntent->jumpRequested);

    // Release all keys -> intent must immediately clear (no stale intent)
    snap.reset();
    input.update(snap);
    world.progress(1.0F / 60.0F);
    const auto* intentAfter = world.getComponent<CharacterMovementIntentComponent>(ch);
    TEST_CHECK(std::abs(intentAfter->moveAxis.x) < 1e-5F);
    TEST_CHECK(std::abs(intentAfter->moveAxis.y) < 1e-5F);
    TEST_CHECK(!intentAfter->jumpRequested);

    std::printf("[PASS] testCharacterInputIntegrationAndNoStaleIntent\n");
}

class DummyConductorMode final : public Mode {
public:
    explicit DummyConductorMode(std::string_view name) : name_(name) {}
    [[nodiscard]] std::string_view getName() const noexcept override { return name_; }
    [[nodiscard]] Result<void> enter(world::World&) override { return Result<void>::success(); }
    void update(world::World&, Duration) override {}
    [[nodiscard]] Result<void> exit(world::World&) override { return Result<void>::success(); }
    [[nodiscard]] Result<PreparationStatus> prepare(const world::ReadOnlyWorld&, StateContext&,
                                                    const Budget&) override {
        return PreparationStatus::Ready;
    }
    [[nodiscard]] Result<void> commit(world::World&) override { return Result<void>::success(); }
    void cancelPrepare() override {}
    [[nodiscard]] ModeState getModeState() const noexcept override { return ModeState::Active; }

private:
    std::string_view name_;
};

void testConductorAndContextSwitchingInvariants() {
    world::World world;
    physics::PhysicsWorld physicsWorld;
    world::registerHierarchySystems(world);
    physics::registerPhysicsComponents(world);
    physics::registerPhysicsSystems(world, physicsWorld);
    registerCharacterComponents(world);

    GameplayInputSystem input;
    input.installDefaultThirdPersonBindings();
    registerCharacterInputSystem(world, input);
    registerCharacterControllerSystems(world, physicsWorld);

    auto ch = world.createEntity("Hero");
    world.addTag<CharacterTag>(ch);
    world.addTag<PlayerControlledTag>(ch);
    world.setComponent(ch, CharacterConfigComponent{});
    world.setComponent(ch, CharacterStateComponent{});
    world.setComponent(ch, CharacterMovementIntentComponent{});
    world.setComponent(ch, world::TransformComponent(Transform::identity()));

    Conductor conductor(world, std::make_unique<DummyConductorMode>("StateA"),
                        std::make_unique<DummyConductorMode>("StateB"), {});
    TEST_CHECK(conductor.initialize(ActiveSlot::SlotA).hasValue());

    const size_t entityCountBefore = world.getAllEntities().size();

    platform::InputSnapshot snap{};
    snap.addKeyEvent(platform::KeyCode::D, true);
    input.update(snap);
    conductor.update(Duration::fromMilliseconds(16));

    // Toggle input context and transition Conductor StateA -> StateB
    TEST_CHECK(input.setContextEnabled("Gameplay", false).hasValue());
    TEST_CHECK(conductor.requestTransition().hasValue());
    input.update(snap);
    conductor.update(Duration::fromMilliseconds(16));
    TEST_CHECK(conductor.executeTransition().hasValue());
    TEST_CHECK(world.getAllEntities().size() == entityCountBefore);
    TEST_CHECK(std::abs(world.getComponent<CharacterMovementIntentComponent>(ch)->moveAxis.x) <
               1e-5F);

    TEST_CHECK(input.setContextEnabled("Gameplay", true).hasValue());
    input.update(snap);
    conductor.update(Duration::fromMilliseconds(16));
    TEST_CHECK(std::abs(world.getComponent<CharacterMovementIntentComponent>(ch)->moveAxis.x -
                        1.0F) < 1e-4F);

    std::printf("[PASS] testConductorAndContextSwitchingInvariants\n");
}

} // namespace

int main() {
    TEST_CHECK(physics::PhysicsSystem::initialize().hasValue());

    testActionPressHoldReleaseAndRebinding();
    testAnalogDeadZoneMouseAndContexts();
    testJsonMappingAndValidationErrors();
    testCharacterInputIntegrationAndNoStaleIntent();
    testConductorAndContextSwitchingInvariants();

    physics::PhysicsSystem::shutdown();
    std::printf("All Milestone 12C gameplay_input_test cases PASSED!\n");
    return 0;
}
