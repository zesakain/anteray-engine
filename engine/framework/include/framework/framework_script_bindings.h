/// @file framework_script_bindings.h
/// @brief Controlled Lua 5.4 gameplay bindings for CharacterController and Two-State Conductor.
///
/// Purpose:
/// Registers safe, budgeted `character` and `conductor` modules into ScriptEngine without
/// exposing unrestricted mutable World access or editor internals.
///
/// Thread-safety:
/// Simulation thread only during runtime setup and script execution.

#pragma once

#include "framework/conductor.h"
#include "script/script_engine.h"
#include "world/world.h"

namespace eng::framework {

/// Registers `character` and `conductor` Lua modules into `scriptEngine`.
///
/// Mutating functions (`set_movement_intent`, `request_jump`, `request_transition`) are guarded
/// by `ScriptExecutionMode::Standby` so standby preparation scripts cannot mutate the World.
[[nodiscard]] Result<void, script::ScriptError>
registerFrameworkScriptBindings(script::ScriptEngine& scriptEngine, world::World& world,
                                Conductor* conductor = nullptr);

} // namespace eng::framework
