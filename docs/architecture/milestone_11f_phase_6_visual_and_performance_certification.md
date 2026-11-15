# Hikari Machine — Milestone 11F: Material & Surface Fidelity
# Phase 6 Certification Report
## Visual Certification, 1080p Benchmarking & Comprehensive Test Suite

**Project:** Hikari Machine (C++20)  
**Milestone:** 11F (Material & Surface Fidelity)  
**Phase:** Phase 6 — Visual Certification, Benchmarking & Comprehensive Test Suite  
**Certification Status:** **PASSED (100% COMPLETE)**  
**Date:** 2026-11-15  
**Governing Standard:** Root `/AGENTS.md` (C++20, Zero Hot-Path Heap Allocation, Hard Module Boundaries, Exception-Free, No RTTI)  
**Hardware Test Floor:** NVIDIA GeForce GTX 1060 / P106-100 (Pascal Architecture, 6GB VRAM, Driver 535.247.01, Vulkan 1.3.242)

---

## 1. Executive Summary

Milestone 11F Phase 6 concludes the six-phase 11F material, shader, texture pipeline, and memory residency roadmap. This phase provides final end-to-end visual certification, 1080p baseline performance benchmarks across standard test scenes, and comprehensive automated test validation across all compilation presets.

Key Accomplishments in Phase 6:
1. **Deterministic `MaterialLab` Certification Scene**:
   - Synthesized persistent `.hikari` and companion glTF 2.0 geometry (`assets/benchmark-scenes/MaterialLab_Benchmark.hikari`, `material_lab.gltf`, `material_lab.bin`).
   - Implements 24 reference spheres arranged in a 4×6 grid over a neutral planar ground floor.
   - Deterministically demonstrates and validates all seven core glTF material extensions:
     - `KHR_materials_clearcoat`
     - `KHR_materials_sheen`
     - `KHR_materials_anisotropy`
     - `KHR_materials_transmission`
     - `KHR_materials_volume`
     - `KHR_materials_ior`
     - `KHR_materials_specular`
   - Configured with calibrated directional sunlight (with shadow maps), soft fill lighting, neutral spherical harmonics irradiance probes, and ACES tone mapping.
2. **1080p (1920×1080) Visual Certification**:
   - Upgraded offscreen capture tooling (`tools/capture_benchmark_scenes.cpp`) to native 1080p offscreen Vulkan rendering.
   - Generated uncompressed 24-bit RGB verification images across all four canonical scenes:
     - **Bistro**: Hero Corner Match, Exterior Street, Courtyard, Cafe Windows, Upper Facade.
     - **Sponza**: Overview Atrium, Detail Archway, Directional Shadows.
     - **DamagedHelmet**: Overview PBR reference.
     - **MaterialLab**: Full 4×6 Grid Overview, Dielectrics Row, Metals Row, Clearcoat & Sheen Row, Anisotropy & Transmission Row.
   - All captures were inspected and verified for correct tangent space shading, highlight conservation, fresnel attenuation, and absence of visual artifacts.
3. **Full 1080p Performance Benchmarks**:
   - Benchmarked 50 frames with warm-up for all four scenes on the Pascal test floor.
   - All scenes run well under the 16.6 ms (60 FPS) frame budget:
     - **Bistro** (6,011 entities, 1.45 GB VRAM): **6.72 ms** average (148.90 FPS), P99 = 7.96 ms.
     - **Sponza** (107 entities, 346.67 MB VRAM): **4.45 ms** average (224.94 FPS), P99 = 6.71 ms.
     - **MaterialLab** (28 entities): **1.57 ms** average (638.97 FPS), P99 = 3.45 ms.
     - **DamagedHelmet** (3 entities, 106.67 MB VRAM): **1.28 ms** average (783.85 FPS), P99 = 1.33 ms.
4. **Full Automated Test Suite**:
   - All 55 CTest test suites pass 100% across all compilation presets (`release`, `debug`, and `asan` with AddressSanitizer, LeakSanitizer, and UBSan).

---

## 2. MaterialLab Certification Scene

### 2.1 Scene Design & Geometric Layout
The MaterialLab scene is structured deterministically to expose potential shader defects, highlight artifacts, energy loss, or numerical instability.

- **Geometry**:
  - 24 UV reference spheres: Tessellated with $32 \times 32$ latitude/longitude rings ($1,089$ vertices, $6,144$ triangles per sphere).
  - Explicit analytic normals, UV coordinates, and orthonormal tangents ($(\pm \sin\phi, 0, \cos\phi, +1)$) embedded in binary buffers.
  - Sphere radius $R = 0.45\text{ m}$ (diameter $0.9\text{ m}$), placed with center at $Y = 0.45\text{ m}$ so each rests tangent to the ground.
  - Sphere grid spacing: $1.4\text{ m}$ in $X$ and $1.4\text{ m}$ in $Z$.
  - Planar ground floor: $12\text{ m} \times 12\text{ m}$ neutral gray plane ($Y = 0.0\text{ m}$) receiving directional shadows.
- **Lighting**:
  - Key Directional Sunlight: Direction $\frac{(0.4, 0.8, 0.35)}{\|(0.4, 0.8, 0.35)\|}$, Color $(1.0, 0.98, 0.94)$, Intensity $3.5\text{ lux}$, Shadow Casting enabled with bias $0.0015$.
  - Diffuse Ambient Bounce: Spherical Harmonics $L_2$ Irradiance Probe positioned at $(0.0, 1.5, 0.0)$, radius $20\text{ m}$, providing neutral balanced sky and ground ambient irradiance.
- **Framed Overview Camera**:
  - Position: $(0.0, 3.8, 7.2)$, Target: $(0.0, 0.45, 0.0)$, Up: $(0.0, 1.0, 0.0)$.
  - Vertical FOV: $50^\circ$, Aspect Ratio: $16:9$, Near: $0.1\text{ m}$, Far: $500\text{ m}$.
  - Perfectly frames all 24 spheres and ground plane shadows with balanced grazing and top-down specular highlight angles.

### 2.2 Material Parameter Matrix
The 24 material spheres are organized into 4 distinct functional rows:

| Row | Col | Name | Base Color | Rough | Metal | Key Extensions & Parameters | Shading Validation Target |
| :---: | :---: | :--- | :--- | :---: | :---: | :--- | :--- |
| **0** | **0** | Dielectric Smooth | `[0.9, 0.9, 0.9]` | 0.05 | 0.0 | — | Sharp mirror-like dielectric specular highlight ($F_0 = 0.04$) |
| **0** | **1** | Dielectric Med-Low | `[0.9, 0.9, 0.9]` | 0.25 | 0.0 | — | Smooth GGX lobe spread; energy conservation |
| **0** | **2** | Dielectric Medium | `[0.9, 0.9, 0.9]` | 0.50 | 0.0 | — | Balanced diffuse Lambertian vs specular GGX reflectance |
| **0** | **3** | Dielectric Rough | `[0.9, 0.9, 0.9]` | 0.75 | 0.0 | — | Broad grazing retro-reflection |
| **0** | **4** | Dielectric Ultra-Rough | `[0.9, 0.9, 0.9]` | 0.95 | 0.0 | — | Near-diffuse chalky surface; no grazing white rim blowout |
| **0** | **5** | High-IOR Diamond | `[0.95, 0.95, 0.95]` | 0.08 | 0.0 | `IOR=2.417`, `Specular=1.0` | High dielectric Fresnel reflectance ($F_0 = 0.172$); gem-like sparkle |
| **1** | **0** | Polished Gold | `[1.0, 0.766, 0.336]` | 0.08 | 1.0 | — | Tinted metallic specular highlight; zero diffuse reflection |
| **1** | **1** | Polished Copper | `[0.955, 0.637, 0.538]` | 0.15 | 1.0 | — | Warm copper metallic reflection |
| **1** | **2** | Polished Chrome | `[0.913, 0.921, 0.925]` | 0.05 | 1.0 | — | Bright achromatic metallic reflector |
| **1** | **3** | Satin Silver | `[0.972, 0.960, 0.915]` | 0.35 | 1.0 | — | Semi-rough brushed metal appearance |
| **1** | **4** | Rough Cast Iron | `[0.4, 0.4, 0.4]` | 0.75 | 1.0 | — | Diffuse-suppressed dark rough conductor |
| **1** | **5** | Specular Tinted | `[0.2, 0.2, 0.2]` | 0.20 | 0.0 | `Specular=1.0`, `SpecColor=[0.8, 0.2, 0.9]` | Colored dielectric specular highlight independent of base color |
| **2** | **0** | Red Car Paint | `[0.8, 0.05, 0.05]` | 0.60 | 0.0 | `Clearcoat=1.0`, `CoatRough=0.03` | Sharp glossy clearcoat over rough red base coat |
| **2** | **1** | Polyurethane Wood | `[0.55, 0.35, 0.18]` | 0.70 | 0.0 | `Clearcoat=0.8`, `CoatRough=0.15` | Dual-lobe clearcoat attenuation on organic substrate |
| **2** | **2** | Clearcoat Carbon | `[0.05, 0.05, 0.05]` | 0.40 | 0.0 | `Clearcoat=1.0`, `CoatRough=0.05` | Deep black lacquer with clear specular reflection |
| **2** | **3** | Blue Velvet Sheen | `[0.02, 0.05, 0.35]` | 0.70 | 0.0 | `SheenColor=[0.4, 0.6, 1.0]`, `SheenRough=0.3` | Charlie sheen grazing micro-fiber highlights |
| **2** | **4** | Crimson Silk Sheen | `[0.5, 0.02, 0.05]` | 0.50 | 0.0 | `SheenColor=[1.0, 0.5, 0.6]`, `SheenRough=0.5` | Fabric sheen energy scaling over diffuse base |
| **2** | **5** | Peach Fuzz Sheen | `[0.9, 0.6, 0.5]` | 0.80 | 0.0 | `SheenColor=[1.0, 0.85, 0.75]`, `SheenRough=0.7` | Soft grazing edge highlight on porous organic surface |
| **3** | **0** | Radial Brushed Alum | `[0.9, 0.9, 0.9]` | 0.25 | 1.0 | `AnisoStr=0.85`, `AnisoRot=0.0` | Directionally stretched highlight along bitangent ($B$) |
| **3** | **1** | Cross Brushed Steel | `[0.85, 0.85, 0.88]` | 0.35 | 1.0 | `AnisoStr=0.75`, `AnisoRot=1.5708` | Highlight rotated $90^\circ$ perpendicular to tangent |
| **3** | **2** | Clear Optical Glass | `[1.0, 1.0, 1.0]` | 0.02 | 0.0 | `Transmission=1.0`, `IOR=1.52`, `Thickness=0.9` | High transmittance transparent glass sphere |
| **3** | **3** | Tinted Emerald Glass | `[0.2, 0.9, 0.5]` | 0.05 | 0.0 | `Transmission=0.9`, `IOR=1.55`, `Atten=[0.2, 0.8, 0.4]`, `Dist=1.5` | Volumetric Beer-Lambert wavelength-dependent absorption |
| **3** | **4** | Coated Brushed Metal | `[0.8, 0.8, 0.8]` | 0.30 | 1.0 | `AnisoStr=0.8`, `Clearcoat=1.0`, `CoatRough=0.05` | Clearcoat isotropic layer over anisotropic metallic core |
| **3** | **5** | Coated Sheen Fabric | `[0.1, 0.2, 0.1]` | 0.60 | 0.0 | `SheenColor=[0.5, 0.8, 0.5]`, `Clearcoat=0.6`, `CoatRough=0.1` | Multi-lobe interaction: Clearcoat + Charlie sheen |
| **Floor**| — | Neutral Floor | `[0.35, 0.35, 0.35]` | 0.60 | 0.0 | — | Shadow receiving planar surface |

---

## 3. Visual Certification Captures

All captures were performed at native 1080p ($1920 \times 1080$), rendered offscreen via the Vulkan RHI backend with full PBR shading, directional cascaded shadow maps, spherical harmonics ambient illumination, and ACES tone mapping.

### 3.1 Verification Image Inventory

| Scene | Capture Filename | Resolution | Description & Camera Framing |
| :--- | :--- | :---: | :--- |
| **MaterialLab** | `test-assets/material_lab_1080p_grid.ppm` | $1920 \times 1080$ | Primary framed overview of all 24 material spheres |
| **MaterialLab** | `test-assets/material_lab_dielectrics.ppm` | $1920 \times 1080$ | Close-up of Row 0 (Dielectrics & Diamond IOR) |
| **MaterialLab** | `test-assets/material_lab_metals.ppm` | $1920 \times 1080$ | Close-up of Row 1 (Gold, Copper, Chrome, Specular) |
| **MaterialLab** | `test-assets/material_lab_clearcoat_sheen.ppm` | $1920 \times 1080$ | Close-up of Row 2 (Car Paint, Wood, Carbon, Velvet, Silk) |
| **MaterialLab** | `test-assets/material_lab_anisotropy_transmission.ppm` | $1920 \times 1080$ | Close-up of Row 3 (Brushed metals, Optical glass, Combined) |
| **Bistro** | `test-assets/bistro_hero_corner_match.ppm` | $1920 \times 1080$ | Canonical Hero Corner view matching Bistro reference |
| **Bistro** | `test-assets/bistro_persistent_street.ppm` | $1920 \times 1080$ | Exterior street perspective toward cafe terrace |
| **Bistro** | `test-assets/bistro_persistent_courtyard.ppm` | $1920 \times 1080$ | Courtyard view showing architectural stonework |
| **Bistro** | `test-assets/bistro_cafe_windows_closeup.ppm` | $1920 \times 1080$ | Storefront window glass, reflection, and interior shading |
| **Bistro** | `test-assets/bistro_building_facade_windows.ppm` | $1920 \times 1080$ | Upper facade windows, forge grating, and masonry detail |
| **Sponza** | `test-assets/sponza_persistent_overview.ppm` | $1920 \times 1080$ | Overview colonnade looking down atrium corridor |
| **Sponza** | `test-assets/sponza_persistent_detail_arch.ppm` | $1920 \times 1080$ | Detail closeup of archways, banners, and stone capitals |
| **Sponza** | `test-assets/sponza_persistent_shadows.ppm` | $1920 \times 1080$ | Sunlight grazing atrium floor with cascaded shadow edges |
| **DamagedHelmet** | `test-assets/damaged_helmet_persistent_scene.ppm` | $1920 \times 1080$ | Standard Khronos glTF PBR reference asset |

### 3.2 Visual Inspection Audit Findings
1. **Dynamic Range & Exposure**:
   - Inspecting pixel luminance across `material_lab_1080p_grid.ppm` confirms balanced dynamic range without clipping ($R \in [76, 251]$, $G \in [72, 251]$, $B \in [70, 251]$).
   - In Bistro (`bistro_hero_corner_match.ppm`), pixel values span full $[0, 255]$ with smooth luminance transitions under ACES tone mapping.
2. **Tangent-Space Stability**:
   - Anisotropic spheres (Row 3, Spheres 0 and 1) exhibit stable, continuous highlight elongation along orthogonal directions without tangent seams or poles distortion.
3. **Multi-Lobe Energy Conservation**:
   - Clearcoat spheres demonstrate dual-lobe highlights: a sharp specular gloss on top of the underlying substrate. Base lobe luminance correctly darkens as clearcoat Fresnel factor increases towards grazing angles ($1 - F_c$).
   - Charlie sheen exhibits soft velvet rim scattering at grazing angles with zero white blowouts or NaN/Inf energy explosions.
4. **Transmission & Beer-Lambert Volume**:
   - Optical glass spheres preserve smooth depth-attenuated background illumination without black halos or attachment recursion hazards.

---

## 4. Full 1080p Performance Benchmarking

### 4.1 Methodology & Environment
- **Platform**: Dual-GPU Development Rig (NVIDIA GeForce GTX 1060 / P106-100 Pascal Floor, 6 GB VRAM, Vulkan 1.3.242, Linux).
- **Harness**: `tools/capture_benchmark_scenes.cpp` compiled under CMake `release` preset (`-O3 -DNDEBUG`).
- **Timing**: Double-buffered GPU timestamp queries (`rhi::RhiDevice::getQueryTimeMs`) combined with high-resolution host clocks (`std::chrono::high_resolution_clock`).
- **Repetition**: 15 warm-up frames followed by 50 timed benchmark frames per scene.

### 4.2 Frame-Time Telemetry Summary (1920×1080)

| Scene | Entities Loaded | Avg Frame (ms) | Median (ms) | P95 (ms) | P99 (ms) | Max (ms) | FPS (Avg) | Target Budget |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **DamagedHelmet** | 3 | **1.28** | 1.27 | 1.30 | 1.33 | 1.33 | **783.85** | $\le 16.6\text{ ms}$ |
| **MaterialLab** | 28 | **1.57** | 1.52 | 1.59 | 3.45 | 3.45 | **638.97** | $\le 16.6\text{ ms}$ |
| **Sponza** | 107 | **4.45** | 4.43 | 4.87 | 6.71 | 6.71 | **224.94** | $\le 16.6\text{ ms}$ |
| **Bistro** | 6,011 | **6.72** | 6.73 | 7.28 | 7.96 | 7.96 | **148.90** | $\le 16.6\text{ ms}$ |

> [!NOTE]
> All four benchmark scenes run with enormous headroom above the 60 FPS (16.6 ms) frame budget. Even the massive Amazon Lumberyard Bistro scene (6,011 entities, 1.45 GB VRAM) renders at **148.90 FPS** on Pascal baseline hardware, with P99 frame times of **7.96 ms** ($< 50\%$ of the budget).

### 4.3 GPU Pass & CPU Submission Breakdown

| Metric (ms) | DamagedHelmet | MaterialLab | Sponza | Bistro |
| :--- | :---: | :---: | :---: | :---: |
| **CPU Recording** | 0.04 | 0.05 | 0.12 | 1.65 |
| **GPU Total** | 1.08 | 1.34 | 4.06 | 4.70 |
| **Main PBR Pass** | 0.46 | 0.71 | 3.43 | 4.06 |
| **Tone Mapping Pass** | 0.24 | 0.24 | 0.24 | 0.24 |
| **Bloom Down/Upsample** | 0.36 | 0.37 | 0.37 | 0.37 |

### 4.4 Texture Residency & Memory Telemetry

| Telemetry Dimension | DamagedHelmet | MaterialLab | Sponza | Bistro |
| :--- | :---: | :---: | :---: | :---: |
| **Resident GPU Memory** | 106.67 MB | 0.00 MB | 346.67 MB | 1,448.84 MB |
| **Active Texture Count** | 5 | 0 | 65 | 160 |
| **Total Allocated Memory** | 106.67 MB | 0.00 MB | 346.67 MB | 1,448.84 MB |
| **Frame Evictions** | 0 | 0 | 0 | 0 |
| **Residency Hitching** | None | None | None | None |

- **Texture Streaming Stability**: Under normal steady-state rendering, resident GPU memory stays strictly within allocated limits without frame-time jitter or unexpected promotions/demotions.
- **Eviction Safety**: Under simulated memory pressure (verified in `texture_residency_test`), deferred destruction queues retire unreferenced textures asynchronously with zero GPU pipeline stalls.

---

## 5. Comprehensive Automated Test Suite

All 55 CTest test suites were executed across `release`, `debug`, and `asan` (AddressSanitizer + LeakSanitizer + UndefinedBehaviorSanitizer) presets.

| Preset | Total Suites | Passed | Failed | Pass Rate | Total Execution Time |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Release** (`cmake --preset release`) | 55 | 55 | 0 | **100%** | 12.74 s |
| **Debug** (`cmake --preset debug`) | 55 | 55 | 0 | **100%** | 47.09 s |
| **AddressSanitizer** (`cmake --preset asan`) | 55 | 55 | 0 | **100%** | 200.26 s |

### Sanitizer Verification
- **AddressSanitizer (ASan)**: Zero out-of-bounds reads/writes, zero stack/global buffer overflows, zero use-after-free across texture lifecycle and deferred destruction queues.
- **LeakSanitizer (LSan)**: Zero memory leaks detected across all engine subsystems.
- **UndefinedBehaviorSanitizer (UBSan)**: Zero integer overflows, zero misaligned pointer dereferences, zero null pointer violations.

---

## 6. Architecture & Rule Compliance Audit

| Requirement | Contract Clause | Audit Verification | Status |
| :--- | :--- | :--- | :---: |
| **Dependency Boundaries** | Root `AGENTS.md` §2.1 | `/engine` has zero `#include` from `/game` or `/editor`. Checked via build dependency graph. | **COMPLIANT** |
| **Vocabulary Cleanliness** | Root `AGENTS.md` §2.2 | Zero game-specific vocabulary in engine modules. | **COMPLIANT** |
| **RHI Isolation** | Root `AGENTS.md` §2.3 | Only `engine/rhi` includes Vulkan headers. Shaders consume generic SPIR-V. | **COMPLIANT** |
| **Memory Ownership** | Root `AGENTS.md` §2.4, §5 | Zero raw `delete`, zero `std::shared_ptr`. Ownership is strictly `std::unique_ptr` or generational handles. | **COMPLIANT** |
| **Hot Path Discipline** | Root `AGENTS.md` §2.5, §6 | Zero heap allocation, zero mutex locks, zero script invocations during render loop recording. | **COMPLIANT** |
| **Exception & RTTI-Free** | Root `AGENTS.md` §2.6, §4 | `-fno-exceptions -fno-rtti` enabled globally; all errors returned as `Result<T, Error>`. | **COMPLIANT** |
| **No Third-Party Bloat** | Root `AGENTS.md` §2.7 | Zero new external dependencies. Standard Khronos glTF extensions handled via embedded `cgltf`. | **COMPLIANT** |
| **Shared World & Conductor**| Root `AGENTS.md` §8 | Single shared ECS World with alternating standby/active mode states. | **COMPLIANT** |
| **Deferred GPU Destruction**| Phase 11F-R1 Mandate | Retired textures tracked by completion serials; zero synchronous `waitIdle` in eviction loop. | **COMPLIANT** |

---

## 7. Final Verdict

### Final Milestone 11F Phase 6 Verdict: **PASS**

Milestone 11F (Material & Surface Fidelity) has achieved full physical, visual, performance, and architectural certification.
All six phases are fully validated, documented, and reproducible on baseline hardware.
