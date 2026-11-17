/// @file framework_script_bindings.h
/// @brief Controlled Lua 5.4 gameplay bindings for CharacterController and Two-State Conductor.
///
/// Purpose:
/// Registers safe, budgeted `character` and `conductor` modules into ScriptEngine without
/// exposing unrestricted mutable World access or editor internals. Provides ConductorScriptBridge
/// to dispatch deterministic transition lifecycle hooks and standby read-only preparation hooks.
///
/// Thread-safety:
/// Simulation thread only during runtime setup and script execution.

#pragma once

#include "framework/conductor.h"
#include "script/script_engine.h"
#include "world/world.h"

#include <cstdint>

namespace eng::framework {

/// Registers `character` and `conductor` Lua modules into `scriptEngine`.
///
/// Mutating functions (`set_movement_intent`, `request_jump`, `request_transition`,
/// `execute_transition`, `cancel_transition`) are guarded by `ScriptExecutionMode::Standby`
/// so standby preparation scripts cannot mutate the authoritative World or trigger transitions.
[[nodiscard]] Result<void, script::ScriptError>
registerFrameworkScriptBindings(script::ScriptEngine& scriptEngine, world::World& world,
                                Conductor* conductor = nullptr);

/// @class ConductorScriptBridge
/// @brief Observes Two-State Conductor transitions and dispatches Lua lifecycle & standby hooks.
///
/// Contract:
/// - Non-owning pointers to ScriptEngine and Conductor; both must outlive or call reset()
///   before destruction.
/// - Steady-state frames with no transition or `on_conductor_prepare` hook perform zero
///   heap allocations.
/// - Invokes `on_conductor_prepare(standbySlot, modeName, budgetMs)` with
///   `ScriptExecutionMode::Standby` enforced, restoring `ScriptExecutionMode::Active` on return.
class ConductorScriptBridge {
public:
    ConductorScriptBridge() noexcept = default;

    /// Binds the bridge to a ScriptEngine and Conductor and snapshots initial state.
    void bind(script::ScriptEngine* scriptEngine, Conductor* conductor) noexcept;

    /// Clears bound pointers and resets tracked state.
    void reset() noexcept;

    /// Polls Conductor state changes and dispatches Lua lifecycle and standby preparation hooks.
    void update();

private:
    script::ScriptEngine* scriptEngine_{nullptr};
    Conductor* conductor_{nullptr};
    ConductorState prevState_{ConductorState::Uninitialized};
    ActiveSlot prevActiveSlot_{ActiveSlot::SlotA};
    uint32_t prevSuccessCount_{0};
    uint32_t prevFailCount_{0};
    bool initialized_{false};
};

} // namespace eng::framework
