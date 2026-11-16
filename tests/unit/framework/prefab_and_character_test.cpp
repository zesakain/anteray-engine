/// @file prefab_and_character_test.cpp
/// @brief Milestone 12A tests for Character components, reflection, .hikari_prefab, and Conductor
/// standby isolation.

#include "framework/character_components.h"
#include "framework/conductor.h"
#include "framework/mode.h"
#include "framework/prefab.h"
#include "framework/runtime_session.h"
#include "framework/scene_serializer.h"
#include "physics/physics_components.h"
#include "render/camera_component.h"
#include "script/script_components.h"
#include "world/components.h"
#include "world/world.h"

#include <cmath>
#include <iostream>
#include <memory>

using namespace eng;
using namespace eng::framework;
using namespace eng::world;

namespace {

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << '\n'; \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

constexpr const char* kValidCharacterPrefabJson = R"({
  "formatVersion": 1,
  "name": "StandardCharacter",
  "entities": [
    {
      "id": 1,
      "parent": 0,
      "name": "CharacterRoot",
      "components": {
        "Transform": {
          "position": [1.0, 2.0, 3.0],
          "rotation": [0.0, 0.0, 0.0, 1.0],
          "scale": [1.0, 1.0, 1.0]
        },
        "CharacterTag": {},
        "CharacterConfig": {
          "capsuleRadiusMeters": 0.4,
          "capsuleHalfHeightMeters": 0.75,
          "eyeHeightMeters": 1.6,
          "maxMoveSpeedMetersPerSec": 7.5,
          "maxSlopeDegrees": 45.0,
          "stepOffsetMeters": 0.35,
          "massKilograms": 75.0
        },
        "CharacterState": {
          "velocityMetersPerSec": [0.0, 0.0, 0.0],
          "facingYawRadians": 0.5,
          "isGrounded": true
        },
        "Collider": {
          "type": "Capsule",
          "radius": 0.4,
          "halfHeight": 0.75
        },
        "Script": {
          "scriptName": "character_controller",
          "enabled": true,
          "updateInterval": 0.0
        }
      }
    },
    {
      "id": 2,
      "parent": 1,
      "name": "CameraMount",
      "components": {
        "Transform": {
          "position": [0.0, 1.6, 0.0],
          "rotation": [0.0, 0.0, 0.0, 1.0],
          "scale": [1.0, 1.0, 1.0]
        },
        "Camera": {
          "fovDegrees": 65.0,
          "nearPlane": 0.1,
          "farPlane": 500.0,
          "isPrimary": true
        }
      }
    }
  ]
})";

int testCharacterComponentsAndReflection() {
    World world;
    registerCharacterComponents(world);

    CharacterConfigComponent defaults{};
    TEST_CHECK(defaults.isValid());
    TEST_CHECK(defaults.capsuleRadiusMeters > 0.0F);
    TEST_CHECK(defaults.massKilograms > 0.0F);

    CharacterConfigComponent badRadius{};
    badRadius.capsuleRadiusMeters = 0.0F;
    TEST_CHECK(!badRadius.isValid());

    CharacterConfigComponent badSlope{};
    badSlope.maxSlopeDegrees = 95.0F;
    TEST_CHECK(!badSlope.isValid());

    const auto cfgFields = getCharacterConfigReflectionFields();
    TEST_CHECK(cfgFields.size() == 15);
    TEST_CHECK(cfgFields[0].name == "capsuleRadiusMeters");

    const auto stateFields = getCharacterStateReflectionFields();
    TEST_CHECK(stateFields.size() == 8);
    TEST_CHECK(stateFields[0].name == "velocityMetersPerSec");

    const auto intentFields = getCharacterMovementIntentReflectionFields();
    TEST_CHECK(intentFields.size() == 4);
    TEST_CHECK(intentFields[0].name == "moveAxis");

    EntityHandle ch = world.createEntity("TestChar");
    world.addTag<CharacterTag>(ch);
    world.setComponent(ch, defaults);
    world.setComponent(ch, CharacterStateComponent{});

    TEST_CHECK(world.hasComponent<CharacterTag>(ch));
    TEST_CHECK(world.hasComponent<CharacterConfigComponent>(ch));
    TEST_CHECK(world.hasComponent<CharacterStateComponent>(ch));

    world.destroyEntity(ch);
    TEST_CHECK(!world.isAlive(ch));
    return 0;
}

int testPrefabValidationRejectsInvalidInputs() {
    // 1. Invalid JSON
    TEST_CHECK(PrefabLoader::parseFromString("{bad json").isError());

    // 2. Missing formatVersion
    TEST_CHECK(PrefabLoader::parseFromString(R"({"name":"x","entities":[]})").isError());

    // 3. Empty entities
    TEST_CHECK(PrefabLoader::parseFromString(R"({"formatVersion":1,"entities":[]})").isError());

    // 4. Duplicate IDs
    TEST_CHECK(PrefabLoader::parseFromString(R"({
      "formatVersion": 1,
      "entities": [
        {"id": 1, "parent": 0, "name": "A"},
        {"id": 1, "parent": 0, "name": "B"}
      ]
    })")
                   .isError());

    // 5. Dangling parent
    TEST_CHECK(PrefabLoader::parseFromString(R"({
      "formatVersion": 1,
      "entities": [{"id": 1, "parent": 99, "name": "Orphan"}]
    })")
                   .isError());

    // 6. Invalid CharacterConfig
    TEST_CHECK(PrefabLoader::parseFromString(R"({
      "formatVersion": 1,
      "entities": [{
        "id": 1, "parent": 0, "name": "BadChar",
        "components": {"CharacterConfig": {"capsuleRadiusMeters": -1.0}}
      }]
    })")
                   .isError());

    // 7. Unknown component
    TEST_CHECK(PrefabLoader::parseFromString(R"({
      "formatVersion": 1,
      "entities": [{
        "id": 1, "parent": 0, "name": "BadComp",
        "components": {"NonExistentComp": {}}
      }]
    })")
                   .isError());

    return 0;
}

int testPrefabInstantiationOverridesAndRollback() {
    auto parseRes = PrefabLoader::parseFromString(kValidCharacterPrefabJson);
    TEST_CHECK(parseRes.hasValue());
    const PrefabDescriptor& desc = parseRes.value();
    TEST_CHECK(desc.entities.size() == 2);

    World world;
    physics::registerPhysicsComponents(world);

    // 1. Instantiate with overrides
    PrefabInstantiateOptions opts{};
    opts.rootNameOverride = "PlayerHero";
    opts.rootTransformOverride = Transform(Vec3(10.0F, 0.0F, 5.0F));
    CharacterConfigComponent cfgOverride{};
    cfgOverride.maxMoveSpeedMetersPerSec = 9.0F;
    opts.characterConfigOverride = cfgOverride;
    opts.scriptNameOverride = "hero_logic";

    auto instRes = PrefabLoader::instantiate(world, desc, opts);
    TEST_CHECK(instRes.hasValue());
    const EntityHandle root = instRes.value();
    TEST_CHECK(world.isAlive(root));
    TEST_CHECK(world.getEntityName(root) == "PlayerHero");
    TEST_CHECK(world.hasComponent<CharacterTag>(root));

    const auto* cfg = world.getComponent<CharacterConfigComponent>(root);
    TEST_CHECK(cfg != nullptr && std::abs(cfg->maxMoveSpeedMetersPerSec - 9.0F) < 1e-4F);

    const auto* sc = world.getComponent<script::ScriptComponent>(root);
    TEST_CHECK(sc != nullptr && std::string_view(sc->scriptName) == "hero_logic");

    const auto children = world.getChildren(root);
    TEST_CHECK(children.size() == 1);
    const EntityHandle camEnt = children[0];
    TEST_CHECK(world.getEntityName(camEnt) == "CameraMount");
    const auto* camWt = world.getComponent<WorldTransformComponent>(camEnt);
    TEST_CHECK(camWt != nullptr);
    TEST_CHECK(std::abs(camWt->worldTransform.position.x - 10.0F) < 1e-4F);
    TEST_CHECK(std::abs(camWt->worldTransform.position.y - 1.6F) < 1e-4F);
    TEST_CHECK(std::abs(camWt->worldTransform.position.z - 5.0F) < 1e-4F);

    // 2. Serialization round-trip
    auto serRes = SceneSerializer::serializeToString(world);
    TEST_CHECK(serRes.hasValue());
    World worldCopy;
    registerCharacterComponents(worldCopy);
    physics::registerPhysicsComponents(worldCopy);
    auto deserRes = SceneSerializer::deserializeFromString(worldCopy, serRes.value(), true);
    TEST_CHECK(deserRes.hasValue());
    TEST_CHECK(worldCopy.getAllEntities().size() == 2);

    // 3. Rollback on failed instantiation (Dynamic RigidBody cannot be parented to another entity)
    auto dynamicPrefabRes = PrefabLoader::parseFromString(R"({
      "formatVersion": 1,
      "entities": [
        {
          "id": 1, "parent": 0, "name": "DynRoot",
          "components": {
            "Transform": {"position": [0, 0, 0]},
            "RigidBody": {"motionType": "Dynamic"}
          }
        },
        {"id": 2, "parent": 1, "name": "Child"}
      ]
    })");
    TEST_CHECK(dynamicPrefabRes.hasValue());
    const EntityHandle existingParent = world.createEntity("ExistingParent");
    const size_t countBefore = world.getAllEntities().size();
    PrefabInstantiateOptions badParentOpts{};
    badParentOpts.parent = existingParent;
    auto failRes = PrefabLoader::instantiate(world, dynamicPrefabRes.value(), badParentOpts);
    TEST_CHECK(failRes.isError());
    TEST_CHECK(world.getAllEntities().size() == countBefore);

    return 0;
}

class CharacterStandbyMode final : public Mode {
public:
    explicit CharacterStandbyMode(std::string_view name, EntityHandle targetChar)
        : name_(name), targetChar_(targetChar) {}

    [[nodiscard]] std::string_view getName() const noexcept override { return name_; }

    [[nodiscard]] Result<void> enter(World&) override { return Result<void>::success(); }

    void update(World&, Duration) override {}

    [[nodiscard]] Result<void> exit(World&) override { return Result<void>::success(); }

    [[nodiscard]] Result<PreparationStatus> prepare(const ReadOnlyWorld& world,
                                                    StateContext& standbyContext,
                                                    const Budget& /*budget*/) override {
        standbyContext.cache.clear();
        if (world.isAlive(targetChar_) && world.hasComponent<CharacterTag>(targetChar_)) {
            standbyContext.cache.preparedEntities.push_back(targetChar_);
            standbyContext.cache.isReady = true;
        }
        return PreparationStatus::Ready;
    }

    [[nodiscard]] Result<void> commit(World& world) override {
        if (!targetChar_.isValid() || !world.isAlive(targetChar_) ||
            !world.hasComponent<CharacterTag>(targetChar_)) {
            return Error{.code = ErrorCode::CommitValidationFailed,
                         .message = "Prepared character entity no longer alive"};
        }
        return Result<void>::success();
    }

    void cancelPrepare() override {}

    [[nodiscard]] ModeState getModeState() const noexcept override { return ModeState::Active; }

private:
    std::string_view name_;
    EntityHandle targetChar_;
};

int testConductorSharedCharacterAndRuntimeSession() {
    auto parseRes = PrefabLoader::parseFromString(kValidCharacterPrefabJson);
    TEST_CHECK(parseRes.hasValue());

    World world;
    auto instRes = PrefabLoader::instantiate(world, parseRes.value());
    TEST_CHECK(instRes.hasValue());
    const EntityHandle characterRoot = instRes.value();
    const size_t initialEntityCount = world.getAllEntities().size();
    TEST_CHECK(initialEntityCount == 2);

    // Repeated A/B transitions must never duplicate character entities
    Conductor conductor(world, std::make_unique<CharacterStandbyMode>("StateA", characterRoot),
                        std::make_unique<CharacterStandbyMode>("StateB", characterRoot));
    TEST_CHECK(conductor.initialize(ActiveSlot::SlotA).hasValue());

    for (int i = 0; i < 6; ++i) {
        TEST_CHECK(conductor.requestTransition().hasValue());
        conductor.update(Duration::fromMilliseconds(16));
        TEST_CHECK(conductor.getState() == ConductorState::Prepared);
        TEST_CHECK(conductor.executeTransition().hasValue());
        // Finish slow-motion transition
        conductor.update(Duration::fromMilliseconds(350));
        TEST_CHECK(conductor.getState() == ConductorState::Active);
        TEST_CHECK(world.getAllEntities().size() == initialEntityCount);
    }

    // Stale character entity must fail commit() and preserve currently active state
    const ActiveSlot slotBeforeFail = conductor.getActiveSlot();
    TEST_CHECK(conductor.requestTransition().hasValue());
    conductor.update(Duration::fromMilliseconds(16));
    TEST_CHECK(conductor.getState() == ConductorState::Prepared);
    world.destroyEntity(characterRoot);
    TEST_CHECK(conductor.executeTransition().isError());
    TEST_CHECK(conductor.getActiveSlot() == slotBeforeFail);
    TEST_CHECK(conductor.getState() == ConductorState::Active);
    TEST_CHECK(conductor.getFailedTransitionsCount() == 1);

    // RuntimeSession PIE cloning with character prefab
    World editWorld;
    auto editCharRes = PrefabLoader::instantiate(editWorld, parseRes.value());
    TEST_CHECK(editCharRes.hasValue());

    RuntimeSession session;
    session.setEditWorld(&editWorld);
    TEST_CHECK(session.play().hasValue());
    TEST_CHECK(session.getRuntimeWorld() != nullptr);
    TEST_CHECK(session.getRuntimeWorld()->getAllEntities().size() == 2);
    session.update(Duration::fromMilliseconds(16));
    TEST_CHECK(session.stop().hasValue());
    TEST_CHECK(editWorld.getAllEntities().size() == 2);

    return 0;
}

} // namespace

int main() {
    if (testCharacterComponentsAndReflection() != 0) {
        return 1;
    }
    if (testPrefabValidationRejectsInvalidInputs() != 0) {
        return 1;
    }
    if (testPrefabInstantiationOverridesAndRollback() != 0) {
        return 1;
    }
    if (testConductorSharedCharacterAndRuntimeSession() != 0) {
        return 1;
    }
    std::cout << "All Prefab & Character 12A tests PASSED!\n";
    return 0;
}
