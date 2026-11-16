# ADR-0018: Standby Preparation Isolation via ReadOnlyWorld

- **Status**: Accepted
- **Date**: 2026-11-16
- **Authors**: zesakain

## Context

`eng::framework::Mode` is a frozen public API under `AGENTS.md` §12. Currently, `Mode::prepare(world::World& world, const Budget& budget)` passes a mutable `world::World&` into the standby mode. Even though `ADR-0009` specifies that `prepare()` must treat its output as a speculative cache and never mutate the shared `World`, the current C++ signature permits standby code to call `createEntity()`, `destroyEntity()`, `setComponent()`, `getMutableComponent()`, `setParent()`, `setSystemGroupActive()`, `progress()`, and `getRawWorld()`.

`eng::world::ReadOnlyWorld` has been introduced as a non-mutating facade exposing only `const` entity, component, hierarchy, and version-query APIs (omitting all mutators and `getRawWorld()`). However, switching `Mode::prepare(...)` from `world::World&` to `const world::ReadOnlyWorld&` (plus `StateContext& standbyContext`) breaks the frozen `Mode` interface and requires updating all 8 existing `Mode` subclasses.

## Decision

*Proposed (pending explicit approval):*

1. Change `Mode::prepare` from:
   ```cpp
   virtual Result<PreparationStatus> prepare(world::World& world, const Budget& budget) = 0;
   ```
   to:
   ```cpp
   virtual Result<PreparationStatus> prepare(const world::ReadOnlyWorld& world,
                                             StateContext& standbyContext,
                                             const Budget& budget) = 0;
   ```
2. Update `Conductor::update()` to construct `world::ReadOnlyWorld roWorld(world_)` and pass `(roWorld, getStandbyStateContext(), budget)` to `getStandbyMode().prepare(...)`.
3. Keep `Mode::commit(world::World& world)` mutable so validated speculative caches in `standbyContext.cache` are revalidated (`world.isAlive(handle)`) and applied atomically during `Conductor::executeTransition()`.

## Alternatives Considered

1. **`const world::World&` without a facade:** Still exposes `const flecs::world& World::getRawWorld() const` and allows Flecs C-API `const_cast` escape hatches. Rejected because `ReadOnlyWorld` closes raw Flecs access completely.
2. **Adding a parallel `prepareStandby()` while keeping `prepare(World&, ...)`:** Leaves the mutable `prepare()` path open and violates `AGENTS.md` §2.8 (no duplicate abstractions). Rejected.
3. **Second shadow/staging `World`:** Violates the Single Shared World invariant (`ADR-0005`, `ADR-0009`). Rejected.

## Consequences

### Positive
- Compile-time enforcement preventing standby `Mode::prepare()` from creating/destroying entities, mutating ECS components, altering hierarchy, or toggling `SystemGroup`s.
- Direct, type-safe access to `StateContext& standbyContext` (`SpeculativePreparationCache`, `TargetSelectionContext`) during standby preparation.

### Negative / Migration
- Breaking signature change to frozen `Mode::prepare` (requires updating `GenericRuntimeMode`, `StateAMode`, `StateBMode`, and 5 test mock `Mode` classes).
- Cannot be landed without explicit approval of this ADR.
