# Third-Person Character Template (Milestone 12F)

## Overview

The Third-Person Character Template provides a reusable, game-agnostic character setup built on Hikari Machine's existing Character Framework, Jolt Physics, `GameplayInputSystem`, `ThirdPersonCamera`, and skeletal `CharacterAnimation` pipeline.

It is packaged as a data-driven prefab (`assets/prefabs/third_person_character.hikari_prefab`) and exposed through the Hub's `third_person_character` project template (`TemplateRegistry`). It does not depend on the Bistro benchmark scene or any game-specific logic.

---

## Architecture & Separation of Concerns

The template separates **Character Core**, **Movement & Input**, **Camera**, **Animation**, and **Appearance** across existing ECS components:

- **Character Core** (`PlayerCharacter` root entity):
  - `CharacterTag`, `PlayerControlledTag`
  - `CharacterConfigComponent` (capsule dimensions, movement speeds/accelerations, jump/gravity, slope/step limits, mass)
  - `CharacterStateComponent` (velocity, ground normal, facing yaw, grounded flag)
  - `RigidBodyComponent` (`Kinematic`, layer `2`) + `ColliderComponent` (`Capsule`)
- **Movement & Input**:
  - `CharacterMovementIntentComponent` (`moveAxis`, `jumpRequested`, `scriptJumpRequested`, `cameraYawRadians`)
  - Populated by `GameplayInputSystem` (WASD / Space / Mouse orbit) or Lua (`character.set_movement_intent`, `character.request_jump`), and consumed by `CharacterController` (`registerCharacterControllerSystems`).
  - `CharacterController` never polls keyboard/mouse directly.
- **Camera** (`ThirdPersonCamera` entity):
  - `CameraComponent` (`isPrimary = true`)
  - `ThirdPersonCameraComponent` (`targetEntityId` wired to `PlayerCharacter`, orbit yaw/pitch limits, distance, height offset, raycast collision radius, smooth speed).
- **Animation** (`PlayerCharacter`):
  - `CharacterAnimationComponent` (`skeletonMeshPath`, `animationLibraryPath`, semantic clip names `idleClip`, `walkClip`, `runClip`, `jumpClip`, `inAirClip`, `landClip`, speed thresholds, blend/landing durations).
- **Appearance** (`PlayerVisual` child entity):
  - `MeshComponent` (`assetPath` referencing the glTF/GLB skeletal mesh and its materials).

---

## Instantiating the Template

### C++ (`ThirdPersonCharacterTemplate` / `PrefabLoader`)

```cpp
#include "framework/third_person_character_template.h"

eng::framework::PrefabInstantiateOptions opts{};
opts.rootTransformOverride = Transform::fromPosition(Vec3{0.0F, 1.0F, 0.0F});
opts.validateAppearanceAssets = true;

auto res = eng::framework::ThirdPersonCharacterTemplate::instantiate(world, opts);
```

- `ThirdPersonCharacterTemplate::instantiate()` loads `assets/prefabs/third_person_character.hikari_prefab`, validates all components and (when `validateAppearanceAssets = true`) verifies appearance asset paths on disk before creating any entities, and wires `ThirdPersonCameraComponent::targetEntityId` to the spawned `PlayerCharacter`.
- On any validation or hierarchy error, instantiation rolls back transactionally with zero partial entities left in `world`.

### Hikari Hub

Selecting the **Third-Person Character (TPP)** template (`id = "third_person_character"`) in Hub creates a project with:
- `Scenes/ThirdPersonTest.hikari`
- `Assets/Prefabs/third_person_character.hikari_prefab`
- `README.md` and `.hikari-project` manifest.

---

## Replacing Character Appearance

The default humanoid mesh (`assets/characters/player/Superhero_Male_FullBody.gltf`) and animations (`assets/animations/player/UAL1_Standard.glb`) are CC0 1.0 example assets (see `LICENSE_CC0.txt` in their respective directories).

To replace appearance without modifying `CharacterController`, `GameplayInputSystem`, or `ThirdPersonCamera`:

1. Pass `CharacterAppearanceOverride` in `PrefabInstantiateOptions`:
   ```cpp
   PrefabInstantiateOptions opts{};
   opts.validateAppearanceAssets = true;
   opts.appearanceOverride = CharacterAppearanceOverride{
       .meshAssetPath = "assets/characters/custom/Hero.gltf",
       .skeletonMeshPath = "assets/characters/custom/Hero.gltf",
       .animationLibraryPath = "assets/animations/custom/Locomotion.glb",
   };
   ```
2. Or edit `Mesh.assetPath` on `PlayerVisual` and `CharacterAnimation` fields in the Editor Inspector (`Character Animation` section) or `.hikari_prefab` JSON.

### Skeleton & Animation Compatibility Limitations

- Replacement meshes must share a compatible skeleton hierarchy/bone naming with the configured `animationLibraryPath` (or provide their own matching `animationLibraryPath` and clip names).
- Automatic cross-skeleton animation retargeting is not implemented in this milestone.
- Missing or unreadable appearance assets are rejected when `validateAppearanceAssets = true` or logged safely without crashing during `resolveCharacterAnimationAssets()`.

---

## Configuring Input, Movement, and Camera

- **Input**: `GameplayInputSystem::installDefaultThirdPersonBindings()` binds `MoveForward`/`MoveBackward`/`MoveLeft`/`MoveRight` (WASD), `Jump` (Space), and `LookYaw`/`LookPitch` (Mouse). Rebind actions through `GameplayInputSystem` without modifying `CharacterController`.
- **Movement & Jump**: Tune `CharacterConfigComponent` in `.hikari_prefab`, `PrefabInstantiateOptions::characterConfigOverride`, or the Editor Inspector (`Character Config` section). All parameters use explicit units (`meters`, `m/s`, `m/s^2`, `rad/s`, `deg`, `kg`).
- **Camera**: Tune `ThirdPersonCameraComponent` in `.hikari_prefab` or the Editor Inspector (`Third-Person Camera` section).

---

## Reused Systems & Two-State Conductor Compatibility

- **Reused Systems**: `registerCharacterComponents`, `registerCharacterControllerSystems`, `registerCharacterAnimationComponents`, `registerCharacterAnimationSystems`, `registerThirdPersonCameraSystems`, `GameplayInputSystem`, `PrefabLoader`, and `SceneSerializer`.
- **Conductor**: All character entities live in the single shared `World`. State A/B transitions never duplicate character entities, physics continues across transitions, and standby preparation never mutates the authoritative `World`.

---

## Relation to Future Milestones (12G–12L)

Because movement is decoupled through `CharacterMovementIntentComponent` and Conductor control is exposed via `conductor.*` / `character.*` bindings, future Blueprint (12G–12J) and AI/NPC (12K) systems can drive the same `CharacterController` and `CharacterAnimation` pipeline by writing movement intent without duplicating character physics or camera logic.
