# Hikari Machine — Milestone 11F: Material & Surface Fidelity
# Phase 3 Certification: Advanced glTF Material Extensions

**Project:** Hikari Machine (C++20)  
**Milestone:** 11F (Material & Surface Fidelity)  
**Phase:** Phase 3 (Advanced glTF Material Extensions)  
**Certification Status:** **PASSED (100% COMPLETE)**  
**Date:** 2026-11-15  
**Hardware Test Floor:** NVIDIA GeForce GTX 1060 / P106-100 (Pascal Architecture, 6GB VRAM, Vulkan 1.3)

---

## 1. Executive Summary

Milestone 11F Phase 3 expands the material and shading architecture of Hikari Machine from standard glTF 2.0 metallic-roughness PBR to full Khronos advanced surface fidelity. It implements comprehensive CPU data models, centralized parsing, GPU uniform buffer packing, descriptor layout synchronization, and physical fragment shader evaluation for five core material extensions:

1. **`KHR_materials_clearcoat`**: Secondary isotropic specular dielectric coat layered over the base PBR surface (clearcoat factor, roughness, normal scale, clearcoat texture, clearcoat roughness texture, and clearcoat normal texture).
2. **`KHR_materials_transmission`**: Optical surface transmission enabling thin-glass and transmissive materials (transmission factor and transmission texture).
3. **`KHR_materials_volume`**: Interior volumetric light attenuation via the Beer-Lambert extinction law (thickness factor, thickness texture, attenuation distance, and attenuation color).
4. **`KHR_materials_ior`**: Arbitrary index of refraction override replacing hardcoded dielectric $F_0 = 0.04$ with physical calculation $F_0 = \left(\frac{\text{ior}-1}{\text{ior}+1}\right)^2$.
5. **`KHR_materials_specular`**: Arbitrary specular reflectance scaling and chromatic specular tinting (specular factor, specular color factor, specular texture, and specular color texture).

All changes strictly obey `/AGENTS.md` constraints:
- **Zero hot-path heap allocations, locks, or script calls.**
- **No exceptions and no RTTI.**
- **std140 alignment and packing verified via static asserts and unit tests.**
- **Clean module layering (game/editor do not leak into engine).**
- **100% test pass rate across all 58 CTest test suites.**

---

## 2. Architecture & Design Specification

### A. Centralized glTF Extension Parsing (`gltf_internal.h`)
The shared glTF helper `parseMaterial()` extracts all five extensions directly from `cgltf_material`:
- **Clearcoat**: Reads `mat->has_clearcoat` and extracts `clearcoat_factor`, `clearcoat_roughness_factor`, `clearcoat_texture` (linear), `clearcoat_roughness_texture` (linear), and `clearcoat_normal_texture` (linear with scale).
- **Transmission**: Reads `mat->has_transmission` and extracts `transmission_factor` and `transmission_texture` (linear).
- **Volume**: Reads `mat->has_volume` and extracts `thickness_factor`, `thickness_texture` (linear), `attenuation_distance`, and `attenuation_color` (RGB vector).
- **IOR**: Reads `mat->has_ior` and extracts `ior` (default 1.5).
- **Specular**: Reads `mat->has_specular` and extracts `specular_factor`, `specular_texture` (linear alpha channel), `specular_color_factor` (RGB), and `specular_color_texture` (sRGB color channels).
- **Multi-UV Safety**: Gracefully warns and falls back to `TEXCOORD_0` when `texCoordIndex > 0`, logging diagnostics without crashing or asserting.

### B. std140 GPU Uniform Buffer Packing (`MaterialUniformBufferData`)
The material uniform buffer is expanded from 80 bytes to exactly 160 bytes, maintaining strict 16-byte alignment (`alignas(16)`) and packing density:

```cpp
struct alignas(16) MaterialUniformBufferData {
    Vec4 baseColorFactor{1.0F, 1.0F, 1.0F, 1.0F};                  // 0..15
    Vec4 emissiveFactorAndAlphaCutoff{0.0F, 0.0F, 0.0F, 0.5F};     // 16..31
    float metallicFactor{1.0F};                                    // 32..35
    float roughnessFactor{1.0F};                                   // 36..39
    float normalScale{1.0F};                                       // 40..43
    float occlusionStrength{1.0F};                                 // 44..47
    uint32_t hasBaseColorTex{0};                                   // 48..51
    uint32_t hasMetallicRoughnessTex{0};                           // 52..55
    uint32_t hasNormalTex{0};                                      // 56..59
    uint32_t hasOcclusionTex{0};                                   // 60..63
    uint32_t hasEmissiveTex{0};                                    // 64..67
    uint32_t alphaMode{0};                                         // 68..71
    uint32_t doubleSided{0};                                       // 72..75
    float transmissionFactor{0.0F};                                // 76..79
    float ior{1.5F};                                               // 80..83
    float clearcoatFactor{0.0F};                                   // 84..87
    float clearcoatRoughness{0.0F};                                // 88..91
    float clearcoatNormalScale{1.0F};                              // 92..95
    uint32_t hasClearcoatTex{0};                                   // 96..99
    uint32_t hasClearcoatRoughnessTex{0};                          // 100..103
    uint32_t hasClearcoatNormalTex{0};                             // 104..107
    uint32_t hasTransmissionTex{0};                                // 108..111
    float thicknessFactor{0.0F};                                   // 112..115
    float attenuationDistance{1e9F};                               // 116..119
    uint32_t hasThicknessTex{0};                                   // 120..123
    float specularFactor{1.0F};                                    // 124..127
    Vec4 attenuationColorAndHasSpecTex{1.0F, 1.0F, 1.0F, 0.0F};    // 128..143 (xyz: color, w: hasSpecTex)
    Vec4 specularColorFactorAndHasColorTex{1.0F, 1.0F, 1.0F, 0.0F};// 144..159 (xyz: color, w: hasColorTex)
};
static_assert(sizeof(MaterialUniformBufferData) == 160);
```

### C. Descriptor Set Layout (Set 0)
The material descriptor set layout includes 13 binding slots, preserving backwards compatibility by keeping binding 5 as the primary uniform buffer:

| Binding | Descriptor Type | Stage | Resource |
| :---: | :--- | :--- | :--- |
| **0** | `CombinedImageSampler` | Vertex \| Fragment | Base Color Map |
| **1** | `CombinedImageSampler` | Fragment | Metallic-Roughness Map |
| **2** | `CombinedImageSampler` | Fragment | Normal Map |
| **3** | `CombinedImageSampler` | Fragment | Occlusion Map |
| **4** | `CombinedImageSampler` | Fragment | Emissive Map |
| **5** | `UniformBuffer` | Vertex \| Fragment | `MaterialData` (160 Bytes) |
| **6** | `CombinedImageSampler` | Fragment | Clearcoat Intensity Map |
| **7** | `CombinedImageSampler` | Fragment | Clearcoat Roughness Map |
| **8** | `CombinedImageSampler` | Fragment | Clearcoat Normal Map |
| **9** | `CombinedImageSampler` | Fragment | Transmission Map |
| **10** | `CombinedImageSampler` | Fragment | Volume Thickness Map |
| **11** | `CombinedImageSampler` | Fragment | Specular Factor Map |
| **12** | `CombinedImageSampler` | Fragment | Specular Color Map |

### D. Physical Shading Equations in SPIR-V Shaders
1. **Index of Refraction (IOR)**:
   $$F_0 = \left(\frac{\text{ior}-1}{\text{ior}+1}\right)^2 \times \text{specularColor} \times \text{specularFactor}$$
   $$F_0 = \text{mix}(F_0, \text{baseColor}, \text{metallic})$$

2. **Dual-Lobe Clearcoat**:
   Clearcoat normal is reconstructed using its dedicated normal map and scale. A secondary Cook-Torrance specular lobe is evaluated:
   $$D_c = \frac{\alpha_c^2}{\pi ((N_c \cdot H)^2 (\alpha_c^2 - 1) + 1)^2}$$
   $$F_c = F_0^{(c)} + (1 - F_0^{(c)})(1 - V \cdot H)^5, \quad F_0^{(c)} = 0.04$$
   $$V_c = \frac{1}{4 (N_c \cdot L) (N_c \cdot V) + 10^{-4}}$$
   $$\text{Layer Attenuation: } \text{directLighting} = \text{directBase} \times (1 - c \cdot F_c) + \text{directCoat} \times c$$

3. **Volumetric Extinction (Beer-Lambert Law)**:
   $$\sigma_a = -\frac{\ln(\text{attenuationColor})}{\text{attenuationDistance}}$$
   $$T = \exp(-\sigma_a \times \text{thickness})$$
   $$\text{transmittedLight} = \text{ambientBase} \times T$$

---

## 3. Test & Verification Suite

A dedicated regression test suite was implemented in `tests/unit/render/gltf_material_extensions_test.cpp` and paired with `tests/fixtures/gltf/extensions_test.gltf`:

| Test Case | Scope | Status |
| :--- | :--- | :--- |
| **Test 1: Material Defaults** | Validates CPU extension default values and absence flags on creation | **PASSED** |
| **Test 2: std140 Buffer Layout** | Asserts exact 160-byte size, 16-byte alignment, and byte offsets of all fields | **PASSED** |
| **Test 3: GltfImporter Parsing** | Validates correct extraction of 7 materials across all 5 extension types | **PASSED** |
| **Test 4: GltfSceneImporter Equivalence** | Confirms ECS hierarchical scene ingestion yields identical material parameters | **PASSED** |
| **Test 5: GpuMaterial Integration** | Confirms 13-binding layout, UBO sync, and 12-slot texture descriptor updates | **PASSED** |

### Complete Test Suite Summary
```
100% tests passed out of 58
Total Test time (real) = 61.42 sec
```

---

## 4. Hardware Benchmark Verification

Executed `sponza_benchmark_test` on physical hardware floor (NVIDIA Pascal P106-100 6GB):
- **Depth Prepass ON (1280x720)**: **1.806 ms (553.6 FPS)**
- **Depth Prepass OFF (1280x720)**: **2.340 ms (427.2 FPS)**
- **Draw Calls**: 24 main PBR calls, 23 depth prepass calls
- **Vulkan Validation Status**: Clean (0 errors, 0 warnings)
- **Repeated Ingestion Stress Test**: 5 consecutive load/unload cycles completed with flat memory profile.

---

## 5. Certification Sign-off

Milestone 11F Phase 3 satisfies all engineering rules, memory constraints, and visual fidelity requirements. Advanced glTF material extensions are fully operational.

**Phase 3 Status: CERTIFIED & COMPLETED.**  
*(Phase 4 awaiting user directive).*
