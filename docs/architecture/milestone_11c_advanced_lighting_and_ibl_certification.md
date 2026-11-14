# Hikari-Machine Milestone 11C: Advanced Lighting & Image-Based Lighting (IBL)
## Final Certification and Empirical Verification Report

**Repository:** `/home/lux/hikari-machine`  
**Milestone:** 11C (Advanced Lighting & Image-Based Lighting)  
**Hardware Floor:** NVIDIA Pascal (P106-100 6GB, Vulkan 1.3 / Linux 6.6)  
**Target Resolution:** Native 1080p ($1920 \times 1080$)  
**Target Frame Budget:** $\le 16.67\text{ ms}$ ($\ge 60\text{ FPS}$)  
**Status:** COMPLETE & CERTIFIED  
**Overall 1080p Performance:** 1.823 ms average frame time (548.6 FPS), P95: 2.404 ms (415.9 FPS floor)  
**Test Suite Verification:** 52/52 tests passing (100% CTest pass rate)  
**Commit Series:**
- `ab9731a`: `render/ibl: implement HDR decoding, cubemap convolution, and BRDF LUT pipeline`
- `c879ab6`: `render/ibl: add GPU cubemap and BRDF LUT upload factories`
- `3fdbef1`: `render/shaders: integrate IBL split-sum path in PBR fragment shaders`
- `614eff0`: `render/scene: wire IBL environment map into SceneRenderer`
- `0a496f0`: `render/ibl: clean up BRDF LUT naming and struct aggregate initialization`
- `4ae07ef`: `tests/render: add comprehensive IBL validation and DamagedHelmet visual test`

---

## 1. Executive Summary

Milestone 11C introduces a physically based Image-Based Lighting (IBL) system into Hikari-Machine's Cook-Torrance microfacet rendering pipeline. Using the split-sum approximation, the renderer accurately evaluates ambient diffuse irradiance and specular reflection from high-dynamic-range (HDR) environment probes across continuous roughness values while strictly conserving energy.

All deliverables have been designed, implemented, integrated, empirically verified, and certified:

1. **HDR Environment Pipeline (Work Package A):**
   - 32-bit float equirectangular HDR decoding via `stbi_loadf`.
   - Procedural Rayleigh and Mie atmospheric physical sky generator.
   - Seam-aware equirectangular-to-cubemap projection generating 6 cubemap faces with Vulkan-compliant coordinate orientation (`+X, -X, +Y, -Y, +Z, -Z`) in half-precision `RGBA16_FLOAT`.

2. **Diffuse Irradiance Convolution (Work Package B):**
   - Cosine-weighted hemisphere irradiance integration over the upper hemisphere for each face texel.
   - Strict energy conservation ensuring integrated diffuse radiance does not exceed input environment irradiance.
   - Pure conductor exclusion ($1.0 - \text{metallic}$) in the shading stage preventing diffuse energy bleed onto metallic surfaces.

3. **Specular GGX Prefiltering (Work Package C):**
   - Roughness-dependent specular cubemap prefiltering using importance-sampled Cook-Torrance GGX distribution with Hammersley low-discrepancy sequences.
   - Multi-level mip chain generation ($128 \times 128$ base down to $8 \times 8$ across 5 mip levels), linearly parameterizing perceptual roughness $0.0 \le \alpha \le 1.0$.

4. **Split-Sum 2D BRDF Integration LUT (Work Package D):**
   - Numerical integration of Cook-Torrance geometry ($G$) and Fresnel ($F$) terms over $(N \cdot V, \text{roughness}) \in [0, 1]^2$.
   - Precomputed $256 \times 256$ 2D LUT stored in `RGBA16_FLOAT`, encoding the Fresnel scale ($R$) and bias ($G$) factors:
     $$F_{env}(v, n) = F_0 \cdot \text{LUT.x} + (1 - F_0) \cdot \text{LUT.y}$$

5. **PBR Fragment Shader Integration (Work Package E):**
   - Descriptor Set 1 extended with `samplerCube uIrradianceMap` (binding 2), `samplerCube uPrefilteredEnvMap` (binding 3), and `sampler2D uBrdfLut` (binding 4) in `pbr_mesh.frag` and `pbr_mesh_opaque.frag`.
   - Smooth fallback to uniform flat ambient (`uLighting.ambientColor`) when IBL is disabled via configuration or when no environment map is assigned.

6. **Runtime Environment Lifecycle & Memory Management (Work Package F):**
   - Dynamic GPU texture allocation and cubemap upload in `GpuTexture::createCubemap` and `GpuTexture::createBrdfLut`.
   - Hot-swappable environment maps at runtime without pipeline recreation or GPU resource leaks.
   - Seamless integration with `GraphicsSettings::LightingSettings` (`useIbl`, `environmentIntensity`, `environmentMapPath`).

7. **Hardware Floor Certification (Work Package G):**
   - Validated on NVIDIA Pascal P106-100 at native 1080p ($1920 \times 1080$).
   - 1080p worst-case 9-scenario benchmark sustained 548.6 FPS (1.823 ms frame time), well within the $16.67\text{ ms}$ 60 FPS budget.
   - Visual certification on the Khronos DamagedHelmet glTF asset verified 16,149 shaded pixels with realistic environmental reflections.

---

## 2. Mathematical Formulation & Architecture

### 2.1 The Split-Sum Cook-Torrance Approximation

The rendering equation for indirect specular reflectance is given by:

$$L_o(p, v) = \int_{\Omega} L_i(p, l) f_r(p, l, v) \cos\theta \, dl$$

Under the Epic Games split-sum approximation, this integral is decomposed into two independent factors:

$$L_o(p, v) \approx \underbrace{\left( \frac{1}{N} \sum_{k=1}^N L_i(l_k) \right)}_{\text{Prefiltered Environment Map}} \cdot \underbrace{\left( \frac{1}{N} \sum_{k=1}^N \frac{f_r(l_k, v) \cos\theta_k}{p(l_k, v)} \right)}_{\text{2D BRDF Integration LUT}}$$

### 2.2 Diffuse Irradiance Integral

The diffuse irradiance is precomputed per cubemap texel normal direction $N$:

$$E(N) = \frac{1}{\pi} \int_{\Omega} L(l) (N \cdot l) \, dl$$

Discrete integration uses spherical coordinates $\phi \in [0, 2\pi)$ and $\theta \in [0, \frac{\pi}{2}]$ with step sizes $d\phi = 0.05$ and $d\theta = 0.025$:

$$E(N) \approx \frac{\pi}{N_{samples}} \sum_{\phi} \sum_{\theta} L(\text{tangentToWorld}(l)) \cos\theta \sin\theta$$

In the PBR fragment shader, the diffuse ambient contribution is modulated by the Fresnel reflection term and material metallicity:

$$k_S = F_{\text{SchlickRoughness}}(N \cdot V, F_0, \text{roughness})$$
$$k_D = (1 - k_S) \cdot (1 - \text{metallic})$$
$$\text{diffuseAmbient} = k_D \cdot \text{albedo} \cdot \text{texture}(uIrradianceMap, N).rgb$$

For pure metals ($\text{metallic} = 1.0$), $k_D = 0$, guaranteeing zero diffuse ambient reflection.

### 2.3 Specular Environment Prefiltering & BRDF LUT

Specular prefiltering draws $N = 1024$ samples using the Hammersley sequence:

$$x_i = \left( \frac{i}{N}, \phi_2(i) \right)$$

Importance-sampled half-vectors $H$ are generated from the GGX distribution:

$$\phi = 2\pi x_1, \quad \cos\theta = \sqrt{\frac{1 - x_2}{1 + (\alpha^2 - 1) x_2}}, \quad \sin\theta = \sqrt{1 - \cos^2\theta}$$

The reflection vector $L = 2(V \cdot H)H - V$ samples the environment map. For the 2D BRDF LUT, the Schlick-GGX geometry shadowing function with Smith's method is integrated against $(N \cdot V, \alpha)$:

$$G(N, V, L) = G_1(N, V) \cdot G_1(N, L)$$
$$G_1(N, V) = \frac{N \cdot V}{(N \cdot V)(1 - k) + k}, \quad k = \frac{\alpha^2}{2}$$

The resulting LUT stores scale and bias in the red and green channels of a $256 \times 256$ `RGBA16_FLOAT` texture.

---

## 3. GPU Descriptor Set & Pipeline Layout

The scene lighting descriptor set (Set 1) was updated to provide all necessary textures for both direct directional lighting and indirect IBL:

| Binding | Type | Shader Stage | Resource | Format / Dimensions |
| :---: | :--- | :--- | :--- | :--- |
| **0** | `UniformBuffer` | Vertex + Fragment | `GpuLightingData` UBO | 1696 bytes (std140) |
| **1** | `CombinedImageSampler` | Fragment | `uShadowMap` | `D32_FLOAT` ($2048 \times 2048$) |
| **2** | `CombinedImageSampler` | Fragment | `uIrradianceMap` | `RGBA16_FLOAT` Cubemap ($64 \times 64 \times 6$) |
| **3** | `CombinedImageSampler` | Fragment | `uPrefilteredEnvMap` | `RGBA16_FLOAT` Cubemap ($128 \times 128 \times 6$, 5 mips) |
| **4** | `CombinedImageSampler` | Fragment | `uBrdfLut` | `RGBA16_FLOAT` 2D Texture ($256 \times 256$) |

### UBO Lighting Parameters (`GpuLightingData.iblParams`)
- `iblParams.x`: $1.0\text{F}$ if IBL is active, $0.0\text{F}$ if inactive (fallback to uniform ambient).
- `iblParams.y`: Environment map intensity multiplier ($\text{default} = 1.0\text{F}$).
- `iblParams.z`: Maximum prefilter LOD mip index ($\text{default} = 4.0\text{F}$).
- `iblParams.w`: Unused padding.

---

## 4. Empirical Performance Verification: Native 1080p Sponza Benchmark

Measured via `./build/editor/tests/benchmark_sponza_1080p` on NVIDIA Pascal P106-100 across 9 worst-case camera scenarios (2,700 total frames) under High Quality Preset ($1920 \times 1080$ native, $2048 \times 2048$ shadow map, ACES tone mapping, full IBL split-sum sampling):

```
======================================================================
                 OVERALL 1080P BENCHMARK SUMMARY                      
======================================================================
Path: All 9 Scenarios Combined (2700 frames)
----------------------------------------------------------------------
Metric                     Value        Target         Status
----------------------------------------------------------------------
Average Frame Time:        1.823 ms   <= 16.67 ms      PASSED
Median Frame Time:         1.938 ms   <= 16.67 ms      PASSED
P95 Frame Time:            2.404 ms   <= 16.67 ms      PASSED
P99 Frame Time:            2.629 ms   <= 16.67 ms      PASSED
Min / Max Frame Time:      0.760 ms / 2.740 ms
Frame Time Std Dev:        0.386 ms
Average FPS:             548.591 FPS  >=  60.0 FPS     PASSED
P95 FPS Floor:           415.957 FPS  >=  60.0 FPS     PASSED
----------------------------------------------------------------------
Pass Breakdown (Averages):
  Shadow Pass:             0.061 ms ( 3.322%)
  Main PBR Pass (IBL):     1.137 ms (62.395%)
  Post-Processing:         0.601 ms (32.968%)
  CPU Recording Overhead:  0.780 ms
======================================================================
```

### Scenario Breakdown

| Scenario ID | Description | Avg Frame Time | Avg FPS | Main PBR Time | Triangles | Status |
| :---: | :--- | :---: | :---: | :---: | :---: | :---: |
| **1** | Central Atrium, dense geometry | 1.728 ms | 578.8 FPS | 1.090 ms | 101,415 | PASSED |
| **2** | Complex columns and architectural details | 2.008 ms | 498.0 FPS | 1.384 ms | 169,656 | PASSED |
| **3** | Dense overlapping geometry & overdraw | 1.953 ms | 512.0 FPS | 1.327 ms | 245,624 | PASSED |
| **4** | Upper balcony looking down into atrium | 1.566 ms | 638.5 FPS | 0.944 ms | 121,616 | PASSED |
| **5** | Long corridor with many visible submeshes | 1.540 ms | 649.4 FPS | 0.940 ms | 121,616 | PASSED |
| **6** | Close-up on archways and high-curvature meshes | 1.996 ms | 501.0 FPS | 1.373 ms | 150,115 | PASSED |
| **7** | Worst-case diagonal view, maximum geometry | 1.513 ms | 660.9 FPS | 0.887 ms | 254,705 | PASSED |
| **8** | Dynamic scene with moving shadow casters | 2.085 ms | 479.6 FPS | 1.100 ms | 351,212 | PASSED |
| **9** | Repeated camera movement across cache states | 2.016 ms | 495.9 FPS | 1.211 ms | 364,116 | PASSED |

**Performance Analysis:**
- The addition of 3 texture lookups in the PBR fragment shader (`uIrradianceMap`, `uPrefilteredEnvMap`, and `uBrdfLut`) incurs negligible GPU overhead ($1.137\text{ ms}$ main PBR duration at 1080p).
- Frame rate headroom remains greater than $8 \times$ the 60 FPS target on baseline Pascal hardware.

---

## 5. Automated Test Suite & Visual Verification

Automated regression and visual tests are consolidated in `tests/unit/render/ibl_render_test.cpp` and executed through CTest:

1. **`testIblToggleAndAmbientFallback` (PASSED):**
   - Validates that disabling IBL falls back to uniform flat ambient (`uLighting.ambientColor`).
   - Toggling IBL changes ambient illumination dynamically without artifacts.

2. **`testIblMetallicVsDielectricSplit` (PASSED):**
   - Validates energy conservation and Fresnel separation between conductors and dielectrics.
   - For dielectric surfaces ($\text{metallic} = 0$), red diffuse albedo correctly reflects diffuse irradiance ($\text{dieR} = 227 > \text{dieG} = 177$).
   - For pure metals ($\text{metallic} = 1.0$), diffuse reflection is completely excluded ($k_D = 0$), yielding flat specular reflection with no diffuse color tinting.

3. **`testRuntimeEnvironmentMapSwapping` (PASSED):**
   - Validates that environment maps can be created, uploaded, and swapped at runtime.
   - Swapping between different HDR environments dynamically updates readback pixel values without descriptor pool exhaustion or driver stalls.

4. **`testIblDamagedHelmetVisualCertification` (PASSED):**
   - Renders the Khronos `DamagedHelmet` model (16,149 rendered pixels) using full Cook-Torrance PBR and procedural atmospheric sky IBL.
   - Saved visual capture artifact: `test-assets/damaged_helmet_ibl_capture.ppm`.

### CTest Summary:
```
100% tests passed out of 52
Total Test time (real) = 57.20 sec
```

---

## 6. Conclusion & Milestone Status

Milestone 11C is **COMPLETE** and **CERTIFIED**. All requirements of the Image-Based Lighting pipeline, mathematical approximations, Vulkan descriptor set layouts, and runtime environment lifecycles have been verified on the NVIDIA Pascal P106-100 hardware floor.
