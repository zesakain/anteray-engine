# ANTERAY ENGINE — Rebranding Discovery, Identity Audit & Migration Blueprint (Phase 0)

**Status:** Phase 0 (Read-Only Discovery & Planning)  
**Target Product Identity:** ANTERAY ENGINE (`ANTERAY`, `ANTERAY Hub`, `ANTERAY Editor`, `ANTERAY Launcher`)  
**Source Repository:** `hikari-machine` (branch `main` at commit `f49241a`)  
**Engineering Authority:** Root [`AGENTS.md`](../../AGENTS.md) and accepted ADRs (`docs/adr/0001`–`0020`)

---

## 1. Executive Summary

This document defines the discovery audit and phased migration blueprint to evolve **Hikari Machine** into **ANTERAY ENGINE** while preserving all accepted architectural decisions:

- C++20 with `-fno-exceptions -fno-rtti` in engine targets (`CMakeLists.txt`, `AGENTS.md` §2.6).
- Layered module boundaries (`engine/`, `editor/`, `hub/`, `game/`, `tools/`, `tests/`).
- Flecs ECS (`engine/world/`), Jolt Physics (`engine/physics/`), SDL3 (`engine/platform/`), Vulkan 1.3 behind `eng::rhi` (`engine/rhi/`), and Lua 5.4 C API behind the opaque `ScriptEngine` PIMPL (`engine/script/`, no `sol2` per ADR-0019).
- Two-State `Conductor` over one Shared `World` (`engine/framework/`).

### Key Discovery Findings

1. **C++ Namespaces Are Already Brand-Neutral (`eng::`):** A repository-wide search confirms **zero** `hikari::` C++ namespaces exist. All C++ code uses `eng::` (`eng::core::`, `eng::rhi::`, `eng::render::`, `eng::world::`, `eng::framework::`, `eng::physics::`, `eng::anim::`, `eng::script::`, `eng::ui::`, `eng::platform::`, `eng::editor::`, `eng::hub::`). No C++ namespace rename is required unless `eng::` itself is ever reconsidered (not recommended).
2. **Primary Coupling Is in CMake Targets, Binaries, User-Facing Branding, and Persistent Formats:**
   - **Build System:** `project(hikari-machine ...)`, options `HIKARI_*`, aliases `hikari::*`, executables `hikari` (`src/main.cpp`) and `hikari_hub` (`hub/CMakeLists.txt`), and CPack package name `hikari`.
   - **Persistent Formats:** `.hikari` (scenes), `.hikari_prefab` (prefabs), `.hikari-project` (Hub project manifests), `hikari_editor_layout.ini` (ImGui layout), and OS user-config directories (`hikari` / `Hikari` for `editor_preferences.json` and `hub_recent_projects.json`).
   - **Hub-to-Engine Discovery:** `eng::hub::EngineLauncher::discover()` hardcodes `hikari` / `hikari.exe` lookup across `build/{editor-release,editor,release-editor,profile,release,debug}/bin` and `PATH`.
3. **Total Old-Identity Footprint:**
   - **214 files** containing **1,191** case-insensitive `hikari` line matches (213 non-hidden files / 1,189 matches + `.github/workflows/ci.yml` with 2 matches), plus **20** files/paths whose filenames contain `hikari`.
   - **~282,704 LOC** across C++/Lua/CMake sources (`engine`, `editor`, `hub`, `game`, `src`, `tools`, `tests`).

---

## 2. New Product Identity

| Role | Old Identity | New Official Identity |
|---|---|---|
| Full Product Name | Hikari Machine | **ANTERAY ENGINE** |
| Short Name | Hikari | **ANTERAY** |
| Hub Application | Hikari Machine Hub | **ANTERAY Hub** |
| Editor Application | Hikari Editor / Hikari Machine - Studio Editor | **ANTERAY Editor** |
| Launcher / Engine CLI | `hikari` | **ANTERAY Launcher** (`anteray`) |

Established architectural terms (**World**, **Mode**, **Conductor**, **Character Framework**, **Blueprint**, **RHI**) remain unchanged.

---

## 3. Current Repository Identity Inventory

### 3.1 Baseline Git State (Pre-Audit)

- **Branch:** `main` at `f49241a` (`origin/main` up to date, 0 unpushed commits).
- **Staged changes:** None.
- **Unstaged modifications:** `assets/benchmark-scenes/Bistro_Benchmark.hikari` (user-owned, preserved).
- **Untracked paths:** `assets/Lua/`, `projects/`, `projects1/`, `projects5/`, `projects6/`, `projects7/`, `projects8/`, `projectsgg/` (user-owned, preserved).

### 3.2 Search Methodology & Counts

1. **LOC Count:** `find engine editor hub game src tools tests -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.lua' -o -name '*.cmake' -o -name 'CMakeLists.txt' \) | xargs wc -l` → **282,704 total lines**.
2. **Content Match Count:** `rg -i -c "hikari"` excluding `.git/`, `build*/`, `out*/`, `CMakeFiles/`, `Testing/`, `projects*/`, `test-assets/`:
   - `docs/`: 69 files, 317 line matches
   - `tests/`: 34 files, 296 line matches
   - `engine/`: 42 files, 143 line matches
   - `editor/`: 20 files, 127 line matches
   - `hub/`: 15 files, 113 line matches
   - `(root)`: 5 files, 88 line matches
   - `tools/`: 15 files, 69 line matches
   - `src/`: 1 file, 24 line matches
   - `assets/`: 12 files, 12 line matches
   - `.github/workflows/ci.yml`: 1 file, 2 line matches
   - **Total:** **214 files**, **1,191 line matches**.
3. **Filename Match Count:** **20 files** with `hikari` in their filename (9 `.hikari` scenes, 1 `.hikari_prefab`, 1 `hikari_logo.jpg`, 8 `docs/architecture/hikari_*.md`, 1 `docs/roadmap/hikari_*.md`, 1 `hikari_editor_layout.ini`).

---

## 4. Categorized Reference Inventory

| Classification | File Path(s) | Symbol / Key / Identifier | Current Usage | Dependency Impact | Rename Recommendation | Compatibility Risk |
|---|---|---|---|---|---|---|
| **Branding** | `src/main.cpp` | `"Hikari Machine"`, `"Hikari Machine 3D Engine"`, `"Hikari Diagnostics HUD"`, `"Deep Hikari Blue"` | Window title, CLI `--help`/`--version`, startup log, HUD HTML title | User-facing only | Rename to ANTERAY ENGINE / ANTERAY | **Low** |
| **Branding** | `engine/platform/include/platform/window.h` | `WindowDesc::title{"Hikari"}` | Default OS window title | Default struct member | Rename to `"ANTERAY"` | **Low** |
| **Branding** | `engine/rhi/src/vulkan/vulkan_instance.cpp` | `.pApplicationName = "Hikari Machine"`, `.pEngineName = "Hikari"` | `VkApplicationInfo` metadata | GPU driver/profiler telemetry only | Rename to `"ANTERAY ENGINE"` / `"ANTERAY"` | **Low** |
| **Branding** | `hub/src/main.cpp`, `hub/src/hub_app.cpp` | `"Hikari Machine Hub"`, `"HIKARI MACHINE HUB"`, `"MyHikariProject"`, `"HikariMachineHubRoot"` | Hub window title, ImGui root ID, welcome copy, default project name | Hub UI & ImGui ini ID | Rename UI text & `"MyAnterayProject"`; coordinate ImGui root ID | **Low** |
| **Branding** | `editor/src/workspace_manager.cpp`, `editor/src/editor_engine.cpp`, `editor/include/editor/editor_theme.h`, `editor/src/editor_theme.cpp`, `editor/src/panels/settings_panel.cpp` | `"HIKARI ENGINE"`, `"About Hikari Editor..."`, `"Hikari Machine - Studio Editor"`, `"Hikari Dark"`, `"Hikari Midnight"`, `"Hikari Light"`, `ThemeColors::hikariDark/Midnight/Light()`, `drawHikariLoadingScreen`, `HikariRootDockSpace` | Editor loading modal, About dialog, theme names & factories, ImGui dockspace ID | `editor_preferences.json` stores `themeName` (`"Hikari Dark"`)! | Rename UI labels; **keep legacy theme-name parser aliases** (`"Hikari Dark"` → `"ANTERAY Dark"`) | **Medium** |
| **Branding** | `assets/textures/branding/hikari_logo.jpg` | `hikari_logo.jpg` | Referenced by `src/main.cpp`, `editor/src/editor_engine.cpp`, `hub/src/main.cpp`, `hub/src/hub_app.cpp` | Visual asset | Replace with ANTERAY logo asset in Phase 1 | **Low** |
| **Branding** | `README.md`, `CHANGELOG.md`, `tools/*.cpp`, `tools/download_benchmark_assets.py` | `"Hikari-Machine"`, `"HikariBenchmarkDownloader/1.0"`, CLI banners | Docs, benchmark console banners, HTTP User-Agent | Docs & tool output | Update `README.md` and tool banners; keep `CHANGELOG.md` historical entries intact | **Low** |
| **Technical Internal** | `engine/ui/src/rmlui_system_backend.{h,cpp}`, `rmlui_render_backend.{h,cpp}`, `ui_system.cpp` | `HikariSystemInterface`, `HikariRenderInterface`, `"hikari_main_context"` | Private RmlUi backend classes (not in public `engine/ui/include/`) | Internal to `engine/ui` | Rename to `AnteraySystemInterface`, `AnterayRenderInterface`, `"anteray_main_context"` | **Low** |
| **Technical Internal** | `engine/platform/src/sdl_platform.cpp`, `sdl_window.cpp` | `kWindowPtrProp = "Hikari_WindowPtr"` | Internal SDL3 window property key | Private to `engine/platform` | Rename to `"Anteray_WindowPtr"` | **Low** |
| **Technical Internal** | `engine/script/src/script_engine.cpp` | `kVec3Meta = "Hikari.Vec3"`, `kEntityMeta = "Hikari.Entity"`, `kEngineRegistryKey = "Hikari.ScriptEngine"` | Lua registry/metatable keys inside `ScriptEngine` PIMPL | Private to `script_engine.cpp` (unless Lua scripts inspect `getmetatable`) | Rename to `"Anteray.*"` in Phase 3 after checking script usage | **Low** |
| **Technical Internal** | `editor/src/external_editor_service.cpp` | `"hikari_editor_test.lua"` | Temp file for external editor smoke test | Ephemeral temp file | Rename to `"anteray_editor_test.lua"` | **Low** |
| **Public API** | `hub/include/hub/project_metadata.h` | `kProjectFileExtension = ".hikari-project"` | Public Hub manifest extension constant | Coupled to Hub, Validator, Tests | Migrate via dual-read / versioned convention (see §6) | **High** |
| **Public API** | `engine/framework/include/framework/third_person_character_template.h` | `kDefaultCharacterPrefabPath = "assets/prefabs/third_person_character.hikari_prefab"` | Public default prefab path constant | Coupled to Hub templates, samples, tests | Update alongside prefab format phase | **Medium** |
| **Public API** | `engine/framework/include/framework/scene.h` | `SceneMetadata::generator{"Hikari Machine 0.1.0"}` | Default scene generator string | Serialized metadata | Update default to `"ANTERAY ENGINE 0.1.0"` | **Low** |
| **Public API** | `editor/include/editor/content_browser_controller.h` | `ContentFileTemplate::HikariScene` | Public editor enum value | Coupled to `content_browser_panel.cpp` & `content_browser_test.cpp` | Rename to `AnterayScene` in Phase 3 | **Low** |
| **Public API** | `editor/include/editor/editor_theme.h` | `ThemeColors::hikariDark()`, `hikariMidnight()`, `hikariLight()` | Public static constexpr theme factories | Coupled to `editor_theme.cpp` & `editor_test.cpp` | Rename with deprecated inline aliases during transition if needed | **Low** |
| **Build System** | `CMakeLists.txt`, `CMakePresets.json`, `engine/*/CMakeLists.txt`, `editor/CMakeLists.txt`, `hub/CMakeLists.txt`, `tests/CMakeLists.txt`, `.github/workflows/{ci,release}.yml` | `project(hikari-machine)`, `HIKARI_*` options/defines, `hikari::*` aliases, `hikari` & `hikari_hub` executables, `CPACK_PACKAGE_NAME "hikari"`, `HIKARI_SAMPLE_SCENE_PATH` | Build graph, CI, packaging, `EDITOR` compile defs | High cross-file coupling across all `CMakeLists.txt` & Hub `EngineLauncher` | Coordinated Phase 2 migration (with optional compatibility aliases) | **High** |
| **Persistent Format** | `engine/framework/src/scene_serializer.cpp`, `async_scene_loader.cpp`, `assets/scenes/*.hikari`, `assets/benchmark-scenes/*.hikari` | `.hikari` scene extension & `"generator"` field | Native JSON scene files (`formatVersion: 1`) | User scenes, Hub templates, benchmark tools, tests | Support reading `.hikari` permanently/long-term; introduce `.anteray` (see §6) | **High** |
| **Persistent Format** | `engine/framework/src/prefab.cpp`, `assets/prefabs/*.hikari_prefab` | `.hikari_prefab` prefab extension | Native JSON prefab descriptors (`formatVersion: 1`) | User prefabs, TPP template, Hub sample catalog | Support reading `.hikari_prefab`; introduce `.anteray_prefab` (see §6) | **High** |
| **Persistent Format** | `hub/include/hub/project_metadata.h`, `hub/src/project_metadata.cpp`, `hub/src/template_registry.cpp` | `.hikari-project` manifest extension & `"generator": "Hikari Machine Hub 0.1.0"` | Hub project root manifest (`formatVersion: 1`) | Existing user projects in `projects*/`, Hub validator, Launcher | Support reading `.hikari-project`; introduce `.anteray-project` (see §6) | **High** |
| **Compatibility** | `editor/src/editor_preferences_serializer.cpp`, `hub/src/recent_projects.cpp`, `editor/src/workspace_manager.cpp` | `~/.config/hikari/editor_preferences.json`, `~/.config/hikari/hub_recent_projects.json`, `hikari_editor_layout.ini` | User-local OS config & ImGui layout persistence | Existing user preferences & recent project lists | Read from `anteray/` first, fallback-migrate from `hikari/` | **Medium** |
| **Historical** | `docs/adr/0001`–`0020`, `docs/architecture/*.md`, `docs/roadmap/*.md`, `docs/errors.md`, `CHANGELOG.md` | `Hikari`, `hikari-machine`, `sol2` in ADR-0014 | Architectural history & milestone certifications | None at runtime | **Do not rewrite history.** Add migration banner/notes where needed | **Low** |
| **Third-party** | `engine/script/src/lua/`, `engine/world/src/flecs/`, `engine/render/src/cgltf/`, `engine/render/src/stb/`, ImGui, Jolt, SDL3, RmlUi | N/A | Vendored/fetched dependencies | Not owned by project | **Do not modify.** | **Low** |
| **Test Fixture** | `tests/fixtures/gltf/*.gltf`, `tests/unit/editor/editor_test.cpp`, `tests/unit/hub/hub_test.cpp`, `tests/unit/render/gltf_importer_test.cpp` | `"generator": "Hikari Test Suite"`, `hikari_test_*`, `testHikariSerializationRoundTrip` | Unit/integration test fixtures and temp dir prefixes | Test suite only | Update test names/temp prefixes in Phase 3/6; keep legacy format fixture tests for compatibility | **Low** |

---

## 5. Technical Rename Map

| Category | Current | Proposed ANTERAY |
|---|---|---|
| CMake Project | `hikari-machine` | `anteray-engine` |
| CMake Options / Defines | `HIKARI_WARNINGS_AS_ERRORS`, `HIKARI_BUILD_TESTS`, `HIKARI_BUILD_EDITOR`, `HIKARI_BUILD_HUB`, `HIKARI_SANITIZER` | `ANTERAY_WARNINGS_AS_ERRORS`, `ANTERAY_BUILD_TESTS`, `ANTERAY_BUILD_EDITOR`, `ANTERAY_BUILD_HUB`, `ANTERAY_SANITIZER` |
| CMake Aliases | `hikari::compiler_flags`, `hikari::engine_flags`, `hikari::engine_base`, `hikari::engine_<mod>`, `hikari::editor_lib`, `hikari::hub_core` | `anteray::compiler_flags`, `anteray::engine_flags`, `anteray::engine_base`, `anteray::engine_<mod>`, `anteray::editor_lib`, `anteray::hub_core` |
| Executables | `hikari` (Engine/Editor), `hikari_hub` (Hub) | `anteray` (Engine/Editor/Launcher), `anteray_hub` (Hub) |
| CPack / Install Share Dir | `CPACK_PACKAGE_NAME "hikari"`, `share/hikari/` | `CPACK_PACKAGE_NAME "anteray"`, `share/anteray/` |
| C++ Namespace | `eng::` (`eng::core::`, `eng::rhi::`, etc.) | **Keep `eng::`** (already brand-neutral; avoids massive churn) |
| Project Manifest Ext | `.hikari-project` | `.anteray-project` (with `.hikari-project` compatibility reader) |
| Scene Ext | `.hikari` | `.anteray` (with `.hikari` compatibility reader) |
| Prefab Ext | `.hikari_prefab` | `.anteray_prefab` (with `.hikari_prefab` compatibility reader) |
| Editor Layout / Config | `hikari_editor_layout.ini`, `~/.config/hikari/` | `anteray_editor_layout.ini`, `~/.config/anteray/` (with fallback import) |
| Editor Themes | `"Hikari Dark"`, `"Hikari Midnight"`, `"Hikari Light"` | `"ANTERAY Dark"`, `"ANTERAY Midnight"`, `"ANTERAY Light"` |
| Branding Asset | `assets/textures/branding/hikari_logo.jpg` | `assets/textures/branding/anteray_logo.jpg` (or `.png`) |

---

## 6. Project-Format Compatibility Recommendation

### Options Evaluated

- **Option A — Keep existing extensions permanently (`.hikari`, `.hikari_prefab`, `.hikari-project`):** Zero breakage, but leaves old branding permanently visible in every user-created file and project directory.
- **Option B — Introduce new extensions while continuing to read old extensions (Recommended):**
  - New projects/scenes/prefabs created by ANTERAY Hub & Editor default to `.anteray-project`, `.anteray`, and `.anteray_prefab`.
  - `findProjectManifestInDirectory()`, `AsyncSceneLoader`, `SceneSerializer`, `PrefabLoader`, and `ContentBrowserController` recognize **both** new (`.anteray*`) and legacy (`.hikari*`) extensions.
  - Tradeoff: Small dual-extension check in file discovery/loaders; completely preserves existing `projects*/`, `assets/scenes/`, `assets/benchmark-scenes/`, and `assets/prefabs/` without silent mutation.
- **Option C — Versioned migration system with explicit conversion tool:**
  - Can be layered **on top of Option B** as an explicit, user-invoked Hub/CLI migration action (`Upgrade Legacy Hikari Project` / `--migrate-project`) that copies/renames `.hikari-project`, `.hikari`, and `.hikari_prefab` and rewrites `startupScene` / prefab references atomically.

**Recommendation:** Adopt **Option B immediately in Phase 4**, paired with an **explicit, opt-in conversion tool (Option C)** in Phase 6 so legacy projects work out of the box and can be upgraded cleanly when the user chooses.

---

## 7. Namespace Migration Recommendation

- **Finding:** `eng::` is used uniformly across `engine/`, `editor/`, `hub/`, `src/`, `tests/`, and `tools/`. There is **no** `hikari::` namespace in C++.
- **Recommendation:** **Keep `eng::`** as the C++ namespace. Renaming `eng::` to `anteray::` is unnecessary for the rebrand, would touch ~282k LOC with high merge-conflict risk, and provides no user-facing or architectural benefit.
- Only internal C++ symbol names containing `Hikari` (`HikariSystemInterface`, `HikariRenderInterface`, `ThemeColors::hikariDark`, `ContentFileTemplate::HikariScene`, `drawHikariLoadingScreen`) need renaming in Phase 3.

---

## 8. Build & Executable Migration Recommendation

1. Rename CMake project (`anteray-engine`), options (`ANTERAY_*`), targets/aliases (`anteray::*`), and executables (`anteray`, `anteray_hub`) atomically in Phase 2 across `CMakeLists.txt`, `CMakePresets.json`, `engine/*/CMakeLists.txt`, `editor/CMakeLists.txt`, `hub/CMakeLists.txt`, `tests/CMakeLists.txt`, and `.github/workflows/{ci,release}.yml`.
2. Update `eng::hub::EngineLauncher::discover()` (`hub/src/engine_launcher.cpp`) to search for `anteray` / `anteray.exe` first, with an optional fallback to `hikari` / `hikari.exe` during transition if desired.
3. Update `CMAKE_INSTALL_DATADIR` (`share/anteray/assets`) and `CPack` metadata (`anteray`, `ANTERAY ENGINE`).
4. Temporary compatibility symlink/launcher (`hikari` → `anteray`) is **optional**; recommended only if external developer scripts invoke `build/*/bin/hikari` directly.

---

## 9. Risk Matrix

| Risk Area | Severity | Rationale & Mitigation |
|---|---|---|
| **Source Compatibility** | Low | No `hikari::` C++ namespace exists; only ~6 internal/editor symbols use `Hikari` in their name. |
| **ABI Compatibility** | Low | Engine is built from source via CMake; no external binary ABI contract exists. |
| **Project Compatibility** | **High** | Existing `.hikari-project` manifests in `projects*/` would fail Hub discovery if `.hikari-project` support is dropped. Mitigated by Option B dual-read. |
| **Asset / Scene / Prefab Compatibility** | **High** | `assets/scenes/*.hikari`, `assets/benchmark-scenes/*.hikari`, and `assets/prefabs/*.hikari_prefab` are referenced by `sample_catalog.cpp`, `template_registry.cpp`, `third_person_character_template.h`, `tests/CMakeLists.txt`, and `tools/*.cpp`. Mitigated by dual-extension support + staged path updates. |
| **Serialization Compatibility** | Medium | `editor_preferences.json` stores `themeName` (`"Hikari Dark"`). Mitigated by mapping legacy theme strings on load. |
| **CMake & Build Compatibility** | **High** | `hikari::*` aliases and `HIKARI_*` options span 16 `CMakeLists.txt` files, `CMakePresets.json`, and CI workflows. Mitigated by atomic Phase 2 commit series. |
| **Hub Project Discovery** | **High** | `EngineLauncher::discover()` hardcodes `hikari`/`hikari.exe`. Must be updated in the same phase as executable rename. |
| **Editor Startup** | Medium | `editor_engine.cpp` & `workspace_manager.cpp` load `hikari_logo.jpg` and `hikari_editor_layout.ini`. Missing logo logs a warning (non-fatal), layout resets cleanly. |
| **Test Fixtures** | Low | `tests/fixtures/gltf/*.gltf` and unit test temp prefixes (`hikari_*`) are self-contained. |
| **CI & Packaging** | Medium | `.github/workflows/ci.yml` invokes `./build/${preset}/bin/hikari --headless --frames 60` and uploads `hikari-linux-x86_64-release`. |
| **Developer Documentation** | Low | `AGENTS.md` is brand-neutral; `docs/adr/*` are historical records (retain original context + add migration notes). |
| **Third-Party Integration** | Low | Vendored/fetched dependencies (`flecs`, `Jolt`, `SDL3`, `Lua 5.4`, `RmlUi`, `ImGui`, `cgltf`, `stb`) have zero Hikari coupling. |

---

## 10. Phased Migration Plan

Each phase requires **explicit user approval** before implementation and must follow `AGENTS.md` §12 commit discipline (small, scoped, independently buildable commits).

### Phase 0: Identity Audit (Current)
- **Scope:** Read-only discovery, inventory, and migration blueprint (`docs/architecture/anteray_rebranding_migration_plan.md`).
- **Acceptance Criteria:** Git state preserved; zero production/asset changes; blueprint approved.

### Phase 1: Brand Identity
- **Scope:** User-visible strings, window titles, CLI `--help`/`--version`, About dialog, loading screen, Hub welcome/creation copy, Editor theme display names (`ANTERAY Dark`, `ANTERAY Midnight`, `ANTERAY Light` with legacy `Hikari *` compatibility), Vulkan `VkApplicationInfo`, and `README.md` product identity. **Keep existing `assets/textures/branding/hikari_logo.jpg` unchanged** (deferred).
- **Dependencies:** Phase 0 approval (confirmed).
- **Expected Files:** `src/main.cpp`, `engine/platform/include/platform/window.h`, `engine/rhi/src/vulkan/vulkan_instance.cpp`, `hub/src/main.cpp`, `hub/src/hub_app.cpp`, `hub/src/template_registry.cpp`, `editor/src/workspace_manager.cpp`, `editor/src/editor_theme.cpp`, `editor/src/panels/settings_panel.cpp`, `editor/include/editor/editor_preferences.h`, `README.md`, and regression tests in `tests/unit/editor/` and `tests/unit/hub/`.
- **Risks:** Theme name mismatch in existing `editor_preferences.json`.
- **Tests:** `editor_test`, `editor_preferences_test`, `hub_test`, headless `--headless --frames 60`.
- **Rollback:** Revert Phase 1 commits; no on-disk format changes.

### Phase 2: Build and Executable Identity
- **Scope:** Rename CMake project (`anteray-engine`), options (`ANTERAY_*`), targets/aliases (`anteray::*`), executables (`anteray`, `anteray_hub`), `EngineLauncher` binary lookup, `CPack` metadata, `CMakePresets.json`, and `.github/workflows/{ci,release}.yml`.
- **Dependencies:** Phase 1.
- **Expected Files:** `CMakeLists.txt`, `CMakePresets.json`, `engine/**/CMakeLists.txt`, `editor/CMakeLists.txt`, `hub/CMakeLists.txt`, `tests/CMakeLists.txt`, `hub/src/engine_launcher.cpp`, `src/main.cpp` (`ANTERAY_BUILD_EDITOR`), `.github/workflows/{ci,release}.yml`.
- **Risks:** CI or Hub failing to find `anteray` binary if any `CMakeLists.txt` or preset is missed.
- **Tests:** Full configure/build/ctest across `debug`, `release`, `editor`, `asan`, `tsan` presets; `hub_test` (`EngineLauncher`).
- **Rollback:** Revert Phase 2 commit series.

### Phase 3: Source Identity
- **Scope:** Rename internal/editor C++ identifiers (`HikariSystemInterface`, `HikariRenderInterface`, `"hikari_main_context"`, `Hikari_WindowPtr`, `Hikari.Vec3`/`Entity`/`ScriptEngine`, `ContentFileTemplate::HikariScene`, `ThemeColors::hikari*`, `drawHikariLoadingScreen`, `HikariRootDockSpace`, `hikari_editor_test.lua`) and update unit test identifiers.
- **Dependencies:** Phase 2.
- **Expected Files:** `engine/ui/src/*`, `engine/platform/src/*`, `engine/script/src/script_engine.cpp`, `editor/**`, `tests/unit/**`.
- **Risks:** `clang-tidy` / `-Werror` warnings; ImGui dockspace ID reset for existing users.
- **Tests:** `ui_test`, `platform_test`, `script_test`, `editor_test`, `content_browser_test`, `clang-format --dry-run --Werror`.
- **Rollback:** Revert Phase 3 commits.

### Phase 4: Project and Asset Format
- **Scope:** Introduce `.anteray-project`, `.anteray`, and `.anteray_prefab` as primary formats for new projects/scenes/prefabs while **preserving read/validation support** for `.hikari-project`, `.hikari`, and `.hikari_prefab`. Migrate OS config path (`~/.config/anteray/` with read-fallback from `~/.config/hikari/`) and layout file (`anteray_editor_layout.ini`).
- **Dependencies:** Phase 3 (and ADR if public API of `Scene`/`Prefab`/`ProjectMetadata` changes).
- **Expected Files:** `hub/include/hub/project_metadata.h`, `hub/src/{project_metadata,project_validator,template_registry,recent_projects,sample_catalog}.cpp`, `engine/framework/{include,src}/{scene,scene_serializer,async_scene_loader,prefab,third_person_character_template}.*`, `editor/src/{content_browser_controller,editor_preferences_serializer,workspace_manager}.cpp`, `editor/src/panels/content_browser_panel.cpp`.
- **Risks:** Breaking existing `projects*/` or `assets/benchmark-scenes/*.hikari` if legacy extensions are not kept readable.
- **Tests:** `hub_test`, `scene_test`, `async_scene_loader_test`, `prefab_and_character_test`, `third_person_character_template_test`, `content_browser_test`, `editor_preferences_test` (including explicit legacy `.hikari*` read tests).
- **Rollback:** Revert Phase 4 commits; dual-read ensures no user data is corrupted.

### Phase 5: Repository and Developer Experience
- **Scope:** Update `tools/*.cpp` banners, `tools/download_benchmark_assets.py`, `docs/architecture/` active docs (adding migration notes to historical docs/ADRs without rewriting history), and `CMakeUserPresets.json.example` if applicable. Evaluate (and defer unless requested) GitHub repo / root folder rename (`hikari-machine` → `anteray-engine`).
- **Dependencies:** Phase 4.
- **Tests:** Build all tools under `editor` preset; verify docs links.
- **Rollback:** Revert Phase 5 commits.

### Phase 6: Compatibility and Regression
- **Scope:** Optional explicit legacy project migration CLI/Hub action (`Upgrade .hikari-project → .anteray-project`), full regression & stress suites (`conductor_stress_test`, `smoke_test`), ASan/UBSan/TSan verification, and clean-checkout CI verification.
- **Dependencies:** Phases 1–5.
- **Acceptance Criteria:** All presets (`debug`, `release`, `profile`, `editor`, `asan`, `tsan`) build cleanly with `-Werror`, `clang-format` passes, all tests pass, and legacy `.hikari*` projects open and run without data loss.

---

## 11. Testing Strategy

- **Unit & Integration:** Run `ctest --preset <preset> --output-on-failure` after every phase.
- **Compatibility Regression:** Add dedicated tests verifying `.hikari-project`, `.hikari`, and `.hikari_prefab` continue to load alongside `.anteray-project`, `.anteray`, and `.anteray_prefab`, and that `"Hikari Dark"`/`"Hikari Midnight"`/`"Hikari Light"` preferences map cleanly.
- **Sanitizers & Formatting:** Validate `asan`, `tsan`, and `clang-format --dry-run --Werror` on every commit.
- **Headless Smoke:** Run `./build/<preset>/bin/anteray --headless --frames 60` (and legacy alias if provided).

---

## 12. Rollback Strategy

Because work is split into scoped, independently buildable phases and commits (`AGENTS.md` §12):
- Any phase can be reverted cleanly via `git revert` without affecting earlier phases.
- Keeping dual-format readers in Phase 4 ensures user projects are never destructively migrated in-place.

---

## 13. Confirmed Decisions & Deferred Work

### Confirmed Decisions (User-Approved)
1. **Product Identity:** Product name `ANTERAY ENGINE`, short brand `ANTERAY`, Hub `ANTERAY Hub`, Editor `ANTERAY Editor`.
2. **CMake & Executables (Phase 2):** CMake project `anteray-engine`, options/compile definitions `ANTERAY_*`, aliases `anteray::*`, engine executable `anteray`, hub executable `anteray_hub`. **No** permanent `hikari` executable alias.
3. **C++ Namespace:** Retain `eng::` across the codebase.
4. **Project & Asset Formats (Phase 4):** New formats `.anteray-project`, `.anteray`, `.anteray_prefab`; preserve read compatibility with `.hikari-project`, `.hikari`, and `.hikari_prefab`.
5. **Historical Accuracy:** Keep historical ADRs (`docs/adr/*`) and `CHANGELOG.md` entries historically accurate.
6. **Git Remote:** GitHub repository already renamed; `origin` updated to `git@github.com:zesakain/anteray-engine.git`.
7. **Logo Asset (Phase 1):** Keep `assets/textures/branding/hikari_logo.jpg` unchanged during Phase 1.

### Deferred Work (Not in Phase 1)
- CMake project/options/aliases/executables (`Phase 2`), internal C++ identifiers (`Phase 3`), file extensions & OS config directories (`Phase 4`), logo replacement, `HOMEPAGE_URL` / `tools/` / active architecture doc renames (`Phase 5`), and legacy project upgrade tooling (`Phase 6`).

---

## 14. Known Unknowns

- **External Consumers / Scripts:** Unknown if any external scripts outside this repo invoke `./build/*/bin/hikari` or parse `hikari_editor_layout.ini`.
- **Uncommitted User Projects (`projects*/`):** Contents of untracked `projects/`, `projects1/`, `projects5/`–`projects8/`, `projectsgg/` were intentionally not inspected or modified; assumed to contain `.hikari-project` manifests that Phase 4's dual-read strategy will preserve.
- **Pre-existing Local Edit:** `assets/benchmark-scenes/Bistro_Benchmark.hikari` has unstaged local modifications from the user prior to Phase 0; preserved untouched.
