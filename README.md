# Hikari-Machine

An experimental C++20 3D game engine targeting fast-paced action games, architected around a **Two-State Conductor over a Shared World**.

Repository: [https://github.com/zesakain/hikari-machine](https://github.com/zesakain/hikari-machine)  
License: [Apache 2.0](LICENSE)
Discord: https://discord.gg/b2rhWTj5FE
---

## 1. Project Overview

Hikari-Machine is an experimental, mode-based 3D game engine under active development. Its defining architectural concept is the **Two-State Conductor operating over a single Shared World**:

- **Single Shared World:** All entities, components, and physics states reside in a unified Flecs ECS world. Switching gameplay modes does not require duplicating, reconstructing, or serializing the world state. Continuous simulation systems operate independently of gameplay mode transitions.
- **Two Time States (A and B):** The Conductor maintains exactly two alternating execution states (e.g., fast-paced combat and tactical precision). Exactly one state is active and authoritative at any given instant.
- **Asynchronous Standby Preparation:** The non-active state incrementally prepares its entry data, such as camera parameters, target queries, UI layouts, and resource requirements, within a configurable per-frame time budget.
- **Validated State Transitions:** Before switching roles, the Conductor validates the prepared state. Successful transitions commit the prepared state and support controlled slow-motion dilation during the transition curve. Failed validation preserves the currently active state.

> **Note on Project Scope:** Hikari-Machine is an experimental engine and architecture testbed. It is not a commercial, production-ready replacement for mature engines like Unreal Engine or Unity.

---

## 2. Current Status & Verified Features

The engine contains functional implementations across core systems, rendering, physics, scripting, UI, and editor tooling:

- **Core Primitives & Memory**:
  - Exception-free error handling with `Result<T, Error>`.
  - Custom allocators (Linear, Arena, Pool) and generational slot maps (`SlotMap`).
  - Lock-free work-stealing `JobSystem` with job dependencies.
  - Ring-buffer logger and monotonic high-resolution clock domains (`WorldClock`, `ActionClock`, `UiClock`).
- **Platform & Hardware Abstraction**:
  - SDL3 platform backend for windowing, OS event dispatch, and input handling.
  - Abstract RHI (`RhiDevice`, `RhiCommandList`, buffers, textures, samplers, pipelines, shaders, descriptor sets).
  - Headless `NullDevice` for offscreen simulation testing and CI without a GPU.
  - Vulkan 1.3 dynamic rendering backend (`VulkanDevice`, `VulkanCommandList` with multi-slot submission tracking and deferred staging/resource reclamation, dedicated `VulkanMemoryManager`).
  - DAG-based `RenderGraph` scheduler with topological ordering and automated barrier placement.
- **Rendering & Post-Processing**:
  - `SceneRenderer` with Cook-Torrance PBR shading, GPU texture/sampler descriptor sets, mipmapping, anisotropic filtering, and bounded texture residency management.
  - Image-Based Lighting (`EnvironmentMap`, `IrradianceProbe`) and directional Cascaded Shadow Maps (`ShadowCascade` with Hard, 3x3, and 5x5 PCF kernels and static shadow caster caching).
  - Screen-space passes including GTAO, SSGI, SSR, and bilateral/temporal filtering.
  - Anti-aliasing modes: `None`, `FXAA`, `TAA` (Halton jitter sequences, motion-vector reprojection, variance clipping, and sharpening), `MSAA2x`, and `MSAA4x`.
  - HDR post-processing pipeline with bloom, depth of field, motion blur, tone mapping (`AgX`, `AcesFitted`, `FilmicReinhard`, `Neutral` / Khronos PBR Neutral, `Bypass`), debug visualization modes, and serialized `GraphicsSettings`.
- **Simulation & Framework**:
  - Unified simulation `World` encapsulating Flecs ECS with system group gating (`Continuous`, `StateA`, `StateB`) and parent-child entity hierarchy with transform propagation (`hierarchy_systems`).
  - Two-State `Conductor` runtime coordinating active and standby `Mode` lifecycles with transition time dilation.
  - Jolt Physics integration (`PhysicsWorld`) with fixed-timestep simulation and bidirectional ECS component synchronization (`PhysicsTransformComponent`, `RigidBodyComponent`, `ColliderComponent`).
  - Skeletal animation evaluation system (`Skeleton`, `AnimationClip`, `SkeletalEvaluator`) evaluating joint matrices on CPU.
  - Sandboxed Lua 5.4 scripting engine with bounded memory allocations, instruction step limits, and ECS bindings.
  - RmlUi 6.0 runtime UI system with custom RHI render interface and unscaled real-time clock domain.
  - `RuntimeSession` orchestrating simulation lifecycles with an isolated, in-memory `RuntimeWorld` cloned from the authoring `EditWorld`.
  - `AsyncSceneLoader`: scheduler-driven, budget-aware multi-stage scene loading over `JobSystem` with worker-isolated mesh staging, bounded texture decode backpressure, cancellation safety, and per-phase telemetry.
  - Native `.hikari` versioned JSON scene serialization (`SceneSerializer`), including embedded graphics, lighting, and shadow settings.
- **3D Asset Pipeline**:
  - glTF 2.0 (`.gltf`) and Binary glTF (`.glb`) importers (`GltfImporter`, `GltfSceneImporter`) via vendored `cgltf` (v1.14), supporting scene hierarchy, PBR metallic-roughness/extensions, and embedded or external textures decoded via `stb`.
  - Wavefront OBJ (`.obj`) asset importer (`ObjImporter`).
  - Central `AssetManager` with 64-bit `AssetId` hashing, `SamplerCache`, and GPU mesh/texture upload deduplication.
- **Developer Visual Editor & IDE** (`HIKARI_BUILD_EDITOR=ON`):
  - Dear ImGui docking workspace with layout persistence (`hikari_editor_layout.ini`) and persisted `EditorPreferences`.
  - Centralized UI design tokens and theme palettes (Hikari Dark, Midnight, Light) with live runtime switching.
  - Viewport GPU scene rendering: Vulkan 1.3 offscreen dynamic rendering target sampled directly by Dear ImGui (zero CPU readback) with debug view mode switching.
  - 6-DOF fly camera navigation (WASD + QE + Shift boost) and orbit camera mode with mouse look and pitch clamping.
  - Interactive viewport tools: 3D perspective ground grid, orientation tripod, ray-picking entity selection, 3D transform gizmos (Translate, Rotate, Scale), and asset drag-and-drop spawning.
  - Modular editor panels: Scene Hierarchy (parent-child collapsible tree nodes, entity creation, inline rename, duplication, deletion, component filters), Entity Inspector (component reflection and multi-selection batch editing), Content Browser, Settings Panel (graphics quality & editor preferences), Conductor controls, Performance Metrics, Log Console, and external script editor integration.

---

## 3. Technology Stack

| Subsystem | Technology / Library | Actual Status in Codebase |
| :--- | :--- | :--- |
| **Language** | C++20 (`-fno-exceptions`, `-fno-rtti` on engine targets) | Implemented & enforced globally |
| **Build System** | CMake (>= 3.25) + Ninja | Implemented with 6 presets |
| **Window & Input** | SDL3 | Integrated |
| **ECS** | Flecs (v3.2.11 vendored C source) | Integrated with system group gating & hierarchy |
| **Physics** | Jolt Physics (`v5.6.0` via FetchContent) | Integrated with fixed-timestep accumulator |
| **Graphics API** | Vulkan 1.3 (Dynamic Rendering, Synchronization2) / Null RHI | Integrated |
| **GPU Memory** | Dedicated `VulkanMemoryManager` | Implemented (custom sub-allocation) |
| **Render Graph** | Custom DAG pass scheduler | Implemented |
| **Asset Import** | `cgltf` (v1.14), `stb` image decoder, custom OBJ loader | Integrated into `AssetManager` & importers |
| **File Dialogs** | `tinyfiledialogs` | Integrated for native asset import picker |
| **Scripting** | Lua 5.4 (Sandboxed C API with memory bounds) | Integrated (direct C API used; `sol2` omitted due to `-fno-exceptions`) |
| **Game UI** | RmlUi 6.0 (via FetchContent) | Integrated with custom RHI render interface |
| **Editor UI** | Dear ImGui `v1.91.8-docking` (via FetchContent) | Integrated (`HIKARI_BUILD_EDITOR=ON`) |
| **Shaders** | GLSL sources (`assets/shaders/`) -> embedded SPIR-V | Compiled & embedded (`mesh_shader_spirv.h`) |
| **Audio** | N/A | Not implemented (`engine/audio/` is a placeholder) |

---

## 4. Current Limitations & Incomplete Areas

- **Skeletal Animation GPU Skinning**: Joint matrices and animation curves are evaluated on the CPU via `SkeletalEvaluator`; GPU vertex skinning shaders are not yet connected.
- **Audio**: `engine/audio/` is a directory placeholder; audio mixing and spatial playback are not implemented.
- **Gameplay Slice & Data**: `game/` and `data/` are directory placeholders; benchmark scenes and sample scripts reside under `assets/`.
- **Host Vulkan Validation Layers**: Vulkan 1.3 validation requires `VK_LAYER_KHRONOS_validation` to be installed on the host system (not bundled in the repository).

---

## 5. Repository Layout

```
/
├── CMakeLists.txt              # Root build configuration & CPack packaging
├── CMakePresets.json           # Presets: debug, release, profile, editor, asan, tsan
├── AGENTS.md                   # Authoritative engineering guidelines and rules
├── engine/                     # Core engine subsystems (never depends on game/editor)
│   ├── core/                   # Allocators, job system, math, logging, handles, Result
│   ├── platform/               # SDL3 windowing, events, input snapshotting
│   ├── rhi/                    # Abstract RHI, Null backend, Vulkan 1.3 dynamic backend
│   ├── render/                 # RenderGraph, SceneRenderer, PBR/IBL/CSM/GI/TAA/PostProcess, AssetManager, importers
│   ├── world/                  # Shared World, Flecs ECS integration, hierarchy propagation, system gating
│   ├── physics/                # Jolt Physics integration, fixed-step accumulator
│   ├── anim/                   # Skeletal animation, CPU evaluation, cancel windows
│   ├── script/                 # Sandboxed Lua 5.4 scripting engine
│   ├── ui/                     # RmlUi runtime UI system & RHI render interface
│   ├── framework/              # Two-State Conductor, Mode, AsyncSceneLoader, SceneSerializer, RuntimeSession
│   └── audio/                  # Audio subsystem (placeholder directory)
├── editor/                     # Dear ImGui visual editor & IDE (HIKARI_BUILD_EDITOR)
│   ├── include/editor/         # Editor public interfaces, theme tokens, preferences, selection service
│   └── src/                    # EditorEngine, WorkspaceManager, panels (Viewport, Hierarchy, Inspector, Settings, etc.)
├── assets/                     # Models (.glb, .obj), benchmark scenes (.hikari), Lua scripts, GLSL shaders
├── data/                       # Runtime game data (placeholder directory)
├── docs/                       # Architecture documentation, ADRs, GPU notes, error catalog
├── src/                        # Standalone application entrypoint (main.cpp)
├── tests/                      # Automated unit, headless, golden, and benchmark test suites
├── tools/                      # Benchmark harnesses, scene generators, shader & asset utilities
└── game/                       # Standalone game slice (placeholder directory)
```

---

## 6. How to Build and Run

### Prerequisites

- **Compiler**: C++20 compliant compiler:
  - GCC >= 12 (Linux)
  - Clang >= 15 (Linux)
- **Build Tools**: CMake >= 3.25, Ninja.
- **System Libraries (Linux)**:
  - Vulkan SDK / loader and driver with Vulkan 1.3 support.
  - SDL3 development libraries.
  - FreeType development libraries (`libfreetype6-dev` / `freetype2`).
  - Platform libraries: `libasound2-dev`, `libpulse-dev`, `libx11-dev`, `libwayland-dev`.

### Build & Test Commands

```bash
# 1. Configure and build the visual editor
cmake --preset editor
cmake --build --preset editor

# 2. Run the test suite
ctest --preset editor --output-on-failure
```

### Available Presets

All presets inherit `HIKARI_WARNINGS_AS_ERRORS=ON` and use the Ninja generator:

| Preset | Build Type | Key Cache Variables | Primary Use |
| :--- | :--- | :--- | :--- |
| `editor` | `Debug` | `HIKARI_BUILD_EDITOR=ON` | Visual Editor & IDE development |
| `debug` | `Debug` | Default (`HIKARI_BUILD_EDITOR=OFF`) | Local engine development with assertions |
| `release` | `Release` | Optimizations enabled | Performance testing and distribution |
| `profile` | `RelWithDebInfo` | Release optimizations with debug symbols | Profiling and trace analysis |
| `asan` | `Debug` | `HIKARI_SANITIZER=address,undefined` | Memory safety, UB, and leak verification |
| `tsan` | `Debug` | `HIKARI_SANITIZER=thread` | Concurrency and data-race verification |

### Running the Executable

```bash
# Launch the visual editor
./build/editor/bin/hikari

# Launch the editor directly on a scene or glTF model
./build/editor/bin/hikari --scene assets/benchmark-scenes/Sponza_Benchmark.hikari

# Run headless simulation loop for 60 frames (Null RHI, no window)
./build/release/bin/hikari --headless --frames 60

# Display CLI options and version
./build/release/bin/hikari --help
```

---

## 7. Testing and Validation

CTest registers **58** core test suites in non-editor presets (`debug`, `release`, `profile`, `asan`, `tsan`) and **63** total test suites when `HIKARI_BUILD_EDITOR=ON` (`editor` preset):

- **Core Primitives**: `smoke_test`, `result_test`, `assert_test`, `linear_allocator_test`, `arena_allocator_test`, `pool_allocator_test`, `generational_handle_test`, `time_test`, `log_test`, `slot_map_test`, `id_test`, `event_test`, `json_test`, `job_system_test`, `math_test`.
- **Platform & RHI**: `platform_test`, `null_rhi_test`, `vulkan_rhi_test` (validates physical GPU selection, offscreen/swapchain dynamic rendering, and pipeline compatibility).
- **World, Simulation & Framework**: `world_test`, `hierarchy_test`, `transform_propagation_test`, `conductor_test`, `shared_world_conductor_test`, `clock_domain_test`, `scene_test`, `async_scene_loader_test`, `runtime_session_test`, `conductor_stress_test`.
- **Render, Lighting & Assets**: `render_graph_test`, `image_decoder_test`, `environment_map_test`, `irradiance_probe_test`, `mesh_test`, `asset_manager_test`, `glTF_importer_test`, `gltf_scene_importer_test`, `scene_renderer_test`, `textured_mesh_render_test`, `material_descriptor_test`, `sampler_and_texture_quality_test`, `gltf_material_extensions_test`, `advanced_pbr_pipeline_test`, `texture_residency_test`, `pbr_render_test`, `lighting_render_test`, `shadow_cascade_test`, `post_process_test`, `sponza_benchmark_test`, `graphics_settings_test`, `ibl_render_test`, `lighting_upgrade_test`, `gi_render_test`, `reflection_and_temporal_test`, `taa_test`.
- **Subsystems**: `physics_test`, `anim_test`, `script_test`, `ui_test`.
- **Editor Tooling** (`editor` preset): `editor_test`, `content_browser_test`, `script_editor_test`, `editor_preferences_test`, `graphics_settings_apply_test`.

### Validation Baseline
- Real-machine Vulkan testing is verified on Linux under Wayland/Xwayland on NVIDIA hardware (`NVIDIA P106-100`).
- Headless execution runs in CI on x86_64 using `NullDevice` without GPU dependencies.
