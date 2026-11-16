# ADR-0019: Lua Binding Strategy Reconciliation (Lua 5.4 C API vs. sol2)

- **Status**: Proposed
- **Date**: 2026-11-16
- **Authors**: zesakain

## Context

`AGENTS.md` §1 and `ADR-0014` (`Accepted`, 2026-11-07) specify **Lua 5.4 + sol2 v3.3.1** (`SOL_NO_EXCEPTIONS=1`, `SOL_ALL_SAFETIES_ON=1`). However, inspection of `engine/script/` reveals that only vendored **Lua 5.4** (`engine/script/src/lua/onelua.c`) is present in the repository; `sol2` is not vendored or fetched in `engine/script/CMakeLists.txt`, and `engine/script/src/script_engine.cpp` currently implements `Vec3`, `Entity`, and `print` bindings directly via the Lua 5.4 C API.

Milestone 12D/12E requires expanding Lua bindings to transforms, `CharacterController`, `GameplayInput`, `PhysicsWorld` spatial queries, and `Conductor` (active-state vs. read-only standby preparation).

## Decision

*Proposed (pending explicit approval of one of the two paths below):*

**Recommended Path — Option B (Complete `ADR-0014` by vendoring/pinning `sol2 v3.3.1` inside `engine/script/src/sol2/`):**
1. Vendor `sol2 v3.3.1` single-header (`sol/sol.hpp`, MIT License) privately inside `engine/script/src/sol2/` (never exposed in `engine/script/include/script/`), compiled with `-fno-exceptions -fno-rtti`, `SOL_NO_EXCEPTIONS=1`, and `SOL_ALL_SAFETIES_ON=1`.
2. Preserve all existing `ScriptEngine` guarantees:
   - Custom `scriptAlloc` allocator enforcing `ScriptConfig::memoryBudgetBytes`.
   - `lua_sethook` instruction watchdog (`LUA_MASKCOUNT`).
   - Stripped libraries (`os`, `io`, `package`, `debug`, `dofile`, `loadfile`, `load`, `collectgarbage`).
   - Incremental GC via `stepGarbageCollector(stepBudget)`.
3. Enforce **active-state vs. standby-state Lua permissions** via two distinct capability tables/environments:
   - **Active environment:** read/write access to active-mode entity components, `CharacterController` movement inputs, `GameplayInput` action queries, and `conductor.request_transition(...)` (validated).
   - **Standby preparation environment:** read-only `ReadOnlyWorld` queries and write access **only** to staged `SpeculativePreparationCache` / `TargetSelectionContext` handles; mutating ECS/physics/hierarchy functions are omitted from the standby Lua environment.

## Alternatives Considered

**Option A — Continue with raw Lua 5.4 C API only (superseding `ADR-0014`):**
- *Pros:* Zero new third-party headers; minimal binary/compile-time overhead; full manual control over stack and userdata lifetimes.
- *Cons:* Contradicts `AGENTS.md` §1 ("Scripting: Lua 5.4 + sol2") and `ADR-0014`; hand-writing stack-manipulation boilerplate for dozens of gameplay/physics/input/Conductor types increases maintenance burden and stack-imbalance bug risk.

## Consequences

### Positive (if Option B approved)
- Aligns implementation with `AGENTS.md` §1 and `ADR-0014` without violating header encapsulation (`engine/script/include/script/` remains free of `<lua.h>` and `<sol/sol.hpp>`).
- Type-safe usertype bindings and argument validation (`SOL_ALL_SAFETIES_ON=1`) with zero C++ exceptions (`SOL_NO_EXCEPTIONS=1`).

### Negative / Migration
- Adds ~250 KB single-header `sol.hpp` to `engine/script/src/` (requires explicit third-party dependency approval per `AGENTS.md` §2.7, even though `ADR-0014` previously selected `sol2 v3.3.1`).
- Existing `Vec3` / `Entity` C-API bindings in `script_engine.cpp` should be migrated or interoperate cleanly via `sol::usertype` with identical Lua metamethod names.
- Increase in `script_engine.cpp` compile time; mitigated by keeping all `sol2` usage inside `engine/script/src/`.
