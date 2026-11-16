/// @file clock_domain_test.cpp
/// @brief Unit tests for Decoupled Clocks (World, Action, UI) and slow-motion transition curves.

#include "framework/conductor.h"
#include "framework/mode.h"
#include "world/world.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <string_view>

using namespace eng;
using namespace eng::framework;
using namespace eng::world;

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << '\n'; \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

class SimpleMode : public Mode {
public:
    explicit SimpleMode(std::string_view name) noexcept : name_(name) {}
    [[nodiscard]] std::string_view getName() const noexcept override { return name_; }
    [[nodiscard]] Result<void> enter(World& /*world*/) override { return Result<void>::success(); }
    void update(World& /*world*/, Duration /*delta*/) override {}
    [[nodiscard]] Result<void> exit(World& /*world*/) override { return Result<void>::success(); }
    [[nodiscard]] Result<PreparationStatus> prepare(const ReadOnlyWorld& /*world*/,
                                                    StateContext& /*standbyContext*/,
                                                    const Budget& /*budget*/) override {
        return PreparationStatus::Ready;
    }
    [[nodiscard]] Result<void> commit(World& /*world*/) override { return Result<void>::success(); }
    void cancelPrepare() override {}
    [[nodiscard]] ModeState getModeState() const noexcept override { return ModeState::Active; }

private:
    std::string_view name_;
};

static int testDecoupledClockDomains() {
    World world;
    auto modeA = std::make_unique<SimpleMode>("ModeA");
    auto modeB = std::make_unique<SimpleMode>("ModeB");

    ConductorConfig config{
        .prepareBudgetUs = 500,
        .slowMotionDuration = Duration::fromMilliseconds(300),
        .slowMotionInitialScale = 0.2F,
    };

    Conductor conductor(world, std::move(modeA), std::move(modeB), config);
    (void)conductor.initialize(ActiveSlot::SlotA);

    // Initial state: all clocks at 1.0 scale
    TEST_CHECK(std::abs(conductor.getWorldClock().timeScale() - 1.0F) < 0.001F);
    TEST_CHECK(std::abs(conductor.getActionClock().timeScale() - 1.0F) < 0.001F);
    TEST_CHECK(std::abs(conductor.getUiClock().timeScale() - 1.0F) < 0.001F);

    // Advance 100ms
    conductor.update(Duration::fromMilliseconds(100));
    TEST_CHECK(conductor.getWorldClock().totalTime() == Duration::fromMilliseconds(100));
    TEST_CHECK(conductor.getActionClock().totalTime() == Duration::fromMilliseconds(100));
    TEST_CHECK(conductor.getUiClock().totalTime() == Duration::fromMilliseconds(100));

    // Request and prepare transition
    (void)conductor.requestTransition();
    conductor.update(Duration::fromMilliseconds(16));
    TEST_CHECK(conductor.getState() == ConductorState::Prepared);

    // Execute transition: slow-motion curve kicks off
    auto execRes = conductor.executeTransition();
    TEST_CHECK(execRes.hasValue());
    TEST_CHECK(conductor.getState() == ConductorState::Transitioning);

    // Initial scale on ActionClock should be 0.2f
    TEST_CHECK(std::abs(conductor.getActionClock().timeScale() - 0.2F) < 0.001F);
    // World and UI clocks must remain unscaled (1.0f)
    TEST_CHECK(std::abs(conductor.getWorldClock().timeScale() - 1.0F) < 0.001F);
    TEST_CHECK(std::abs(conductor.getUiClock().timeScale() - 1.0F) < 0.001F);

    // Advance 150ms (halfway through the 300ms curve)
    conductor.update(Duration::fromMilliseconds(150));
    TEST_CHECK(conductor.getState() == ConductorState::Transitioning);
    // Expected scale at halfway: 0.2 + 0.5 * (1.0 - 0.2) = 0.6
    TEST_CHECK(std::abs(conductor.getActionClock().timeScale() - 0.6F) < 0.05F);
    TEST_CHECK(std::abs(conductor.getWorldClock().timeScale() - 1.0F) < 0.001F);
    TEST_CHECK(std::abs(conductor.getUiClock().timeScale() - 1.0F) < 0.001F);

    // Advance another 160ms (surpassing 300ms total)
    conductor.update(Duration::fromMilliseconds(160));
    // Curve complete: Conductor transitions back to Active, scale restored to 1.0
    TEST_CHECK(conductor.getState() == ConductorState::Active);
    TEST_CHECK(std::abs(conductor.getActionClock().timeScale() - 1.0F) < 0.001F);
    TEST_CHECK(std::abs(conductor.getWorldClock().timeScale() - 1.0F) < 0.001F);
    TEST_CHECK(std::abs(conductor.getUiClock().timeScale() - 1.0F) < 0.001F);

    // Invariant: ActionClock accumulated simulation time is lower than WorldClock real elapsed time
    TEST_CHECK(conductor.getActionClock().totalTime() < conductor.getWorldClock().totalTime());

    return 0;
}

class FailingCommitMode : public SimpleMode {
public:
    explicit FailingCommitMode(std::string_view name) noexcept : SimpleMode(name) {}
    [[nodiscard]] Result<void> commit(World& /*world*/) override {
        return Error{.code = ErrorCode::CommitValidationFailed, .message = "Simulated fail"};
    }
};

static int testClockRoutingAcrossSystemGroups() {
    World world;
    auto modeA = std::make_unique<SimpleMode>("ModeA");
    auto modeB = std::make_unique<SimpleMode>("ModeB");

    ConductorConfig config{
        .prepareBudgetUs = 500,
        .slowMotionDuration = Duration::fromMilliseconds(300),
        .slowMotionInitialScale = 0.2F,
    };

    Conductor conductor(world, std::move(modeA), std::move(modeB), config);
    TEST_CHECK(conductor.initialize(ActiveSlot::SlotA).hasValue());

    int contCalls = 0;
    int stateACalls = 0;
    int stateBCalls = 0;
    int transCalls = 0;
    float lastContDt = 0.0F;
    float lastStateADt = 0.0F;
    float lastStateBDt = 0.0F;
    float lastTransDt = 0.0F;

    auto& ecs = world.getRawWorld();
    auto sCont = ecs.system<>("TestCont").kind(flecs::OnUpdate).run([&](flecs::iter& it) {
        ++contCalls;
        lastContDt = it.delta_time();
    });
    auto sA = ecs.system<>("TestA").kind(flecs::OnUpdate).run([&](flecs::iter& it) {
        ++stateACalls;
        lastStateADt = it.delta_time();
    });
    auto sB = ecs.system<>("TestB").kind(flecs::OnUpdate).run([&](flecs::iter& it) {
        ++stateBCalls;
        lastStateBDt = it.delta_time();
    });
    auto sT = ecs.system<>("TestT").kind(flecs::OnUpdate).run([&](flecs::iter& it) {
        ++transCalls;
        lastTransDt = it.delta_time();
    });

    world.registerSystem(SystemGroup::Continuous, sCont);
    world.registerSystem(SystemGroup::StateA, sA);
    world.registerSystem(SystemGroup::StateB, sB);
    world.registerSystem(SystemGroup::Transition, sT);

    // 1. Steady StateA: both Continuous and StateA run once with unscaled 20ms
    conductor.update(Duration::fromMilliseconds(20));
    TEST_CHECK(contCalls == 1 && stateACalls == 1 && stateBCalls == 0 && transCalls == 0);
    TEST_CHECK(std::abs(lastContDt - 0.020F) < 1e-5F);
    TEST_CHECK(std::abs(lastStateADt - 0.020F) < 1e-5F);

    // 2. Prepare and execute transition A -> B
    TEST_CHECK(conductor.requestTransition().hasValue());
    conductor.update(Duration::fromMilliseconds(20));
    TEST_CHECK(conductor.getState() == ConductorState::Prepared);
    TEST_CHECK(conductor.executeTransition().hasValue());
    TEST_CHECK(conductor.getState() == ConductorState::Transitioning);

    // 3. During slow-motion transition: Continuous must receive unscaled 20ms, while StateB and
    //    Transition receive scaled ActionClock delta (~0.2533 * 20ms = ~5.07ms)
    contCalls = stateACalls = stateBCalls = transCalls = 0;
    conductor.update(Duration::fromMilliseconds(20));
    TEST_CHECK(contCalls == 1);
    TEST_CHECK(stateACalls == 0);
    TEST_CHECK(stateBCalls == 1);
    TEST_CHECK(transCalls == 1);
    TEST_CHECK(std::abs(lastContDt - 0.020F) < 1e-5F);
    TEST_CHECK(lastStateBDt > 0.0F && lastStateBDt < 0.010F);
    TEST_CHECK(std::abs(lastTransDt - lastStateBDt) < 1e-5F);

    // 4. Complete transition curve (> 300ms): StateB returns to 1.0x unscaled, Transition stops
    conductor.update(Duration::fromMilliseconds(300));
    TEST_CHECK(conductor.getState() == ConductorState::Active);
    contCalls = stateACalls = stateBCalls = transCalls = 0;
    conductor.update(Duration::fromMilliseconds(20));
    TEST_CHECK(contCalls == 1 && stateACalls == 0 && stateBCalls == 1 && transCalls == 0);
    TEST_CHECK(std::abs(lastContDt - 0.020F) < 1e-5F);
    TEST_CHECK(std::abs(lastStateBDt - 0.020F) < 1e-5F);

    // 5. Failed transition back to A must preserve StateB active and not scale clocks
    World world2;
    Conductor cond2(world2, std::make_unique<SimpleMode>("A"),
                    std::make_unique<FailingCommitMode>("B"), config);
    TEST_CHECK(cond2.initialize(ActiveSlot::SlotA).hasValue());
    TEST_CHECK(cond2.requestTransition().hasValue());
    cond2.update(Duration::fromMilliseconds(10));
    TEST_CHECK(cond2.getState() == ConductorState::Prepared);
    TEST_CHECK(cond2.executeTransition().isError());
    TEST_CHECK(cond2.getActiveSlot() == ActiveSlot::SlotA);
    TEST_CHECK(cond2.getState() == ConductorState::Active);
    TEST_CHECK(cond2.getFailedTransitionsCount() == 1);
    TEST_CHECK(std::abs(cond2.getActionClock().timeScale() - 1.0F) < 1e-5F);

    return 0;
}

int main() {
    if (testDecoupledClockDomains() != 0) {
        return 1;
    }
    if (testClockRoutingAcrossSystemGroups() != 0) {
        return 1;
    }
    std::cout << "All Clock Domain unit tests passed successfully.\n";
    return 0;
}
