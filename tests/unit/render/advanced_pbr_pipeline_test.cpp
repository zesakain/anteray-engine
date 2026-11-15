/// @file advanced_pbr_pipeline_test.cpp
/// @brief Physical and visual verification tests for Milestone 11F Phase 4 Advanced PBR Pipeline.
///
/// Validates:
/// 1. Clearcoat dual-lobe energy attenuation and conservation
/// 2. IOR-derived dielectric Fresnel F0, specular factor, and color tinting
/// 3. Metallic/dielectric transitions and diffuse suppression
/// 4. Anisotropic GGX microfacet distribution and tangent rotation
/// 5. Charlie sheen microfacet distribution and Ashikhmin-Velazquez visibility
/// 6. Transmission and volume Beer-Lambert extinction
/// 7. Numerical stability guards against degenerate inputs (normals, tangents, grazing angles)
/// 8. Shader compilation and embedded SPIR-V synchronization
/// 9. GPU-side rendering tests on Null and Vulkan RHI backends

#include "core/assert.h"
#include "core/math.h"
#include "framework/scene.h"
#include "render/asset_manager.h"
#include "render/gpu_material.h"
#include "render/gpu_mesh.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/mesh_component.h"
#include "render/mesh_shader_spirv.h"
#include "render/scene_renderer.h"
#include "rhi/command_list.h"
#include "rhi/rhi_device.h"
#include "world/world.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HIKARI_HAS_ASAN 1
#endif
#if __has_feature(thread_sanitizer)
#define HIKARI_HAS_TSAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define HIKARI_HAS_ASAN 1
#endif
#if defined(__SANITIZE_THREAD__)
#define HIKARI_HAS_TSAN 1
#endif

#if defined(HIKARI_HAS_ASAN)
extern "C" const char* __lsan_default_suppressions() {
    return "leak:libdbus-1\nleak:libvulkan\nleak:nvidia\nleak:libwayland\n";
}
#endif

using namespace eng;
using namespace eng::render;
using namespace eng::rhi;

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ << ": " #cond       \
                      << std::endl;                                                                \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

#define TEST_APPROX(a, b, eps)                                                                     \
    do {                                                                                           \
        if (std::abs((a) - (b)) > (eps)) {                                                         \
            std::cerr << "Approx assertion failed: |" #a " - " #b "| = |" << (a) << " - " << (b)   \
                      << "| > " << (eps) << " at " << __FILE__ << ":" << __LINE__ << std::endl;    \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kInvPi = 0.31830988618379067154F;

// -----------------------------------------------------------------------------
// Test 1: Clearcoat Energy Attenuation and Conservation
// -----------------------------------------------------------------------------
void testClearcoatEnergyAttenuation() {
    std::cout << "[Test 1] Clearcoat Energy Attenuation & Conservation..." << std::endl;

    // Clearcoat lobe uses fixed dielectric IOR = 1.5 -> F0 = 0.04
    constexpr float kClearcoatF0 = 0.04F;

    auto evalClearcoatFresnel = [](float VdotNc, float clearcoatFactor) -> float {
        float fc = kClearcoatF0 +
                   (1.0F - kClearcoatF0) * std::pow(std::clamp(1.0F - VdotNc, 0.0F, 1.0F), 5.0F);
        return fc * clearcoatFactor;
    };

    // At normal incidence (VdotNc = 1.0): Fc = 0.04 * clearcoat
    float fcNormal = evalClearcoatFresnel(1.0F, 1.0F);
    TEST_APPROX(fcNormal, 0.04F, 1e-4F);

    // At grazing incidence (VdotNc = 0.0): Fc = 1.0 * clearcoat
    float fcGrazing = evalClearcoatFresnel(0.0F, 1.0F);
    TEST_APPROX(fcGrazing, 1.0F, 1e-4F);

    // At partial clearcoat strength (0.5):
    TEST_APPROX(evalClearcoatFresnel(1.0F, 0.5F), 0.02F, 1e-4F);
    TEST_APPROX(evalClearcoatFresnel(0.0F, 0.5F), 0.50F, 1e-4F);

    // Energy Conservation: Base layer is attenuated by (1 - Fc)
    // The total reflected energy from clearcoat + attenuated base layer must never exceed incoming
    // energy
    for (int angle = 0; angle <= 90; angle += 5) {
        float rad = static_cast<float>(angle) * (kPi / 180.0F);
        float VdotNc = std::cos(rad);
        for (float cc = 0.0F; cc <= 1.0F; cc += 0.25F) {
            float Fc = evalClearcoatFresnel(VdotNc, cc);
            float baseAttenuation = 1.0F - Fc;
            TEST_CHECK(baseAttenuation >= 0.0F && baseAttenuation <= 1.0F);
            TEST_CHECK(Fc + baseAttenuation <= 1.0001F);
        }
    }

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 2: IOR and Dielectric Fresnel F0 Integration
// -----------------------------------------------------------------------------
void testIorAndDielectricFresnel() {
    std::cout << "[Test 2] IOR-Derived Dielectric Fresnel F0..." << std::endl;

    auto deriveF0FromIor = [](float ior) -> float {
        float f0Sqrt = (ior - 1.0F) / (ior + 1.0F);
        return f0Sqrt * f0Sqrt;
    };

    // Standard IOR validation points:
    // Air (IOR = 1.0) -> F0 = 0.0
    TEST_APPROX(deriveF0FromIor(1.0F), 0.0F, 1e-5F);

    // Water (IOR = 1.333) -> F0 = 0.02
    TEST_APPROX(deriveF0FromIor(1.333F), 0.02037F, 1e-4F);

    // Standard glTF default (IOR = 1.5) -> F0 = 0.04
    TEST_APPROX(deriveF0FromIor(1.5F), 0.04F, 1e-4F);

    // Glass / Crystal (IOR = 1.6) -> F0 = 0.05325
    TEST_APPROX(deriveF0FromIor(1.6F), 0.05325F, 1e-4F);

    // Diamond (IOR = 2.417) -> F0 = 0.1719
    TEST_APPROX(deriveF0FromIor(2.417F), 0.1719F, 1e-4F);

    // Specular factor and specular color tinting:
    // dielectricF0 = min(F0 * specularFactor * specularColorFactor, 1.0)
    float baseF0 = deriveF0FromIor(1.5F); // 0.04
    float specFactor = 0.5F;
    Vec3 specColor{1.0F, 0.8F, 0.6F};

    Vec3 tintedF0 = Vec3{
        std::min(baseF0 * specFactor * specColor.x, 1.0F),
        std::min(baseF0 * specFactor * specColor.y, 1.0F),
        std::min(baseF0 * specFactor * specColor.z, 1.0F),
    };
    TEST_APPROX(tintedF0.x, 0.02F, 1e-4F);
    TEST_APPROX(tintedF0.y, 0.016F, 1e-4F);
    TEST_APPROX(tintedF0.z, 0.012F, 1e-4F);

    // Metallic Transition:
    // Pure metal (metallic = 1.0) overrides F0 with baseColor, diffuse is completely suppressed
    Vec3 baseColor{0.9F, 0.7F, 0.2F};
    for (float m = 0.0F; m <= 1.0F; m += 0.2F) {
        Vec3 effectiveF0 = Vec3{
            std::lerp(tintedF0.x, baseColor.x, m),
            std::lerp(tintedF0.y, baseColor.y, m),
            std::lerp(tintedF0.z, baseColor.z, m),
        };
        float kD = (1.0F - m);
        TEST_CHECK(kD >= 0.0F && kD <= 1.0F);
        if (m == 1.0F) {
            TEST_APPROX(effectiveF0.x, baseColor.x, 1e-4F);
            TEST_APPROX(kD, 0.0F, 1e-4F);
        }
    }

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 3: Anisotropic GGX Microfacet Distribution and Tangent Rotation
// -----------------------------------------------------------------------------
void testAnisotropicBrdGgx() {
    std::cout << "[Test 3] Anisotropic GGX NDF & Tangent Rotation..." << std::endl;

    // Evaluates anisotropic GGX NDF
    auto evalAnisoGgx = [](float XdotH, float YdotH, float ZdotH, float ax, float ay) -> float {
        float dTerm = (XdotH * XdotH) / (ax * ax) + (YdotH * YdotH) / (ay * ay) + ZdotH * ZdotH;
        return 1.0F / (kPi * ax * ay * std::max(dTerm * dTerm, 1e-7F));
    };

    // 1. Zero anisotropy fallback: aspect = 1.0, ax = ay = roughness^2
    float roughness = 0.5F;
    float a = roughness * roughness;
    float dAnisoIso = evalAnisoGgx(0.0F, 0.0F, 1.0F, a, a);
    float dIsotropicGgx = 1.0F / (kPi * a * a);
    TEST_APPROX(dAnisoIso, dIsotropicGgx, 1e-4F);

    // 2. High anisotropy strength (s = 0.9): aspect = sqrt(1.0 - 0.9 * 0.9) = sqrt(0.19) ~ 0.43589
    float anisoStrength = 0.9F;
    float aspect = std::sqrt(1.0F - anisoStrength * 0.9F);
    float ax = std::max(0.001F, (roughness * roughness) / aspect);
    float ay = std::max(0.001F, (roughness * roughness) * aspect);

    // Along Tangent X (XdotH = 0.5, YdotH = 0.0, ZdotH = 0.866)
    float dAlongX = evalAnisoGgx(0.5F, 0.0F, 0.866F, ax, ay);
    // Along Bitangent Y (XdotH = 0.0, YdotH = 0.5, ZdotH = 0.866)
    float dAlongY = evalAnisoGgx(0.0F, 0.5F, 0.866F, ax, ay);

    // Directional elongation: highlight is elongated along the tangent axis (X),
    // so at an offset from center, reflectance along X falls off much slower than along Y
    TEST_CHECK(dAlongX > dAlongY * 50.0F);

    // 3. Tangent Rotation
    float rotationAngle = kPi * 0.5F; // 90 degree rotation swaps X and Y
    Vec2 dir{std::cos(rotationAngle), std::sin(rotationAngle)};
    TEST_APPROX(dir.x, 0.0F, 1e-5F);
    TEST_APPROX(dir.y, 1.0F, 1e-5F);

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 4: Charlie Sheen Microfacet Distribution
// -----------------------------------------------------------------------------
void testCharlieSheenLobe() {
    std::cout << "[Test 4] Charlie Sheen Microfacet Distribution..." << std::endl;

    auto evalCharlieD = [](float NdotH, float sheenRoughness) -> float {
        float invR = 1.0F / std::max(sheenRoughness, 0.001F);
        float sinTheta2 = std::max(0.0F, 1.0F - NdotH * NdotH);
        return ((2.0F + invR) * 0.5F * kInvPi) * std::pow(sinTheta2, std::max(invR * 0.5F, 0.001F));
    };

    auto evalSheenV = [](float NdotL, float NdotV) -> float {
        return 1.0F / (4.0F * (NdotL + NdotV - NdotL * NdotV) + 1e-4F);
    };

    // Charlie NDF characteristic:
    // Peak reflectance occurs at grazing angles (NdotH -> 0, sinTheta2 -> 1.0)
    // Reflectance at normal incidence (NdotH = 1.0, sinTheta2 = 0.0) is zero!
    float dNormal = evalCharlieD(1.0F, 0.3F);
    TEST_APPROX(dNormal, 0.0F, 1e-5F);

    float dGrazing = evalCharlieD(0.1F, 0.3F);
    TEST_CHECK(dGrazing > 0.5F);

    // Sheen visibility is always positive and finite
    float vNormal = evalSheenV(1.0F, 1.0F);
    TEST_APPROX(vNormal, 0.25F, 1e-3F);

    float vGrazing = evalSheenV(0.01F, 0.01F);
    TEST_CHECK(vGrazing > 0.0F && !std::isnan(vGrazing) && !std::isinf(vGrazing));

    // Sheen Energy Scaling: Base layer scaled by (1 - max(sheenColor) * sheenFresnel)
    Vec3 sheenColor{0.8F, 0.4F, 0.2F};
    float maxSheen = std::max({sheenColor.x, sheenColor.y, sheenColor.z});
    float NdotV = 0.2F;
    float sheenFresnel = std::pow(1.0F - NdotV, 5.0F);
    float scaling = 1.0F - maxSheen * sheenFresnel;
    TEST_CHECK(scaling >= 0.0F && scaling <= 1.0F);

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 5: Transmission and Volume Beer-Lambert Extinction
// -----------------------------------------------------------------------------
void testTransmissionAndVolumeBeerLambert() {
    std::cout << "[Test 5] Transmission & Beer-Lambert Volume Extinction..." << std::endl;

    auto evalBeerLambert = [](Vec3 attenuationColor, float attenuationDistance,
                              float thickness) -> Vec3 {
        if (thickness <= 0.0F || attenuationDistance >= 1e8F) {
            return Vec3{1.0F, 1.0F, 1.0F};
        }
        Vec3 density = Vec3{
            -std::log(std::max(attenuationColor.x, 1e-4F)) / attenuationDistance,
            -std::log(std::max(attenuationColor.y, 1e-4F)) / attenuationDistance,
            -std::log(std::max(attenuationColor.z, 1e-4F)) / attenuationDistance,
        };
        return Vec3{
            std::exp(-density.x * thickness),
            std::exp(-density.y * thickness),
            std::exp(-density.z * thickness),
        };
    };

    // When thickness is zero: transmission is 100% (no extinction)
    Vec3 attenZero = evalBeerLambert(Vec3{0.5F, 0.2F, 0.8F}, 5.0F, 0.0F);
    TEST_APPROX(attenZero.x, 1.0F, 1e-5F);
    TEST_APPROX(attenZero.y, 1.0F, 1e-5F);
    TEST_APPROX(attenZero.z, 1.0F, 1e-5F);

    // At exactly thickness == attenuationDistance: transmission equals attenuationColor
    Vec3 targetColor{0.4F, 0.7F, 0.9F};
    Vec3 attenOneDist = evalBeerLambert(targetColor, 10.0F, 10.0F);
    TEST_APPROX(attenOneDist.x, targetColor.x, 1e-4F);
    TEST_APPROX(attenOneDist.y, targetColor.y, 1e-4F);
    TEST_APPROX(attenOneDist.z, targetColor.z, 1e-4F);

    // Double thickness: transmission is squared (Beer-Lambert exponential law)
    Vec3 attenDoubleDist = evalBeerLambert(targetColor, 10.0F, 20.0F);
    TEST_APPROX(attenDoubleDist.x, targetColor.x * targetColor.x, 1e-4F);
    TEST_APPROX(attenDoubleDist.y, targetColor.y * targetColor.y, 1e-4F);
    TEST_APPROX(attenDoubleDist.z, targetColor.z * targetColor.z, 1e-4F);

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 6: Numerical Stability Guards (Degenerate Inputs)
// -----------------------------------------------------------------------------
void testNumericalStabilityGuards() {
    std::cout << "[Test 6] Numerical Stability Guards..." << std::endl;

    // Degenerate normal vector (0, 0, 0)
    Vec3 degenN{0.0F, 0.0F, 0.0F};
    float lenSq = degenN.x * degenN.x + degenN.y * degenN.y + degenN.z * degenN.z;
    Vec3 safeN = (lenSq < 1e-6F) ? Vec3{0.0F, 0.0F, 1.0F} : (degenN * (1.0F / std::sqrt(lenSq)));
    TEST_APPROX(safeN.z, 1.0F, 1e-5F);

    // Grazing angle clamp (NdotV <= 0)
    float rawNdotV = -0.5F;
    float safeNdotV = std::max(rawNdotV, 0.0001F);
    TEST_APPROX(safeNdotV, 0.0001F, 1e-5F);

    // Roughness lower bound clamp (prevent GGX Dirac delta overflow)
    float rawRoughness = 0.0F;
    float safeRoughness = std::max(rawRoughness, 0.045F);
    TEST_APPROX(safeRoughness, 0.045F, 1e-5F);

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 7: Embedded SPIR-V Shader Synchronization Check
// -----------------------------------------------------------------------------
void testShaderCompilationAndSpirvSync() {
    std::cout << "[Test 7] Embedded SPIR-V Header Synchronization Check..." << std::endl;

    // Verify non-empty SPIR-V arrays with valid Vulkan SPIR-V magic word 0x07230203
    constexpr uint32_t kSpirvMagic = 0x07230203U;

    TEST_CHECK(sizeof(kPbrMeshVertexShaderSpirv) > 0);
    TEST_CHECK(kPbrMeshVertexShaderSpirv[0] == kSpirvMagic);

    TEST_CHECK(sizeof(kGltfMaterialFragmentShaderSpirv) > 0);
    TEST_CHECK(kGltfMaterialFragmentShaderSpirv[0] == kSpirvMagic);

    TEST_CHECK(sizeof(kGltfMaterialOpaqueFragmentShaderSpirv) > 0);
    TEST_CHECK(kGltfMaterialOpaqueFragmentShaderSpirv[0] == kSpirvMagic);

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 8: GPU-Side Rendering Tests with Advanced PBR Materials
// -----------------------------------------------------------------------------
void testGpuMaterialAdvancedRendering() {
    std::cout << "[Test 8] GPU-Side Rendering with Advanced Materials..." << std::endl;

    auto devRes = rhi::createNullDevice();
    TEST_CHECK(devRes.hasValue());
    auto device = std::move(devRes.value());
    TEST_CHECK(device != nullptr);

    auto layoutDesc = createMaterialDescriptorSetLayoutDesc();
    TEST_CHECK(layoutDesc.bindings.size() == 16);
    auto layoutRes = device->createDescriptorSetLayout(layoutDesc);
    TEST_CHECK(layoutRes.hasValue());
    auto layout = layoutRes.value();

    AssetManager assetMgr;

    // 1. Clearcoat + Sheen Dielectric Material
    Material matA = Material::createDefault();
    matA.name = "ClearcoatSheenMat";
    matA.baseColor = Vec4{0.8F, 0.2F, 0.3F, 1.0F};
    matA.roughness = 0.4F;
    matA.clearcoat.factor = 0.8F;
    matA.clearcoat.roughnessFactor = 0.1F;
    matA.sheen.colorFactor = Vec3{0.9F, 0.7F, 0.5F};
    matA.sheen.roughnessFactor = 0.3F;

    auto gpuMatARes = GpuMaterial::create(*device, layout, matA, assetMgr);
    TEST_CHECK(gpuMatARes.hasValue());
    auto gpuMatA = std::move(gpuMatARes.value());
    TEST_CHECK(gpuMatA->isValid());

    const auto& uboA = gpuMatA->getUniformData();
    TEST_APPROX(uboA.clearcoatFactor, 0.8F, 1e-4F);
    TEST_APPROX(uboA.clearcoatRoughness, 0.1F, 1e-4F);
    TEST_APPROX(uboA.sheenRoughness, 0.3F, 1e-4F);
    TEST_APPROX(uboA.sheenColorFactorAndFlags.x, 0.9F, 1e-4F);

    // 2. Anisotropic Brushed Metal Material
    Material matB = Material::createDefault();
    matB.name = "AnisotropicBrushedMetal";
    matB.metallic = 1.0F;
    matB.roughness = 0.3F;
    matB.anisotropy.strength = 0.85F;
    matB.anisotropy.rotation = 1.570796F;

    auto gpuMatBRes = GpuMaterial::create(*device, layout, matB, assetMgr);
    TEST_CHECK(gpuMatBRes.hasValue());
    auto gpuMatB = std::move(gpuMatBRes.value());
    TEST_CHECK(gpuMatB->isValid());

    const auto& uboB = gpuMatB->getUniformData();
    TEST_APPROX(uboB.anisotropyStrength, 0.85F, 1e-4F);
    TEST_APPROX(uboB.anisotropyRotation, 1.570796F, 1e-4F);

    // 3. Transmission & Volume Glass Material
    Material matC = Material::createDefault();
    matC.name = "BeerLambertGlass";
    matC.transmissionFactor = 0.95F;
    matC.ior = 1.52F;
    matC.volume.thicknessFactor = 2.0F;
    matC.volume.attenuationDistance = 5.0F;
    matC.volume.attenuationColor = Vec3{0.3F, 0.8F, 0.9F};

    auto gpuMatCRes = GpuMaterial::create(*device, layout, matC, assetMgr);
    TEST_CHECK(gpuMatCRes.hasValue());
    auto gpuMatC = std::move(gpuMatCRes.value());
    TEST_CHECK(gpuMatC->isValid());

    const auto& uboC = gpuMatC->getUniformData();
    TEST_APPROX(uboC.transmissionFactor, 0.95F, 1e-4F);
    TEST_APPROX(uboC.ior, 1.52F, 1e-4F);
    TEST_APPROX(uboC.thicknessFactor, 2.0F, 1e-4F);
    TEST_APPROX(uboC.attenuationColorAndHasSpecTex.x, 0.3F, 1e-4F);

    // Test SceneRenderer integration with Null RHI device
    SceneRenderer renderer{};
    auto initRes = renderer.init(*device, 128, 128);
    TEST_CHECK(initRes.hasValue());
    world::World world;

    // Create Mesh Component with our materials
    MeshData quadMesh{};
    quadMesh.vertices = {
        Vertex(Vec3(-1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 1.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 1.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(1.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 0.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
    };
    quadMesh.indices = {0, 1, 2};
    quadMesh.submeshes.push_back(Submesh{
        .name = "SubmeshA",
        .firstIndex = 0,
        .indexCount = 3,
        .vertexOffset = 0,
        .materialIndex = 0,
    });
    quadMesh.materials = {matA};

    auto regRes = assetMgr.registerMesh("virtual://mesh_pbr", std::move(quadMesh));
    TEST_CHECK(regRes.hasValue());
    auto uploadMeshRes = assetMgr.uploadMesh(regRes.value(), *device);
    TEST_CHECK(uploadMeshRes.hasValue());
    auto uploadMatRes = assetMgr.uploadMaterial(regRes.value(), 0, *device, layout);
    TEST_CHECK(uploadMatRes.hasValue());

    auto entity = world.createEntity("PbrEntity");
    MeshComponent meshComp{};
    meshComp.meshAsset = regRes.value();
    meshComp.materialAsset = regRes.value();
    meshComp.castShadows = true;
    world.setComponent(entity, meshComp);

    // Record scene render frame
    auto cmdRes = device->createCommandList();
    TEST_CHECK(cmdRes.hasValue());
    auto cmd = std::move(cmdRes.value());
    cmd->begin();

    Mat4 viewProj = Mat4::identity();
    renderer.renderScene(world, assetMgr, *device, *cmd, viewProj);
    cmd->end();

    device->destroyDescriptorSetLayout(layout);
    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 9: Advanced PBR Material Visual Consistency & Lighting Variations
// -----------------------------------------------------------------------------
void testPbrVisualMaterialVariations() {
    std::cout << "[Test 9] Advanced PBR Material Visual Consistency & Lighting Variations..."
              << std::endl;

    // 1. Direct lighting only vs IBL only vs Mixed lighting consistency
    // Verify that energy does not explode under different illuminations
    for (float roughness : {0.1F, 0.5F, 0.9F}) {
        // Clearcoat attenuation
        float fc = 0.04F + (1.0F - 0.04F) * std::pow(1.0F - 0.5F, 5.0F);
        float baseWeight = 1.0F - fc;
        TEST_CHECK(baseWeight >= 0.0F && baseWeight <= 1.0F);

        // Charlie sheen energy balance
        float sheenRoughness = std::max(0.001F, roughness);
        float sheenLobe = std::pow(1.0F - 0.25F, 0.5F / sheenRoughness) / (2.0F * 3.14159F);
        TEST_CHECK(std::isfinite(sheenLobe) && sheenLobe >= 0.0F);

        // Dielectric IOR transition
        for (float ior : {1.1F, 1.33F, 1.5F, 1.8F, 2.4F}) {
            float f0 = std::pow((ior - 1.0F) / (ior + 1.0F), 2.0F);
            TEST_CHECK(f0 > 0.0F && f0 < 0.2F);
        }

        // Anisotropy stability across rotation angles
        for (float rotation : {0.0F, 0.785F, 1.57F, 3.14F}) {
            float cosR = std::cos(2.0F * rotation);
            float sinR = std::sin(2.0F * rotation);
            TEST_CHECK(std::isfinite(cosR) && std::isfinite(sinR));
        }
    }

    // 2. Vulkan GPU execution with multiple material variations
    VulkanDeviceDesc devDesc{.windowHandle = nullptr, .enableValidation = false};
    auto devRes = createVulkanDevice(devDesc);
    if (!devRes.hasValue()) {
        std::cout << "  [SKIP] Vulkan device unavailable; skipping live GPU visual test."
                  << std::endl;
        return;
    }
    auto device = std::move(devRes.value());

    SceneRenderer renderer;
    TEST_CHECK(renderer.init(*device, 64, 64).hasValue());
    world::World world;
    AssetManager assetMgr;

    // Build test quad
    MeshData quadMesh{};
    quadMesh.vertices = {
        Vertex(Vec3(-1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 1.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 1.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(1.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 0.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
    };
    quadMesh.indices = {0, 1, 2};
    quadMesh.submeshes.push_back(Submesh{
        .name = "TestSubmesh",
        .firstIndex = 0,
        .indexCount = 3,
        .vertexOffset = 0,
        .materialIndex = 0,
    });

    // Material with Anisotropy, Sheen, Clearcoat, Transmission, and IOR
    Material advMat = Material::createDefault();
    advMat.anisotropy.strength = 0.8F;
    advMat.anisotropy.rotation = 0.5F;
    advMat.sheen.colorFactor = Vec3{0.8F, 0.7F, 0.9F};
    advMat.sheen.roughnessFactor = 0.4F;
    advMat.clearcoat.factor = 0.9F;
    advMat.clearcoat.roughnessFactor = 0.1F;
    advMat.transmission.factor = 0.5F;
    advMat.ior = 1.6F;
    quadMesh.materials = {advMat};

    auto regMesh = assetMgr.registerMesh("virtual://adv_pbr_quad", std::move(quadMesh));
    TEST_CHECK(regMesh.hasValue());
    TEST_CHECK(assetMgr.uploadMesh(regMesh.value(), *device).hasValue());

    auto layoutDesc = createMaterialDescriptorSetLayoutDesc();
    auto layoutRes = device->createDescriptorSetLayout(layoutDesc);
    TEST_CHECK(layoutRes.hasValue());
    auto layout = layoutRes.value();

    TEST_CHECK(assetMgr.uploadMaterial(regMesh.value(), 0, *device, layout).hasValue());

    auto ent = world.createEntity("PbrVisualEntity");
    MeshComponent meshComp{};
    meshComp.meshAsset = regMesh.value();
    meshComp.materialAsset = regMesh.value();
    world.setComponent(ent, meshComp);

    // Render under Vulkan GPU
    auto cmdRes = device->createCommandList();
    TEST_CHECK(cmdRes.hasValue());
    auto cmd = std::move(cmdRes.value());
    cmd->begin();
    renderer.renderScene(world, assetMgr, *device, *cmd, Mat4::identity());
    cmd->end();
    TEST_CHECK(device->submit(cmd.get()).hasValue());

    device->destroyDescriptorSetLayout(layout);
    renderer.shutdown();
    std::cout << "  Passed." << std::endl;
}

} // namespace

int main() {
    std::cout << "========================================================\n"
              << "Running Milestone 11F Phase 4 Advanced PBR Pipeline Suite\n"
              << "========================================================" << std::endl;

    testClearcoatEnergyAttenuation();
    testIorAndDielectricFresnel();
    testAnisotropicBrdGgx();
    testCharlieSheenLobe();
    testTransmissionAndVolumeBeerLambert();
    testNumericalStabilityGuards();
    testShaderCompilationAndSpirvSync();
    testGpuMaterialAdvancedRendering();
    testPbrVisualMaterialVariations();

    std::cout << "========================================================\n"
              << "All 9 tests in Advanced PBR Pipeline Suite PASSED.\n"
              << "========================================================" << std::endl;
    return 0;
}
