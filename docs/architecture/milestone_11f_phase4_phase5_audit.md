# Hikari Machine — Milestone 11F: Material & Surface Fidelity
# Combined Audit & Architecture Plan: Phase 4 & Phase 5

**Project:** Hikari Machine (C++20)  
**Milestone:** 11F (Material & Surface Fidelity)  
**Batch Scope:** Phase 4 (Advanced PBR Pipeline) & Phase 5 (Texture Residency and Memory Management)  
**Status:** Audit & Architecture Specification Completed  
**Date:** 2026-11-15  
**Governing Standard:** Root `/AGENTS.md` (C++20, Zero Hot-Path Heap Allocation, Hard Module Boundaries, Exception-Free)

---

## 1. Executive Summary

Milestone 11F Phase 4 and Phase 5 complete the modern material, shading, and texture management architecture of Hikari Machine. 

- **Phase 4 (Advanced PBR Pipeline)** elevates the shading engine to full Khronos PBR material compliance by adding physically grounded **Anisotropic GGX** (`KHR_materials_anisotropy`) and **Charlie Sheen** (`KHR_materials_sheen`), integrating them with Phase 3's clearcoat, transmission, volume Beer-Lambert extinction, IOR, and specular lobes into a unified, energy-conserving dual-lobe BRDF model with strict numerical stability.
- **Phase 5 (Texture Residency and Memory Management)** establishes a deterministic texture lifecycle, hardware-aware memory budgets, LRU eviction with in-flight GPU safety, and descriptor fallback protection to prevent VRAM exhaustion and descriptor corruption during large-scale scene streaming.

---

## 2. Phase 4 Shading Pipeline Architecture

### 2.1 Audit of Existing Shading Path
Hikari Machine maintains two primary PBR fragment shaders:
1. `assets/shaders/pbr_mesh_opaque.frag` (Opaque and Alpha-Masked geometry with early-Z rejection)
2. `assets/shaders/pbr_mesh.frag` (Transparent and Transmissive geometry with alpha blending)

Both shaders currently share:
- Set 0: Material descriptor bindings (Bindings 0..4 base textures, 5 UBO, 6..12 extension textures).
- Set 1: Scene lighting & shadow bindings (Camera UBO, Directional Shadow Map, GTAO, SSGI, IBL Irradiance/Prefiltered cubemaps, BRDF LUT).

### 2.2 Numerical Stability & Robustness
Fragment evaluations must be guarded against:
- **Degenerate Normals & Tangents**: Zero-length vertex normals or tangents produce NaNs during TBN construction. Shaders must sanitize TBN vectors with epsilon guards (`length(N) < 1e-4 ? vec3(0,0,1) : normalize(N)`).
- **Grazing Angle Singularities**: $N \cdot V \to 0$ or $N \cdot L \to 0$ causes division-by-zero in Smith visibility and specular denominators. All geometric denominators must be clamped to $\max(\dots, 10^{-4})$.
- **Roughness Lower Bounds**: Microfacet roughness $\alpha \to 0$ collapses the GGX distribution to a Dirac delta function, creating IEEE-754 floating-point overflow. Roughness is clamped to $\max(\text{roughness}, 0.045)$.

### 2.3 Microfacet Formulations
#### A. Anisotropic GGX (`KHR_materials_anisotropy`)
Anisotropic surfaces (such as brushed metals, carbon fiber, and phonograph grooves) reflect light directionally along a tangent frame $(T, B, N)$.
- Anisotropy strength $s \in [0, 1]$ and rotation $\theta \in [-\pi, \pi]$ rotate the local tangent frame:
  $$T' = T \cos\theta + B \sin\theta, \quad B' = -T \sin\theta + B \cos\theta$$
- Roughness along tangent and bitangent axes:
  $$\alpha_x = \text{clamp}(\text{roughness}^2 \times (1 + s), 0.001, 1.0)$$
  $$\alpha_y = \text{clamp}(\text{roughness}^2 \times (1 - s), 0.001, 1.0)$$
- Anisotropic GGX distribution (Burley / Heitz):
  $$D_{\text{aniso}}(H) = \frac{1}{\pi \alpha_x \alpha_y \left( \left(\frac{T' \cdot H}{\alpha_x}\right)^2 + \left(\frac{B' \cdot H}{\alpha_y}\right)^2 + (N \cdot H)^2 \right)^2}$$
- Anisotropic Smith visibility:
  $$\Lambda_v = \sqrt{\alpha_x^2 (T' \cdot V)^2 + \alpha_y^2 (B' \cdot V)^2 + (N \cdot V)^2}$$
  $$\Lambda_l = \sqrt{\alpha_x^2 (T' \cdot L)^2 + \alpha_y^2 (B' \cdot L)^2 + (N \cdot L)^2}$$
  $$V_{\text{aniso}} = \frac{0.5}{(N \cdot V) \Lambda_l + (N \cdot L) \Lambda_v + 10^{-4}}$$

#### B. Charlie Sheen (`KHR_materials_sheen`)
Sheen simulates soft, micro-fibrous cloth, velvet, and peach fuzz through grazing-angle scattering:
- Charlie microfacet distribution (Estevez and Kulla, 2017):
  $$D_{\text{sheen}}(H) = \frac{2 + \frac{1}{\alpha_s}}{2\pi} (1 - (N \cdot H)^2)^{\frac{1}{2\alpha_s}}$$
- Ashikhmin-Velazquez shadow-masking approximation:
  $$V_{\text{sheen}} = \frac{1}{4(N \cdot L + N \cdot V - (N \cdot L)(N \cdot V))}$$
- Energy scaling: The base diffuse and specular lobes are attenuated by $(1 - E_{\text{sheen}})$ to conserve energy.

### 2.4 Refraction Architecture Analysis
- **Screen-Space Refraction (SSRf)** requires sampling the rendered opaque scene color while shading transparent objects.
- **Dependency Constraint**: In `SceneRenderer`, `hdrTarget_` is currently bound as the active color attachment throughout `MainPbrPass`. Sampling `hdrTarget_` inside the same render pass is an attachment feedback loop hazard prohibited by Vulkan §14.2.2.
- **Architectural Solution**: Analytical transmission via Beer-Lambert absorption is physically accurate for thin/thick transmissive media without background deformation. True screen-space refraction would require splitting `MainPbrPass` into `MainOpaquePass` and `MainTransparentPass` with an intermediate `OpaqueSceneCopyPass`. Per the directive, analytical transmission remains our robust, hazard-free baseline for Phase 4, with screen-space copy documented for future RenderGraph expansion.

---

## 3. Phase 5 Texture Residency Architecture

### 3.1 State Machine Lifecycle
Each managed texture asset in `AssetManager` transitions through explicit residency states:
```
[Unloaded]  <----->  [Loading]  ----->  [CpuResident]
    ^                                        |
    |                                        v
[Failed]   <-- (OOM/Error) <--      [GpuUploadPending]
    ^                                        |
    |                                        v
    +---------  [EvictionPending] <----- [GpuResident]
```
- **Unloaded**: Header metadata registered, no CPU or GPU memory allocated.
- **Loading**: Asynchronous or synchronous decode in progress.
- **CpuResident**: Host bitmap pixels resident in CPU heap memory.
- **GpuUploadPending**: CPU bitmap queued for Vulkan staging and transfer.
- **GpuResident**: Vulkan `VkImage`, `VkDeviceMemory`, and `VkImageView` allocated and resident on GPU.
- **EvictionPending**: Marked for LRU reclaim when memory pressure exceeds budget.
- **Failed**: File read or GPU allocation failure; safely points to fallback texture.

### 3.2 Memory Budgeting & Metrics
- **Budget Configuration**: Configurable per platform/tier (Default: 1024 MB for desktop, 512 MB for Tier 0 floor).
- **Tracked Telemetry**:
  - `estimatedGpuMemoryBytes`: Total resident VRAM footprint calculated from resolution, format, and mip levels.
  - `residentTextureCount`: Number of `GpuResident` textures.
  - `evictionCount`: Lifetime count of evicted textures.
  - `uploadBytes`: Total bytes uploaded across current session.
  - `isUnderMemoryPressure`: Boolean flag asserting when memory usage exceeds 90% of budget.

### 3.3 Safe Eviction & Descriptor Fallback Protection
- Evicting a texture must **never** invalidate active material descriptor sets or cause Vulkan validation errors.
- When `evictTexture(AssetId)` or `enforceTextureBudget()` is invoked:
  1. The texture's `GpuTexture` handle is safely destroyed.
  2. Any material referencing this texture is updated to bind the appropriate 1x1 fallback texture (`WhiteSRGB`, `WhiteLinear`, `BlackSRGB`, `FlatNormalLinear`).
  3. The material's UBO flag for that texture is cleared (`hasBaseColorTex = 0`), ensuring shaders fallback cleanly to analytical factors.
  4. Pinned textures (e.g. system fallbacks, active camera targets) are strictly immune from eviction.

---

## 4. Implementation Steps & Milestones

1. **Phase 4**:
   - Update `Material` CPU model with `AnisotropyExtension` and `SheenExtension`.
   - Implement cgltf parsing for `KHR_materials_anisotropy` and `KHR_materials_sheen` in `gltf_internal.h`.
   - Expand `MaterialUniformBufferData` to 192 bytes and Set 0 to 16 bindings (15 samplers + 1 UBO).
   - Implement Anisotropic GGX, Charlie sheen, and numerical guards in `pbr_mesh.frag` and `pbr_mesh_opaque.frag`.
   - Regenerate embedded SPIR-V headers.
   - Add unit test suite in `advanced_pbr_pipeline_test.cpp`.
2. **Phase 5**:
   - Add `texture_residency.h` defining states, metrics, and budget configs.
   - Integrate residency tracking, LRU frame tagging, and budget enforcement into `AssetManager`.
   - Add descriptor fallback update path upon texture eviction.
   - Add unit test suite in `texture_residency_test.cpp`.
   - Run stress tests and full regression verification.
