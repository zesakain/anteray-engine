#include "core/assert.h"
#include "core/time.h"
#include "framework/character_components.h"
#include "framework/conductor.h"
#include "framework/framework_script_bindings.h"
#include "framework/mode.h"
#include "framework/runtime_session.h"
#include "script/script_engine.h"
#include "world/world.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ << ": " << #cond    \
                      << '\n';                                                                     \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)

using namespace eng;
using namespace eng::framework;
using namespace eng::script;

namespace {

class TestScriptMode final : public Mode {
public:
    TestScriptMode(std::string name, std::string displayName, bool autoReady = true,
                   bool failCommit = false)
        : name_(std::move(name)), displayName_(std::move(displayName)), autoReady_(autoReady),
          failCommit_(failCommit) {}

    [[nodiscard]] Result<void> enter(world::World&) override {
        state_ = ModeState::Active;
        return Result<void>::success();
    }

    void update(world::World&, Duration) override {}

    [[nodiscard]] Result<void> exit(world::World&) override {
        state_ = ModeState::Standby;
        return Result<void>::success();
    }

    [[nodiscard]] Result<PreparationStatus> prepare(const world::ReadOnlyWorld&, StateContext&,
                                                    const Budget&) override {
        if (failPrepare_) {
            return PreparationStatus::Failed;
        }
        return autoReady_ ? PreparationStatus::Ready : PreparationStatus::InProgress;
    }

    [[nodiscard]] Result<void> commit(world::World&) override {
        if (failCommit_) {
            return Error{.code = ErrorCode::CommitValidationFailed,
                         .message = "Simulated commit validation failure"};
        }
        return Result<void>::success();
    }

    void cancelPrepare() override {}

    [[nodiscard]] std::string_view getName() const noexcept override { return name_; }
    [[nodiscard]] std::string_view getDisplayName() const noexcept override { return displayName_; }
    [[nodiscard]] ModeState getModeState() const noexcept override { return state_; }

    void setAutoReady(bool ready) noexcept { autoReady_ = ready; }
    void setFailCommit(bool fail) noexcept { failCommit_ = fail; }

private:
    std::string name_;
    std::string displayName_;
    ModeState state_{ModeState::Standby};
    bool autoReady_{true};
    bool failCommit_{false};
    bool failPrepare_{false};
};

void testInspectionAndClocks() {
    world::World world;
    auto modeA = std::make_unique<TestScriptMode>("ModeA", "Overworld");
    auto modeB = std::make_unique<TestScriptMode>("ModeB", "Combat");

    ConductorConfig cfg{
        .prepareBudgetUs = 500,
        .slowMotionDuration = Duration::fromMilliseconds(200),
        .slowMotionInitialScale = 0.25F,
    };
    Conductor conductor(world, std::move(modeA), std::move(modeB), cfg);
    TEST_CHECK(conductor.initialize(ActiveSlot::SlotA).hasValue());

    ScriptEngine engine;
    TEST_CHECK(engine.initialize().hasValue());
    engine.bindWorld(&world);
    TEST_CHECK(registerFrameworkScriptBindings(engine, world, &conductor).hasValue());

    auto res = engine.executeString(R"(
        assert(conductor.get_state() == "Active")
        assert(conductor.get_active_slot() == "SlotA")
        assert(conductor.get_standby_slot() == "SlotB")
        assert(conductor.get_active_mode_name() == "ModeA")
        assert(conductor.get_standby_mode_name() == "ModeB")
        assert(conductor.get_slot_display_name("SlotA") == "Overworld")
        assert(conductor.get_slot_display_name("SlotB") == "Combat")
        assert(not conductor.is_standby())
        assert(not conductor.is_preparing())
        assert(not conductor.is_prepared())
        assert(not conductor.is_transitioning())
        assert(not conductor.has_pending_transition())
        assert(conductor.get_pending_target_slot() == nil)
        assert(conductor.get_successful_transitions() == 0)
        assert(conductor.get_failed_transitions() == 0)
        assert(conductor.get_action_time_scale() == 1.0)
        return true
    )");
    TEST_CHECK(res.hasValue() && std::get<bool>(res.value()));

    conductor.update(Duration::fromMilliseconds(16));
    auto timeRes = engine.executeString(R"(
        assert(conductor.get_world_time() > 0.01)
        assert(conductor.get_action_time() > 0.01)
        assert(conductor.get_ui_time() > 0.01)
        assert(conductor.get_world_delta() > 0.01)
        assert(conductor.get_action_delta() > 0.01)
        return true
    )");
    TEST_CHECK(timeRes.hasValue() && std::get<bool>(timeRes.value()));

    engine.unbindWorld();
    engine.shutdown();
    conductor.shutdown();
    std::cout << "[PASS] testInspectionAndClocks\n";
}

void testTransitionControlAndLifecycleHooks() {
    world::World world;
    auto modeA = std::make_unique<TestScriptMode>("ModeA", "Overworld", false);
    auto modeB = std::make_unique<TestScriptMode>("ModeB", "Combat", false);
    auto* modeBPtr = modeB.get();

    ConductorConfig cfg{
        .prepareBudgetUs = 500,
        .slowMotionDuration = Duration::fromMilliseconds(50),
        .slowMotionInitialScale = 0.2F,
    };
    Conductor conductor(world, std::move(modeA), std::move(modeB), cfg);
    TEST_CHECK(conductor.initialize(ActiveSlot::SlotA).hasValue());

    ScriptEngine engine;
    TEST_CHECK(engine.initialize().hasValue());
    engine.bindWorld(&world);
    TEST_CHECK(registerFrameworkScriptBindings(engine, world, &conductor).hasValue());

    ConductorScriptBridge bridge;
    bridge.bind(&engine, &conductor);

    TEST_CHECK(engine
                   .executeString(R"(
        log_events = {}
        function on_transition_requested(from_slot, to_slot, target_mode)
            table.insert(log_events, "req:" .. from_slot .. "->" .. to_slot .. ":" .. target_mode)
        end
        function on_transition_prepared(standby_slot, target_mode)
            table.insert(log_events, "prep:" .. standby_slot .. ":" .. target_mode)
        end
        function on_transition_committed(prev_slot, new_slot, active_mode)
            table.insert(log_events, "commit:" .. prev_slot .. "->" .. new_slot .. ":" .. active_mode)
        end
        function on_transition_completed(active_slot, active_mode)
            table.insert(log_events, "done:" .. active_slot .. ":" .. active_mode)
        end
        function on_transition_cancelled(active_slot, active_mode)
            table.insert(log_events, "cancel:" .. active_slot .. ":" .. active_mode)
        end
        function on_transition_failed(preserved_slot, active_mode)
            table.insert(log_events, "fail:" .. preserved_slot .. ":" .. active_mode)
        end
    )")
                   .hasValue());

    // Request transition by display name ("Combat"), manual commit
    auto reqRes = engine.executeString(R"(
        assert(not conductor.request_transition("NonExistentMode", false))
        assert(conductor.request_transition("Combat", false))
        assert(conductor.is_preparing())
        assert(conductor.has_pending_transition())
        assert(conductor.get_pending_target_slot() == "SlotB")
        return true
    )");
    TEST_CHECK(reqRes.hasValue() && std::get<bool>(reqRes.value()));

    conductor.update(Duration::fromMilliseconds(16));
    bridge.update();

    // Mark ModeB ready and advance to Prepared
    modeBPtr->setAutoReady(true);
    conductor.update(Duration::fromMilliseconds(16));
    bridge.update();
    TEST_CHECK(conductor.getState() == ConductorState::Prepared);

    // Execute transition from Lua
    auto execRes = engine.executeString(R"(
        assert(conductor.is_prepared())
        assert(conductor.execute_transition())
        assert(conductor.is_transitioning())
        assert(conductor.get_active_slot() == "SlotB")
        assert(conductor.get_active_mode_name() == "ModeB")
        assert(conductor.get_successful_transitions() == 1)
        assert(conductor.get_action_time_scale() < 0.99)
        return true
    )");
    TEST_CHECK(execRes.hasValue() && std::get<bool>(execRes.value()));
    bridge.update();

    // Complete slow-motion curve
    conductor.update(Duration::fromMilliseconds(60));
    bridge.update();
    TEST_CHECK(conductor.getState() == ConductorState::Active);

    auto hookRes = engine.executeString(R"(
        assert(#log_events == 4)
        assert(log_events[1] == "req:SlotA->SlotB:ModeB")
        assert(log_events[2] == "prep:SlotB:ModeB")
        assert(log_events[3] == "commit:SlotA->SlotB:ModeB")
        assert(log_events[4] == "done:SlotB:ModeB")
        return true
    )");
    TEST_CHECK(hookRes.hasValue() && std::get<bool>(hookRes.value()));

    // Test cancel_transition from Lua
    TEST_CHECK(engine
                   .executeString(R"(
        assert(conductor.request_transition("SlotA", false))
        assert(conductor.is_preparing())
        assert(conductor.cancel_transition())
        assert(conductor.get_state() == "Active")
        assert(conductor.get_active_slot() == "SlotB")
    )")
                   .hasValue());

    bridge.reset();
    engine.unbindWorld();
    engine.shutdown();
    conductor.shutdown();
    std::cout << "[PASS] testTransitionControlAndLifecycleHooks\n";
}

void testStandbyPreparationGuardAndSpeculativeCache() {
    world::World world;
    registerCharacterComponents(world);
    auto player = world.createEntity("Player");
    world.addTag<PlayerControlledTag>(player);
    world.setComponent(player, CharacterMovementIntentComponent{});

    auto target = world.createEntity("EnemyTarget");

    auto modeA = std::make_unique<TestScriptMode>("ModeA", "Overworld", false);
    auto modeB = std::make_unique<TestScriptMode>("ModeB", "Tactical", false);
    auto* modeBPtr = modeB.get();

    Conductor conductor(world, std::move(modeA), std::move(modeB));
    TEST_CHECK(conductor.initialize(ActiveSlot::SlotA).hasValue());

    ScriptEngine engine;
    TEST_CHECK(engine.initialize().hasValue());
    engine.bindWorld(&world);
    TEST_CHECK(registerFrameworkScriptBindings(engine, world, &conductor).hasValue());

    ConductorScriptBridge bridge;
    bridge.bind(&engine, &conductor);

    TEST_CHECK(engine
                   .executeString(R"(
        prepare_calls = 0
        mutation_blocked = false
        emit_blocked = false
        transition_blocked = false

        function on_conductor_prepare(standby_slot, mode_name, budget_ms)
            prepare_calls = prepare_calls + 1
            assert(standby_slot == "SlotB")
            assert(mode_name == "ModeB")
            assert(budget_ms > 0.0)
            assert(conductor.is_standby())

            -- Authoritative World mutations & event emission must be blocked in Standby
            local p = character.find_player()
            local ok1 = pcall(function() character.set_movement_intent(p, 1.0, 0.0) end)
            if not ok1 then mutation_blocked = true end

            local ok2 = pcall(function() events.emit("illegal", "data") end)
            if not ok2 then emit_blocked = true end

            local ok3 = pcall(function() conductor.request_transition(true) end)
            if not ok3 then transition_blocked = true end

            -- Speculative StateContext preparation must succeed
            conductor.prepare_clear()
            local enemy = entity.find_by_name("EnemyTarget")
            assert(conductor.prepare_add_entity(enemy))
            assert(conductor.prepare_set_primary_target(enemy))
            conductor.prepare_set_ready(true)
        end
    )")
                   .hasValue());

    TEST_CHECK(conductor.requestTransition(ActiveSlot::SlotB, false).hasValue());
    conductor.update(Duration::fromMilliseconds(16));
    bridge.update();

    // Ensure ExecutionMode was restored to Active after on_conductor_prepare
    TEST_CHECK(!engine.isStandbyMode());

    auto checkRes = engine.executeString(R"(
        assert(prepare_calls == 1)
        assert(mutation_blocked)
        assert(emit_blocked)
        assert(transition_blocked)
        assert(conductor.prepare_is_ready())
        assert(conductor.prepare_get_entity_count() == 1)
        assert(conductor.prepare_get_entity(1):is_alive())
        assert(conductor.get_primary_target("SlotB") ~= nil)
        assert(conductor.prepare_validate_entities())
        return true
    )");
    TEST_CHECK(checkRes.hasValue() && std::get<bool>(checkRes.value()));

    // Destroy the cached entity in the shared World and verify revalidation catches staleness
    world.destroyEntity(target);
    auto staleRes = engine.executeString(R"(
        assert(not conductor.prepare_validate_entities())
        assert(conductor.prepare_get_entity_count() == 0)
        return true
    )");
    TEST_CHECK(staleRes.hasValue() && std::get<bool>(staleRes.value()));

    // Verify commit failure triggers on_transition_failed and preserves SlotA
    modeBPtr->setAutoReady(true);
    modeBPtr->setFailCommit(true);
    conductor.update(Duration::fromMilliseconds(16));
    bridge.update();
    TEST_CHECK(conductor.getState() == ConductorState::Prepared);

    auto commitRes = conductor.executeTransition();
    TEST_CHECK(!commitRes.hasValue());
    bridge.update();
    TEST_CHECK(conductor.getState() == ConductorState::Active);
    TEST_CHECK(conductor.getActiveSlot() == ActiveSlot::SlotA);
    TEST_CHECK(conductor.getFailedTransitionsCount() == 1);

    bridge.reset();
    engine.unbindWorld();
    engine.shutdown();
    conductor.shutdown();
    std::cout << "[PASS] testStandbyPreparationGuardAndSpeculativeCache\n";
}

void testRuntimeSessionIntegration() {
    world::World editWorld;
    RuntimeSession session;
    session.setEditWorld(&editWorld);
    TEST_CHECK(session.play().hasValue());
    session.update(Duration::fromMilliseconds(16));
    auto* cond = session.getActiveConductor();
    TEST_CHECK(cond != nullptr);
    TEST_CHECK(cond->requestTransition(true).hasValue());
    session.update(Duration::fromMilliseconds(16));
    TEST_CHECK(cond->getSuccessfulTransitionsCount() == 1);
    TEST_CHECK(session.stop().hasValue());
    std::cout << "[PASS] testRuntimeSessionIntegration\n";
}

} // namespace

int main() {
    std::cout << "[Running Conductor Script (12E) Tests]\n";
    testInspectionAndClocks();
    testTransitionControlAndLifecycleHooks();
    testStandbyPreparationGuardAndSpeculativeCache();
    testRuntimeSessionIntegration();
    std::cout << "All Milestone 12E Conductor Lua API tests passed.\n";
    return 0;
}
