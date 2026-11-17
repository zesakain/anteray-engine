# Character Controller Third-Person Test Scene (`CharacterController_TPP_Test`)

## Purpose

Dedicated engineering test scene for validating Milestone 12B's physics-based third-person character controller (`CharacterControllerStep`) and follow camera (`ThirdPersonCameraFollow`).

## Files

- **Scene:** `assets/scenes/CharacterController_TPP_Test.hikari`
- **Character Prefab:** `assets/prefabs/third_person_character.hikari_prefab`
- **Character Model (CC0):** `assets/characters/player/Superhero_Male_FullBody.gltf` (Quaternius Universal Base Characters; license: `assets/characters/player/LICENSE_CC0.txt`)
- **Animation Library (CC0):** `assets/animations/player/UAL1_Standard.glb` (Quaternius Universal Animation Library; license: `assets/animations/player/LICENSE_CC0.txt`)

## How to Load & Test

1. **Via Hikari Hub:**
   - Open **Sample Scenes** -> select **CharacterController TPP Test** (`assets/scenes/CharacterController_TPP_Test.hikari`), or
   - Create a new project from **New Project** -> **Third-Person Character (TPP)** (`third_person_character` template).
2. **Via Hikari Editor:**
   - **File -> Open Scene...** -> click **TPP Character** quick-open button (or enter `assets/scenes/CharacterController_TPP_Test.hikari`), or browse `assets/scenes/` and `assets/prefabs/` in the **Content Browser**.
3. `PlayerCharacter` (entity ID `100`) and `ThirdPersonCamera` (entity ID `2`) are pre-placed at spawn `[0, 0.05, 0]`.
   - Alternatively, instantiate `assets/prefabs/third_person_character.hikari_prefab` via `PrefabLoader`.
4. Press **Play** (`RuntimeSession::play()`) to activate `GameplayInputSystem`, `PhysicsWorld`, `CharacterControllerStep`, and `ThirdPersonCameraFollow`.

## Controls (GameplayInputSystem Defaults)

| Input | Action |
|---|---|
| `W` / `S` | `MoveForward` / `MoveBackward` relative to camera horizontal yaw |
| `A` / `D` | `MoveLeft` / `MoveRight` relative to camera horizontal yaw |
| `Space` / Gamepad `South` | `Jump` (when grounded) |
| `Q` / `E` | `CameraOrbitLeft` / `CameraOrbitRight` |
| `Right Mouse + Drag` | `CameraLookX` / `CameraLookY` |

*(Bindings are configured via `GameplayInputSystem` in `"Gameplay"` context and can be rebound at runtime or loaded from JSON.)*

## Test Areas

### Area A: Main Ground & Spawn (`[0, 0, 0]`)
- `AreaA_MainGround`: `80m x 80m` flat static floor.
- Validates acceleration, deceleration, smooth rotation, and camera-relative horizontal movement.

### Area B: Elevation Tests (`z = -9 .. -18`)
- `AreaB_Ramp_Walkable_15deg` & `AreaB_Ramp_Walkable_30deg`: Walkable slopes (`<= 45°` `maxSlopeDegrees`); character climbs smoothly.
- `AreaB_Ramp_Steep_60deg`: Exceeds `maxSlopeDegrees`; character cannot climb and slides down under gravity.
- `AreaB_Stair_Step1..3` (`0.20m`–`0.25m` risers): Within `stepOffsetMeters` (`0.35m`); character steps up smoothly.
- `AreaB_Stair_TallBlock_055m`: Exceeds `stepOffsetMeters`; blocks horizontal walk-up and requires jumping.
- `AreaB_RaisedPlatform` & `AreaB_SlopedPlatform`: Raised landing and lateral slope tests.

### Area C: Collision & Navigation Tests (`x = 10 .. 16`)
- `AreaC_Wall_East`, `AreaC_CorridorWall_Left/Right` (`1.6m` corridor), `AreaC_Pillar_1/2`, `AreaC_LowObstacle_025m`.
- Validates wall sliding, zero penetration, and step-over on `0.25m` low obstacles.

### Area D: Physics Objects (`x = -10 .. -14`)
- `AreaD_DynamicBox_Light_10kg`, `AreaD_DynamicBox_Medium_50kg`, `AreaD_StaticReferenceBlock`.
- Validates pushing dynamic bodies via contact impulse vs. stopping against static blocks.

### Area E: Precision Movement Area (`z = 11 .. 15`)
- `AreaE_StepPlatform_1/2`, `AreaE_DropLedge_And_Landing`.
- Validates jumping across gaps, edge falling (`isGrounded` transitions to `false`), and landing recovery.

### Area F: Camera Test Area (`x = -11 .. -15, z = 13 .. 16`)
- `AreaF_CameraTest_TallTower`, `AreaF_CameraTest_Wall`.
- Validates third-person camera raycast collision pull-in when geometry sits between the character pivot and camera.

## Known Unsupported Behavior (Deferred to Later Milestones)

- GPU vertex skinning and glTF `skins`/`animations` playback in `SceneRenderer` (the humanoid model renders as a static multi-submesh glTF mesh following the physics capsule; `UAL1_Standard.glb` is staged for future renderer skinning support).
- Lua gameplay scripting hooks on character state (Milestone 12D).
