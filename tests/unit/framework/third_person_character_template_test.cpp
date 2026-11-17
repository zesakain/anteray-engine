#include "framework/character_animation.h"
#include "framework/character_components.h"
#include "framework/character_controller.h"
#include "framework/conductor.h"
#include "framework/gameplay_input.h"
#include "framework/mode.h"
#include "framework/prefab.h"
#include "framework/scene_serializer.h"
#include "framework/third_person_camera.h"
#include "framework/third_person_character_template.h"
#include "physics/physics_components.h"
#include "physics/physics_system.h"
#include "physics/physics_world.h"
#include "platform/input_snapshot.h"
#include "render/camera_component.h"
#include "render/mesh_component.h"
#include "world/hierarchy_systems.h"
#include "world/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

using namespace eng;
using namespace eng::framework;
using namespace eng::world;

namespace {

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ << ": " << #cond    \
                      << '\n';                                                                     \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)

class DummyTemplateMode final : public Mode {
public:
    explicit DummyTemplateMode(std::string name) : name_(std::move(name)) {}
    [[nodiscard]] Result<void> enter(World&) override { return Result<void>::success(); }
    void update(World&, Duration) override {}
    [[nodiscard]] Result<void> exit(World&) override { return Result<void>::success(); }
    [[nodiscard]] Result<PreparationStatus> prepare(const ReadOnlyWorld&, StateContext&,
                                                    const Budget&) override {
        return PreparationStatus::Ready;
    }
    [[nodiscard]] Result<void> commit(World&) override { return Result<void>::success(); }
    void cancelPrepare() override {}
    [[nodiscard]] std::string_view getName() const noexcept override { return name_; }
    [[nodiscard]] ModeState getModeState() const noexcept override { return ModeState::Active; }

private:
    std::string name_;
};

void testTemplateInstantiationAndComponents() {
    World world;
    PrefabInstantiateOptions opts{};
    opts.validateAppearanceAssets = true;
    opts.rootTransformOverride = Transform{Vec3{1.0F, 0.5F, -2.0F}, Quat::identity(), Vec3::one()};

    auto res = ThirdPersonCharacterTemplate::instantiate(world, opts);
    TEST_CHECK(res.hasValue());

    const auto& out = res.value();
    TEST_CHECK(world.isAlive(out.characterRoot));
    TEST_CHECK(world.isAlive(out.visualMesh));
    TEST_CHECK(world.isAlive(out.cameraEntity));

    TEST_CHECK(world.hasComponent<CharacterTag>(out.characterRoot));
    TEST_CHECK(world.hasComponent<PlayerControlledTag>(out.characterRoot));
    TEST_CHECK(world.hasComponent<CharacterConfigComponent>(out.characterRoot));
    TEST_CHECK(world.hasComponent<CharacterStateComponent>(out.characterRoot));
    TEST_CHECK(world.hasComponent<CharacterMovementIntentComponent>(out.characterRoot));
    TEST_CHECK(world.hasComponent<CharacterAnimationComponent>(out.characterRoot));
    TEST_CHECK(world.hasComponent<physics::RigidBodyComponent>(out.characterRoot));
    TEST_CHECK(world.hasComponent<physics::ColliderComponent>(out.characterRoot));

    const auto* tc = world.getComponent<TransformComponent>(out.characterRoot);
    TEST_CHECK(tc != nullptr);
    TEST_CHECK(std::abs(tc->transform.position.x - 1.0F) < 1e-4F);
    TEST_CHECK(std::abs(tc->transform.position.z + 2.0F) < 1e-4F);

    const auto* mc = world.getComponent<render::MeshComponent>(out.visualMesh);
    TEST_CHECK(mc != nullptr);
    TEST_CHECK(mc->assetPath == "assets/characters/player/Superhero_Male_FullBody.gltf");

    const auto* cam = world.getComponent<render::CameraComponent>(out.cameraEntity);
    const auto* tpc = world.getComponent<ThirdPersonCameraComponent>(out.cameraEntity);
    TEST_CHECK(cam != nullptr && cam->isPrimary);
    TEST_CHECK(tpc != nullptr);
    TEST_CHECK(tpc->targetEntityId == out.characterRoot.id);

    // Round-trip serialization preserves template configuration
    auto serRes =
        SceneSerializer::serializeToString(world, SceneMetadata{.name = "TPP_Template_RoundTrip"});
    TEST_CHECK(serRes.hasValue());

    World restored;
    registerCharacterComponents(restored);
    registerCharacterAnimationComponents(restored);
    restored.getRawWorld().component<ThirdPersonCameraComponent>("ThirdPersonCameraComponent");
    auto deserRes = SceneSerializer::deserializeFromString(restored, serRes.value());
    TEST_CHECK(deserRes.hasValue());
    TEST_CHECK(restored.getRawWorld().count<CharacterTag>() == 1);
    TEST_CHECK(restored.getRawWorld().count<ThirdPersonCameraComponent>() == 1);
    TEST_CHECK(restored.getRawWorld().count<CharacterAnimationComponent>() == 1);

    std::cout << "[PASS] testTemplateInstantiationAndComponents\n";
}

void testTransactionalValidationAndAppearanceModularity() {
    World world;

    // 1. Missing appearance asset fails transactionally with zero partial entities
    PrefabInstantiateOptions badAppearance{};
    badAppearance.validateAppearanceAssets = true;
    badAppearance.appearanceOverride = CharacterAppearanceOverride{
        .meshAssetPath = "assets/characters/does_not_exist.gltf",
    };
    auto badRes = ThirdPersonCharacterTemplate::instantiate(world, badAppearance);
    TEST_CHECK(!badRes.hasValue());
    TEST_CHECK(world.getAllEntities().empty());

    // 2. Invalid CharacterConfig override fails transactionally with zero partial entities
    PrefabInstantiateOptions badCfg{};
    CharacterConfigComponent invalidCfg{};
    invalidCfg.capsuleRadiusMeters = -0.5F;
    badCfg.characterConfigOverride = invalidCfg;
    auto badCfgRes = ThirdPersonCharacterTemplate::instantiate(world, badCfg);
    TEST_CHECK(!badCfgRes.hasValue());
    TEST_CHECK(world.getAllEntities().empty());

    // 3. Modular appearance replacement preserves CharacterConfig and ThirdPersonCamera
    PrefabInstantiateOptions customApp{};
    customApp.validateAppearanceAssets = true;
    customApp.appearanceOverride = CharacterAppearanceOverride{
        .meshAssetPath = "assets/models/cube.glb",
        .idleClip = "CustomIdle",
    };
    auto okRes = ThirdPersonCharacterTemplate::instantiate(world, customApp);
    TEST_CHECK(okRes.hasValue());

    const auto& out = okRes.value();
    const auto* mc = world.getComponent<render::MeshComponent>(out.visualMesh);
    const auto* ca = world.getComponent<CharacterAnimationComponent>(out.characterRoot);
    const auto* cfg = world.getComponent<CharacterConfigComponent>(out.characterRoot);
    const auto* tpc = world.getComponent<ThirdPersonCameraComponent>(out.cameraEntity);
    TEST_CHECK(mc != nullptr && mc->assetPath == "assets/models/cube.glb");
    TEST_CHECK(ca != nullptr && ca->idleClip == "CustomIdle");
    TEST_CHECK(cfg != nullptr && cfg->isValid());
    TEST_CHECK(tpc != nullptr && tpc->targetEntityId == out.characterRoot.id);

    // Missing animation assets fail safely during resolveCharacterAnimationAssets without crash
    auto* mutCa = world.getMutableComponent<CharacterAnimationComponent>(out.characterRoot);
    mutCa->skeletonMeshPath = "assets/characters/missing_skel.gltf";
    resolveCharacterAnimationAssets(world);
    TEST_CHECK(world.isAlive(out.characterRoot));

    std::cout << "[PASS] testTransactionalValidationAndAppearanceModularity\n";
}

void testInputMovementCameraAndConductorCompatibility() {
    World world;
    physics::PhysicsWorld physicsWorld;

    physics::registerPhysicsComponents(world);
    registerCharacterComponents(world);
    registerCharacterAnimationComponents(world);
    world.getRawWorld().component<ThirdPersonCameraComponent>("ThirdPersonCameraComponent");

    // Static ground plane
    const Transform groundTr{Vec3{0.0F, -0.5F, 0.0F}, Quat::identity(), Vec3::one()};
    EntityHandle ground = world.createEntity("Ground");
    world.setComponent(ground, TransformComponent{groundTr});
    world.setComponent(ground, physics::PhysicsTransformComponent{groundTr});
    world.setComponent(ground, physics::RigidBodyComponent{
                                   .motionType = physics::MotionType::Static,
                                   .collisionLayer = physics::CollisionLayer::kNonMoving,
                               });
    world.setComponent(ground, physics::ColliderComponent{
                                   .desc = physics::ColliderDesc::makeBox(Vec3{20.0F, 0.5F, 20.0F}),
                               });

    PrefabInstantiateOptions opts{};
    opts.validateAppearanceAssets = true;
    opts.rootTransformOverride = Transform{Vec3{0.0F, 0.1F, 0.0F}, Quat::identity(), Vec3::one()};
    auto instRes = ThirdPersonCharacterTemplate::instantiate(world, opts);
    TEST_CHECK(instRes.hasValue());
    const auto& out = instRes.value();

    GameplayInputSystem inputSys;
    inputSys.installDefaultThirdPersonBindings();

    physics::registerPhysicsSystems(world, physicsWorld);
    registerCharacterControllerSystems(world, physicsWorld);
    registerCharacterAnimationSystems(world);
    registerThirdPersonCameraSystems(world, physicsWorld);
    world::registerHierarchySystems(world);

    auto modeA = std::make_unique<DummyTemplateMode>("StateA");
    auto modeB = std::make_unique<DummyTemplateMode>("StateB");
    Conductor conductor(world, std::move(modeA), std::move(modeB));
    TEST_CHECK(conductor.initialize(ActiveSlot::SlotA).hasValue());

    const float startZ =
        world.getComponent<TransformComponent>(out.characterRoot)->transform.position.z;

    // Feed W + Space via GameplayInputSystem into CharacterMovementIntentComponent
    platform::InputSnapshot snap{};
    snap.addKeyEvent(platform::KeyCode::W, true);
    snap.addKeyEvent(platform::KeyCode::Space, true);

    for (int i = 0; i < 20; ++i) {
        inputSys.update(snap);
        if (auto* intent =
                world.getMutableComponent<CharacterMovementIntentComponent>(out.characterRoot)) {
            intent->moveAxis =
                Vec2{inputSys.getActionValue("MoveRight"), inputSys.getActionValue("MoveForward")};
            if (inputSys.isActionPressed("Jump")) {
                intent->jumpRequested = true;
            }
        }
        if (i == 5) {
            TEST_CHECK(conductor.requestTransition(true).hasValue());
        }
        conductor.update(Duration::fromMilliseconds(16));
        snap.reset();
        snap.addKeyEvent(platform::KeyCode::W, true);
    }

    // Character moved in shared World and was never duplicated during StateA -> StateB transition
    TEST_CHECK(world.getRawWorld().count<CharacterTag>() == 1);
    TEST_CHECK(conductor.getActiveSlot() == ActiveSlot::SlotB);
    TEST_CHECK(conductor.getSuccessfulTransitionsCount() == 1);

    const float endZ =
        world.getComponent<TransformComponent>(out.characterRoot)->transform.position.z;
    TEST_CHECK(std::abs(endZ - startZ) > 0.05F);
    TEST_CHECK(world.isAlive(out.cameraEntity));

    conductor.shutdown();
    std::cout << "[PASS] testInputMovementCameraAndConductorCompatibility\n";
}

} // namespace

int main() {
    std::cout << "[Running Third-Person Character Template (12F) Tests]\n";
    testTemplateInstantiationAndComponents();
    testTransactionalValidationAndAppearanceModularity();
    testInputMovementCameraAndConductorCompatibility();
    std::cout << "All Milestone 12F Third-Person Character Template tests passed.\n";
    return 0;
}
