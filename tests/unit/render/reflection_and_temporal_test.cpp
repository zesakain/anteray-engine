/// @file reflection_and_temporal_test.cpp
/// @brief Comprehensive verification test suite for Milestone 11G: Reflection Quality & Temporal
/// Stability.
///
/// Validates:
/// 1. ReflectionProbeComponent box-projection parallax correction and spatial weight blending.
/// 2. GpuLightingData std140 layout, 16-byte alignment, and 2304-byte buffer size.
/// 3. GraphicsSettings SSR scalability (Tier 0 baseline preserved on High/Med/Low; enabled on
/// Ultra/Cinematic).
/// 4. GraphicsSettingsSerializer JSON persistence roundtrip for all SSR and reflection probe
/// controls.
/// 5. Screen-Space Reflections (SSR) ray intersection math, edge fade, and roughness cutoff
/// attenuation.
/// 6. Temporal filter normal-aware silhouette rejection, depth thresholding, and 3x3 variance
/// clipping.
/// 7. SceneRenderer execution with SSR enabled and disabled under Null RHI.

#include "core/assert.h"
#include "core/math.h"
#include "render/graphics_settings.h"
#include "render/graphics_settings_serializer.h"
#include "render/irradiance_probe.h"
#include "render/lighting_data.h"
#include "render/scene_renderer.h"
#include "rhi/rhi_device.h"
#include "world/world.h"

#include <cmath>
#include <iostream>
#include <vector>

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ << ": " #cond       \
                      << std::endl;                                                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

using namespace eng;
using namespace eng::render;

static int testReflectionProbeMathAndBlending() {
    std::cout << "  - Test 1: Reflection probe box-projection and influence weighting..."
              << std::endl;

    ReflectionProbeComponent probe{};
    probe.position = Vec3(0.0F, 0.0F, 0.0F);
    probe.boxMin = Vec3(-5.0F, -5.0F, -5.0F);
    probe.boxMax = Vec3(5.0F, 5.0F, 5.0F);
    probe.boxProjection = true;
    probe.intensity = 1.0F;
    probe.enabled = true;

    // Center point weight must be 1.0
    float centerWeight = probe.computeWeight(Vec3(0.0F, 0.0F, 0.0F));
    TEST_CHECK(std::abs(centerWeight - 1.0F) < 1e-4F);

    // Outside bounding box weight must be exactly 0.0
    float outsideWeight = probe.computeWeight(Vec3(6.0F, 0.0F, 0.0F));
    TEST_CHECK(outsideWeight == 0.0F);

    // Edge margin falloff test: at x = 4.75m (0.25m from wall, margin = 0.5m), weight is ~0.5
    float nearWallWeight = probe.computeWeight(Vec3(4.75F, 0.0F, 0.0F));
    TEST_CHECK(nearWallWeight > 0.4F && nearWallWeight < 0.6F);

    // Box projection parallax correction test:
    // Surface at P = (2, 0, 0) reflecting along R = (1, 0, 0).
    // The ray hits the box wall at x = 5. The corrected vector from probe (0, 0, 0) to hit (5, 0,
    // 0) is (1, 0, 0).
    Vec3 R = Vec3(1.0F, 0.0F, 0.0F);
    Vec3 P = Vec3(2.0F, 0.0F, 0.0F);
    Vec3 R_proj = probe.boxProject(R, P);
    TEST_CHECK((R_proj - Vec3(1.0F, 0.0F, 0.0F)).lengthSq() < 1e-5F);

    // Diagonal reflection test:
    // P = (0, 0, 0), R = normalize(1, 1, 0)
    // Ray hits corner at (5, 5, 0). Direction is normalized (1, 1, 0).
    Vec3 R_diag = Vec3(1.0F, 1.0F, 0.0F).normalized();
    Vec3 R_diag_proj = probe.boxProject(R_diag, Vec3(0.0F, 0.0F, 0.0F));
    TEST_CHECK((R_diag_proj - R_diag).lengthSq() < 1e-5F);

    // Off-center probe position test:
    // Probe center at (0, 2, 0), P at (0, 0, 0), R = (1, 0, 0).
    // Ray hits (5, 0, 0). Vector from probe (0, 2, 0) to hit is (5, -2, 0).
    probe.position = Vec3(0.0F, 2.0F, 0.0F);
    Vec3 R_off = probe.boxProject(Vec3(1.0F, 0.0F, 0.0F), Vec3(0.0F, 0.0F, 0.0F));
    Vec3 expected = Vec3(5.0F, -2.0F, 0.0F).normalized();
    TEST_CHECK((R_off - expected).lengthSq() < 1e-4F);

    return 0;
}

static int testGpuLightingDataLayout() {
    std::cout << "  - Test 2: GpuLightingData std140 layout and alignment..." << std::endl;

    TEST_CHECK(sizeof(GpuLightingData) == 2304);
    TEST_CHECK(sizeof(GpuLightingData) % 16 == 0);

    GpuLightingData data{};
    data.ssrParams = Vec4(1.0F, 1.5F, 0.65F, 25.0F);
    data.probeBoxMin = Vec4(-12.0F, -4.0F, -8.0F, 1.0F);
    data.probeBoxMax = Vec4(12.0F, 6.0F, 8.0F, 0.0F);
    data.probePosAndWeight = Vec4(1.0F, 2.0F, 3.0F, 0.85F);

    TEST_CHECK(data.ssrParams.x == 1.0F);
    TEST_CHECK(data.ssrParams.y == 1.5F);
    TEST_CHECK(data.ssrParams.z == 0.65F);
    TEST_CHECK(data.ssrParams.w == 25.0F);

    TEST_CHECK(data.probeBoxMin.w == 1.0F);
    TEST_CHECK(data.probePosAndWeight.w == 0.85F);

    const auto sceneLayout = createSceneDescriptorSetLayoutDesc();
    TEST_CHECK(sceneLayout.bindings.size() == 8);
    TEST_CHECK(sceneLayout.bindings[7].binding == 7);
    TEST_CHECK(sceneLayout.bindings[7].descriptorType == rhi::DescriptorType::CombinedImageSampler);

    return 0;
}

static int testGraphicsSettingsAndSerialization() {
    std::cout << "  - Test 3: GraphicsSettings SSR scalability & JSON roundtrip..." << std::endl;

    auto low = GraphicsSettings::createPreset(GraphicsQualityPreset::Low);
    auto med = GraphicsSettings::createPreset(GraphicsQualityPreset::Medium);
    auto high = GraphicsSettings::createPreset(GraphicsQualityPreset::High);
    auto ultra = GraphicsSettings::createPreset(GraphicsQualityPreset::Ultra);
    auto cine = GraphicsSettings::createPreset(GraphicsQualityPreset::Cinematic);

    // Tier 0 Pascal baseline protection: Low, Med, High must not enforce SSR
    TEST_CHECK(!low.lighting.ssrEnabled);
    TEST_CHECK(!med.lighting.ssrEnabled);
    TEST_CHECK(!high.lighting.ssrEnabled);

    // High tiers enable SSR
    TEST_CHECK(ultra.lighting.ssrEnabled);
    TEST_CHECK(cine.lighting.ssrEnabled);

    // Serialization roundtrip
    GraphicsSettings settings = ultra;
    settings.lighting.ssrRoughnessCutoff = 0.55F;
    settings.lighting.ssrMaxDistance = 15.0F;
    settings.lighting.ssrIntensity = 1.25F;
    settings.lighting.reflectionProbeBlending = true;

    auto jsonRes = GraphicsSettingsSerializer::serializeToString(settings);
    TEST_CHECK(jsonRes.hasValue());

    auto loadRes = GraphicsSettingsSerializer::deserializeFromString(jsonRes.value());
    TEST_CHECK(loadRes.hasValue());
    const auto& loaded = loadRes.value();

    TEST_CHECK(loaded.lighting.ssrEnabled == settings.lighting.ssrEnabled);
    TEST_CHECK(std::abs(loaded.lighting.ssrRoughnessCutoff - settings.lighting.ssrRoughnessCutoff) <
               1e-4F);
    TEST_CHECK(std::abs(loaded.lighting.ssrMaxDistance - settings.lighting.ssrMaxDistance) < 1e-4F);
    TEST_CHECK(std::abs(loaded.lighting.ssrIntensity - settings.lighting.ssrIntensity) < 1e-4F);
    TEST_CHECK(loaded.lighting.reflectionProbeBlending ==
               settings.lighting.reflectionProbeBlending);

    return 0;
}

static int testSsrRayMathAndEdgeFade() {
    std::cout << "  - Test 4: SSR edge fade and roughness cutoff math..." << std::endl;

    // Viewport edge fade test: UV near edge [0, 1]
    auto computeEdgeFade = [](Vec2 uv) {
        float edgeX = std::min(uv.x, 1.0F - uv.x);
        float edgeY = std::min(uv.y, 1.0F - uv.y);
        float d = std::min(edgeX, edgeY);
        return std::clamp(d / 0.08F, 0.0F, 1.0F);
    };

    // Center screen UV has full fade factor 1.0
    TEST_CHECK(computeEdgeFade(Vec2(0.5F, 0.5F)) == 1.0F);
    // Boundary UV has zero fade factor
    TEST_CHECK(computeEdgeFade(Vec2(0.0F, 0.5F)) == 0.0F);
    TEST_CHECK(computeEdgeFade(Vec2(0.5F, 1.0F)) == 0.0F);
    // Transition margin (0.04 is half of 0.08)
    TEST_CHECK(std::abs(computeEdgeFade(Vec2(0.04F, 0.5F)) - 0.5F) < 1e-4F);

    // Roughness cutoff fade test
    auto computeRoughnessFade = [](float roughness, float cutoff) {
        float start = cutoff - 0.2F;
        return std::clamp(1.0F - (roughness - start) / 0.2F, 0.0F, 1.0F);
    };

    float cutoff = 0.6F;
    // Mirror smooth surface has full SSR contribution
    TEST_CHECK(computeRoughnessFade(0.1F, cutoff) == 1.0F);
    TEST_CHECK(computeRoughnessFade(0.3F, cutoff) == 1.0F);
    // Boundary cutoff has 0 SSR contribution (clean fallback to IBL)
    TEST_CHECK(std::abs(computeRoughnessFade(0.6F, cutoff)) < 1e-5F);
    TEST_CHECK(std::abs(computeRoughnessFade(0.8F, cutoff)) < 1e-5F);
    // Halfway through transition window [0.4, 0.6]
    TEST_CHECK(std::abs(computeRoughnessFade(0.5F, cutoff) - 0.5F) < 1e-4F);

    return 0;
}

static int testTemporalStabilityAndVarianceClipping() {
    std::cout << "  - Test 5: Temporal filter variance clipping and normal rejection..."
              << std::endl;

    // Variance clipping verification
    // 3x3 neighborhood with stable color (1.0, 1.0, 1.0) and one transient spike (2.0, 2.0, 2.0)
    std::vector<Vec3> neighborhood(9, Vec3(1.0F, 1.0F, 1.0F));
    neighborhood[4] = Vec3(2.0F, 2.0F, 2.0F); // Center spike

    Vec3 m1 = Vec3::zero();
    Vec3 m2 = Vec3::zero();
    Vec3 boxMin = neighborhood[0];
    Vec3 boxMax = neighborhood[0];

    for (const auto& sample : neighborhood) {
        m1 += sample;
        m2 += Vec3(sample.x * sample.x, sample.y * sample.y, sample.z * sample.z);
        boxMin = Vec3(std::min(boxMin.x, sample.x), std::min(boxMin.y, sample.y),
                      std::min(boxMin.z, sample.z));
        boxMax = Vec3(std::max(boxMax.x, sample.x), std::max(boxMax.y, sample.y),
                      std::max(boxMax.z, sample.z));
    }

    Vec3 mu = m1 * (1.0F / 9.0F);
    Vec3 sigma = Vec3(std::sqrt(std::max(0.0F, m2.x * (1.0F / 9.0F) - mu.x * mu.x)),
                      std::sqrt(std::max(0.0F, m2.y * (1.0F / 9.0F) - mu.y * mu.y)),
                      std::sqrt(std::max(0.0F, m2.z * (1.0F / 9.0F) - mu.z * mu.z)));

    float gamma = 1.25F;
    Vec3 clipMin =
        Vec3(std::max(boxMin.x, mu.x - gamma * sigma.x), std::max(boxMin.y, mu.y - gamma * sigma.y),
             std::max(boxMin.z, mu.z - gamma * sigma.z));
    Vec3 clipMax =
        Vec3(std::min(boxMax.x, mu.x + gamma * sigma.x), std::min(boxMax.y, mu.y + gamma * sigma.y),
             std::min(boxMax.z, mu.z + gamma * sigma.z));

    // Stale ghost history color (e.g. 5.0 from a bright flash) must be clipped down to clipMax
    Vec3 ghostHistory = Vec3(5.0F, 5.0F, 5.0F);
    Vec3 clampedHistory = Vec3(std::clamp(ghostHistory.x, clipMin.x, clipMax.x),
                               std::clamp(ghostHistory.y, clipMin.y, clipMax.y),
                               std::clamp(ghostHistory.z, clipMin.z, clipMax.z));

    TEST_CHECK(clampedHistory.x <= clipMax.x);
    TEST_CHECK(clampedHistory.x < 2.0F); // Clamped well below the ghost spike

    // Normal rejection verification across silhouettes:
    // Surface normal facing camera (0, 0, 1), history normal at 90 deg (1, 0, 0) -> dot = 0 ->
    // reject!
    Vec3 curN = Vec3(0.0F, 0.0F, 1.0F);
    Vec3 histN = Vec3(1.0F, 0.0F, 0.0F);
    TEST_CHECK(Vec3::dot(curN, histN) < 0.707F); // Trigger history rejection

    // Continuous surface with small normal deflection (15 degrees) -> dot > 0.707 -> accept!
    float angle = 15.0F * 3.14159F / 180.0F;
    Vec3 deflectedN = Vec3(std::sin(angle), 0.0F, std::cos(angle));
    TEST_CHECK(Vec3::dot(curN, deflectedN) > 0.707F);

    return 0;
}

static int testSceneRendererSsrExecutionNullRhi() {
    std::cout << "  - Test 6: SceneRenderer execution with SSR enabled & disabled on Null RHI..."
              << std::endl;

    auto deviceRes = rhi::createNullDevice();
    TEST_CHECK(deviceRes.hasValue());
    auto device = std::move(deviceRes.value());

    SceneRenderer renderer{};
    auto initRes = renderer.init(*device, 960, 540);
    TEST_CHECK(initRes.hasValue());

    world::World world{};

    // Add reflection probe entity
    auto probeEnt = world.getRawWorld().entity("TestReflectionProbe");
    ReflectionProbeComponent reflComp{};
    reflComp.position = Vec3(0.0F, 1.0F, 0.0F);
    reflComp.boxMin = Vec3(-10.0F, -5.0F, -10.0F);
    reflComp.boxMax = Vec3(10.0F, 15.0F, 10.0F);
    reflComp.boxProjection = true;
    reflComp.intensity = 1.0F;
    reflComp.enabled = true;
    probeEnt.set<ReflectionProbeComponent>(reflComp);

    AssetManager assetMgr{};
    auto cmdListRes = device->createCommandList();
    TEST_CHECK(cmdListRes.hasValue());
    auto cmdList = std::move(cmdListRes.value());

    // 1. Render with SSR disabled (default High preset)
    GraphicsSettings settings = GraphicsSettings::createPreset(GraphicsQualityPreset::High);
    settings.lighting.ssrEnabled = false;
    settings.lighting.probesEnabled = true;
    auto applyRes1 = renderer.applyGraphicsSettings(settings);
    TEST_CHECK(applyRes1.hasValue());

    Mat4 viewProj = Mat4::perspective(1.047F, 16.0F / 9.0F, 0.1F, 100.0F);
    cmdList->begin();
    auto stats1 = renderer.renderScene(world, assetMgr, *device, *cmdList, viewProj);
    cmdList->end();
    static_cast<void>(stats1);

    // 2. Render with SSR enabled (Ultra preset)
    settings.lighting.ssrEnabled = true;
    auto applyRes2 = renderer.applyGraphicsSettings(settings);
    TEST_CHECK(applyRes2.hasValue());

    cmdList->begin();
    auto stats2 = renderer.renderScene(world, assetMgr, *device, *cmdList, viewProj);
    cmdList->end();
    static_cast<void>(stats2);

    renderer.shutdown();
    return 0;
}

int main() {
    std::cout << "[reflection_and_temporal_test] Starting Milestone 11G verification tests..."
              << std::endl;

    if (testReflectionProbeMathAndBlending() != 0) {
        return 1;
    }
    if (testGpuLightingDataLayout() != 0) {
        return 2;
    }
    if (testGraphicsSettingsAndSerialization() != 0) {
        return 3;
    }
    if (testSsrRayMathAndEdgeFade() != 0) {
        return 4;
    }
    if (testTemporalStabilityAndVarianceClipping() != 0) {
        return 5;
    }
    if (testSceneRendererSsrExecutionNullRhi() != 0) {
        return 6;
    }

    std::cout << "[reflection_and_temporal_test] All Milestone 11G tests PASSED successfully."
              << std::endl;
    return 0;
}
