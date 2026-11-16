# ADR-0020: Prefab Architecture for Shared World Entities

- **Status**: Accepted
- **Date**: 2026-11-16
- **Authors**: zesakain

## Context

Milestone 12A requires reusable Character and Gameplay Prefabs that instantiate into Hikari's single shared `World` (`ADR-0005`, `ADR-0008`, `ADR-0009`). Existing infrastructure includes:
- Flecs v4.0.4 (`flecs::Prefab`, `EcsIsA`, slot relationships) inside `engine/world/src/flecs/`.
- `eng::world::World` managing `activeEntities_`, `rootEntities_`, `EntityHandle`, and `HierarchyValidator`.
- `eng::framework::SceneSerializer` using Hikari's JSON DOM (`core/json.h`) with transactional validation and atomic writes for `.hikari` scenes, plus `RuntimeSession::play()` cloning `editWorld` into `runtimeWorld` via `SceneSerializer::serializeToString` / `deserializeFromString`.

## Decision

*Proposed (pending explicit approval):*

Adopt a **Data-Driven JSON Prefab Descriptor (`.hikari_prefab`) backed by transactional instantiation into `World`** (Option B), rather than exposing `flecs::Prefab` / `EcsIsA` directly across engine boundaries:

1. **Format & Validation:** Prefabs are authored as JSON documents (`.hikari_prefab`) sharing the same component schema and validation logic as `SceneSerializer` (`TransformComponent`, `MeshComponent`, `RigidBodyComponent`, `ColliderComponent`, `CharacterControllerComponent`, `ScriptComponent`, `CameraComponent`, child hierarchy). Invalid or unknown schemas are rejected with file/key/line diagnostics (`AGENTS.md` §8 Data).
2. **Single Shared Entity Instantiation:** `PrefabSystem::instantiate(World&, const PrefabAsset&, const Transform& rootTransform, EntityHandle parent)` creates **one** authoritative entity hierarchy in the shared `World` (never duplicated for State A vs. State B). State-specific behavior is attached via `StateContext` references and `SystemGroup`-gated systems.
3. **Override Semantics:** Instance-level overrides (e.g., spawn transform, team/faction, script parameters) are applied at instantiation time onto the spawned entity's components and serialized normally by `SceneSerializer` (so `RuntimeSession::play()` PIE world cloning works out-of-the-box without requiring `flecs::Prefab` runtime inheritance resolution).
4. **Asset & Editor Integration:** Prefab descriptors are loaded/cached via `AssetManager` (off hot path) and instantiated on the main thread or during `Mode::commit()` after `Mode::prepare()` pre-loads referenced mesh/material/script assets.

## Alternatives Considered

1. **Native Flecs `flecs::Prefab` + `EcsIsA` (Option A):**
   - *Pros:* Zero-copy shared component memory for un-overridden fields; native Flecs hierarchy instantiation.
   - *Cons:* Leaks Flecs-specific `EcsIsA`/`EcsPrefab` semantics out of `engine/world` (`ADR-0008` requires encapsulating Flecs inside `engine/world`); complicates `SceneSerializer` round-tripping during `RuntimeSession::play()` and Editor Inspector editing (which expects concrete mutable components on entities); `World::activeEntities_` / `rootEntities_` tracking would need special filtering for `EcsPrefab` template entities.
2. **Hybrid (`flecs::Prefab` runtime cache compiled from JSON):**
   - Adds complexity prematurely before a second user proves the memory need (`AGENTS.md` §2.8). Can be added transparently behind `PrefabAsset` later without changing the `.hikari_prefab` format or public API.

## Consequences

### Positive
- Seamless compatibility with `SceneSerializer`, `AsyncSceneLoader`, `RuntimeSession::play()` PIE isolation, and the Editor Inspector/Hierarchy panels.
- Keeps Flecs encapsulated inside `engine/world` (`ADR-0008`).
- Pre-loading prefab assets during `Mode::prepare()` and instantiating in `Mode::commit()` avoids disk I/O in the hot path.

### Negative
- Instantiated entities store their own component copies rather than sharing immutable base component memory via `EcsIsA` (acceptable for character/gameplay entity counts; can be upgraded to a hybrid internal cache later if needed).
