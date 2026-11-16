# ADR-0019: Lua Binding Strategy — Encapsulated Lua 5.4 C API (Superseding sol2 in ADR-0014)

- **Status**: Proposed
- **Date**: 2026-11-16
- **Authors**: zesakain

## Context

`AGENTS.md` §1 lists `Scripting: Lua 5.4 + sol2` and `ADR-0014` (`Accepted`, 2026-11-07) selected `sol2 v3.3.1`. However, `AGENTS.md` §2.6 and §4 strictly enforce `-fno-exceptions`, `-fno-rtti`, no `dynamic_cast`, and `-Wall -Wextra -Wconversion -Wshadow -Werror` across all engine targets.

Because `sol2` relies on RTTI/type-info machinery and exception-oriented dispatch paths that conflict with Hikari's `-fno-exceptions -fno-rtti -Werror` build contract, `sol2` was intentionally omitted from `engine/script/`, and `eng::script::ScriptEngine` (`engine/script/src/script_engine.cpp`) was implemented directly on top of the vendored **Lua 5.4 C API** (`engine/script/src/lua/onelua.c`).

Milestone 12D/12E requires expanding Lua bindings to transforms, `CharacterController`, `GameplayInput`, `PhysicsWorld` spatial queries, and `Conductor` (active-state vs. read-only standby preparation).

## Decision

*Proposed (pending explicit approval; supersedes the `sol2` clause of `ADR-0014` and recommends updating `AGENTS.md` §1 from `Lua 5.4 + sol2` to `Lua 5.4`):*

1. **Official Binding Layer — Encapsulated Lua 5.4 C API with Thin C++20 Binding Helpers:**
   - Continue using the vendored Lua 5.4 C core (`engine/script/src/lua/onelua.c`, MIT License) behind the opaque `eng::script::ScriptEngine` PIMPL (`engine/script/include/script/script_engine.h`).
   - Do **not** add `sol2` to `/engine`.
   - Build lightweight, zero-exception, zero-RTTI C++20 helper templates/macros *privately* inside `engine/script/src/` (typed metatable registration, `EntityHandle` generational validation, `Result<T, E>` -> Lua error propagation, and reflection-driven component field wrappers per `AGENTS.md` §8).
2. **Preserve All Existing `ScriptEngine` Guarantees:**
   - Custom `scriptAlloc` allocator enforcing `ScriptConfig::memoryBudgetBytes`.
   - `lua_sethook` instruction watchdog (`LUA_MASKCOUNT`).
   - Stripped standard libraries (`os`, `io`, `package`, `debug`, `dofile`, `loadfile`, `load`, `collectgarbage`).
   - Incremental GC via `stepGarbageCollector(stepBudget)`.
3. **Active-State vs. Standby-State Lua Permissions:**
   - **Active environment:** read/write access to active-mode entity components, `CharacterController` movement inputs, `GameplayInput` action queries, and validated `conductor.request_transition(...)`.
   - **Standby preparation environment:** read-only `ReadOnlyWorld` queries and write access **only** to staged `SpeculativePreparationCache` / `TargetSelectionContext` handles; mutating ECS/physics/hierarchy functions are omitted from the standby Lua environment.

## Alternatives Considered

**Option B — Adopt `sol2 v3.3.1` (`SOL_NO_EXCEPTIONS=1`, `SOL_ALL_SAFETIES_ON=1`):**
- *Pros:* Higher-level C++ usertype syntax.
- *Cons:* Conflicts with `-fno-exceptions -fno-rtti` and `-Werror` (`-Wshadow`, `-Wconversion`) in `AGENTS.md` §2.6 / §4; adds a ~250 KB header dependency and significant compile-time overhead to `engine_script`. Rejected to preserve Hard Rule §2.6 without workarounds.

## Consequences

### Positive
- 100% compliant with `-fno-exceptions`, `-fno-rtti`, `-Werror`, and zero new third-party dependencies (`AGENTS.md` §2.6, §2.7).
- Zero `<lua.h>` or third-party headers in `engine/script/include/script/`.
- Deterministic memory budget (`scriptAlloc`), instruction watchdog, and incremental GC remain intact.

### Negative / Migration
- Requires maintaining small internal C-API binding helpers in `engine/script/src/` for Milestone 12D/12E types.
- Requires updating `AGENTS.md` §1 (`Lua 5.4 + sol2` -> `Lua 5.4`) and marking `ADR-0014` as superseded by `ADR-0019` upon approval.
