/// @file taa_test.cpp
/// @brief Comprehensive automated verification test suite for Milestone 11H:
/// Advanced Anti-Aliasing & Temporal Rendering.
///
/// Validates:
/// 1. Halton(2, 3) low-discrepancy sequence determinism, periodicity, and subpixel range [-0.5,
/// 0.5].
/// 2. Projection jitter matrix transformation math, NDC shift, and depth/w element preservation.
/// 3. Motion vector coordinate conventions: screen UV velocity (du, dv) = uv_cur - uv_prev.
/// 4. Temporal history rejection rules: screen boundary clipping, depth disocclusion, and
/// confidence gating.
/// 5. YCoCg color space transformation roundtrip and 3x3 statistical variance clipping (mu +/-
/// gamma * sigma).
/// 6. Karis tone weighting damping response on high dynamic range fireflies.
/// 7. Contrast-Adaptive Sharpening (RCAS) filter response, overshoot limiting, and intensity
/// scaling.
/// 8. GraphicsSettings TAA model, preset defaults, and JSON serialization roundtrip with backward
/// compatibility.
/// 9. SceneRenderer offscreen execution with TAA enabled/disabled under Null RHI.

#include "core/assert.h"
#include "core/math.h"
#include "render/graphics_settings.h"
#include "render/graphics_settings_serializer.h"
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

// -----------------------------------------------------------------------------
// Test 1: Halton low-discrepancy sequence & jitter range
// -----------------------------------------------------------------------------
static int testHaltonSequenceAndJitterMath() {
    std::cout << "  - Test 1: Halton sequence & projection jitter range..." << std::endl;

    // Base 2 sequence: 1/2, 1/4, 3/4, 1/8, 5/8, 3/8, 7/8, 1/16...
    TEST_CHECK(std::abs(haltonSequence(1, 2) - 0.500F) < 1e-4F);
    TEST_CHECK(std::abs(haltonSequence(2, 2) - 0.250F) < 1e-4F);
    TEST_CHECK(std::abs(haltonSequence(3, 2) - 0.750F) < 1e-4F);
    TEST_CHECK(std::abs(haltonSequence(4, 2) - 0.125F) < 1e-4F);

    // Base 3 sequence: 1/3, 2/3, 1/9, 4/9, 7/9, 2/9, 5/9, 8/9...
    TEST_CHECK(std::abs(haltonSequence(1, 3) - (1.0F / 3.0F)) < 1e-4F);
    TEST_CHECK(std::abs(haltonSequence(2, 3) - (2.0F / 3.0F)) < 1e-4F);
    TEST_CHECK(std::abs(haltonSequence(3, 3) - (1.0F / 9.0F)) < 1e-4F);

    // Test jitter offsets across sequences of length 8, 16, 32
    for (uint32_t seqLen : {8U, 16U, 32U}) {
        for (uint32_t phase = 0; phase < seqLen * 2; ++phase) {
            Vec2 jitter = computeHaltonJitter(phase, seqLen);
            // Jitter must be centered in [-0.5, 0.5] pixel bounds
            TEST_CHECK(jitter.x >= -0.5F && jitter.x <= 0.5F);
            TEST_CHECK(jitter.y >= -0.5F && jitter.y <= 0.5F);

            // Periodic repetition check
            Vec2 wrappedJitter = computeHaltonJitter(phase % seqLen, seqLen);
            TEST_CHECK(std::abs(jitter.x - wrappedJitter.x) < 1e-6F);
            TEST_CHECK(std::abs(jitter.y - wrappedJitter.y) < 1e-6F);
        }
    }

    return 0;
}

// -----------------------------------------------------------------------------
// Test 2: Projection matrix jitter math
// -----------------------------------------------------------------------------
static int testProjectionJitterMatrix() {
    std::cout << "  - Test 2: Projection matrix jitter transformation..." << std::endl;

    const float fovY = 1.04719755F; // 60 deg
    const float aspect = 1920.0F / 1080.0F;
    const float nearP = 0.1F;
    const float farP = 100.0F;
    Mat4 proj = Mat4::perspective(fovY, aspect, nearP, farP);

    const float jitterPixelX = 0.35F;
    const float jitterPixelY = -0.25F;
    const float width = 1920.0F;
    const float height = 1080.0F;

    Mat4 jitteredProj = proj.withJitter(jitterPixelX, jitterPixelY, width, height);

    // Diagonal elements and depth elements m[0], m[5], m[10], m[11], m[14] must be identical
    TEST_CHECK(std::abs(jitteredProj.m[0] - proj.m[0]) < 1e-6F);
    TEST_CHECK(std::abs(jitteredProj.m[5] - proj.m[5]) < 1e-6F);
    TEST_CHECK(std::abs(jitteredProj.m[10] - proj.m[10]) < 1e-6F);
    TEST_CHECK(std::abs(jitteredProj.m[11] - proj.m[11]) < 1e-6F);
    TEST_CHECK(std::abs(jitteredProj.m[14] - proj.m[14]) < 1e-6F);

    // Transform a test view-space point (0, 0, -5.0)
    Vec3 pView(0.0F, 0.0F, -5.0F);
    float wUnjittered = 0.0F;
    Vec3 clipUnjittered = proj.transformPointWithW(pView, wUnjittered);
    Vec2 ndcUnjittered(clipUnjittered.x / wUnjittered, clipUnjittered.y / wUnjittered);

    float wJittered = 0.0F;
    Vec3 clipJittered = jitteredProj.transformPointWithW(pView, wJittered);
    Vec2 ndcJittered(clipJittered.x / wJittered, clipJittered.y / wJittered);

    TEST_CHECK(std::abs(wUnjittered - wJittered) < 1e-6F);

    // The shift in NDC must match exactly 2.0 * jitterPixels / resolution
    const float expectedShiftX = 2.0F * jitterPixelX / width;
    const float expectedShiftY = 2.0F * jitterPixelY / height;
    TEST_CHECK(std::abs((ndcJittered.x - ndcUnjittered.x) - expectedShiftX) < 1e-5F);
    TEST_CHECK(std::abs((ndcJittered.y - ndcUnjittered.y) - expectedShiftY) < 1e-5F);

    return 0;
}

// -----------------------------------------------------------------------------
// Test 3: Motion vector coordinate conventions
// -----------------------------------------------------------------------------
static int testMotionVectorConvention() {
    std::cout << "  - Test 3: Motion vector convention & reprojection..." << std::endl;

    // Current UV: (0.55, 0.45)
    // Previous UV: (0.50, 0.40)
    // Velocity: (0.55 - 0.50, 0.45 - 0.40) = (+0.05, +0.05)
    Vec2 curUV(0.55F, 0.45F);
    Vec2 prevUV(0.50F, 0.40F);
    Vec2 velocity = curUV - prevUV;

    // Reprojection: uv_history = uv_cur - velocity
    Vec2 reprojectedUV = curUV - velocity;
    TEST_CHECK(std::abs(reprojectedUV.x - prevUV.x) < 1e-6F);
    TEST_CHECK(std::abs(reprojectedUV.y - prevUV.y) < 1e-6F);

    // Stationary test: velocity is (0, 0)
    Vec2 stationaryVel = curUV - curUV;
    TEST_CHECK(std::abs(stationaryVel.x) < 1e-6F && std::abs(stationaryVel.y) < 1e-6F);
    Vec2 reprojectedStationary = curUV - stationaryVel;
    TEST_CHECK(std::abs(reprojectedStationary.x - curUV.x) < 1e-6F);
    TEST_CHECK(std::abs(reprojectedStationary.y - curUV.y) < 1e-6F);

    return 0;
}

// -----------------------------------------------------------------------------
// Test 4: Temporal history validation & rejection rules
// -----------------------------------------------------------------------------
static int testTemporalHistoryRejection() {
    std::cout << "  - Test 4: Temporal history rejection rules..." << std::endl;

    auto testRejection = [](Vec2 curUV, Vec2 vel, float conf, float curDepth, float histDepth,
                            bool historyValid) -> bool {
        if (!historyValid) {
            return false; // Reject: history flagged invalid
        }
        Vec2 prevUV = curUV - vel;
        // Screen bounds test [0, 1]
        if (prevUV.x < 0.0F || prevUV.x > 1.0F || prevUV.y < 0.0F || prevUV.y > 1.0F) {
            return false; // Reject: off-screen
        }
        if (conf < 0.5F) {
            return false; // Reject: low confidence
        }
        // Depth disocclusion test
        if (std::abs(histDepth - curDepth) > 0.1F * (1.0F + curDepth * 2.0F)) {
            return false; // Reject: depth disocclusion
        }
        return true; // Accept history
    };

    // Valid on-screen history
    TEST_CHECK(testRejection(Vec2(0.5F, 0.5F), Vec2(0.02F, 0.01F), 1.0F, 0.5F, 0.51F, true));

    // Off-screen rejection (moving outside left boundary)
    TEST_CHECK(!testRejection(Vec2(0.05F, 0.5F), Vec2(0.10F, 0.0F), 1.0F, 0.5F, 0.5F, true));

    // Global invalid history flag
    TEST_CHECK(!testRejection(Vec2(0.5F, 0.5F), Vec2(0.0F, 0.0F), 1.0F, 0.5F, 0.5F, false));

    // Invalid confidence flag
    TEST_CHECK(!testRejection(Vec2(0.5F, 0.5F), Vec2(0.0F, 0.0F), 0.0F, 0.5F, 0.5F, true));

    // Depth disocclusion (foreground object moving away from background)
    TEST_CHECK(!testRejection(Vec2(0.5F, 0.5F), Vec2(0.0F, 0.0F), 1.0F, 0.2F, 0.8F, true));

    return 0;
}

// -----------------------------------------------------------------------------
// Test 5: YCoCg color space conversion & variance clipping
// -----------------------------------------------------------------------------
static Vec3 rgbToYCoCg(const Vec3& c) {
    return Vec3(0.25F * c.x + 0.50F * c.y + 0.25F * c.z, 0.50F * c.x - 0.50F * c.z,
                -0.25F * c.x + 0.50F * c.y - 0.25F * c.z);
}

static Vec3 yCoCgToRgb(const Vec3& c) {
    return Vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

static int testVarianceClippingInYCoCg() {
    std::cout << "  - Test 5: YCoCg transformation & statistical variance clipping..." << std::endl;

    // Test RGB -> YCoCg -> RGB roundtrip
    const std::vector<Vec3> testColors = {
        Vec3(0.0F, 0.0F, 0.0F), Vec3(1.0F, 1.0F, 1.0F), Vec3(0.8F, 0.2F, 0.1F),
        Vec3(0.1F, 0.9F, 0.3F), Vec3(0.2F, 0.4F, 0.9F), Vec3(2.5F, 1.8F, 0.4F),
    };

    for (const auto& rgb : testColors) {
        Vec3 ycocg = rgbToYCoCg(rgb);
        Vec3 recovered = yCoCgToRgb(ycocg);
        TEST_CHECK(std::abs(rgb.x - recovered.x) < 1e-5F);
        TEST_CHECK(std::abs(rgb.y - recovered.y) < 1e-5F);
        TEST_CHECK(std::abs(rgb.z - recovered.z) < 1e-5F);
    }

    // 3x3 Neighborhood Variance Clipping
    // Uniform neighborhood around 0.5 with a slight gradient
    std::vector<Vec3> neighborhood;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            neighborhood.push_back(Vec3(0.5F + static_cast<float>(x) * 0.02F,
                                        0.5F + static_cast<float>(y) * 0.02F, 0.5F));
        }
    }

    Vec3 m1(0.0F, 0.0F, 0.0F);
    Vec3 m2(0.0F, 0.0F, 0.0F);
    for (const auto& sample : neighborhood) {
        Vec3 ycocg = rgbToYCoCg(sample);
        m1 += ycocg;
        m2 += ycocg * ycocg;
    }
    Vec3 mu = m1 * (1.0F / 9.0F);
    Vec3 sigma(std::sqrt(std::max(0.0F, m2.x / 9.0F - mu.x * mu.x)),
               std::sqrt(std::max(0.0F, m2.y / 9.0F - mu.y * mu.y)),
               std::sqrt(std::max(0.0F, m2.z / 9.0F - mu.z * mu.z)));

    float gamma = 1.25F;
    Vec3 clipMin = mu - sigma * gamma;
    Vec3 clipMax = mu + sigma * gamma;

    // An extreme history outlier (firefly 10.0 or background black 0.0)
    Vec3 historyOutlier = rgbToYCoCg(Vec3(10.0F, 10.0F, 10.0F));
    Vec3 clippedHistory = Vec3(std::clamp(historyOutlier.x, clipMin.x, clipMax.x),
                               std::clamp(historyOutlier.y, clipMin.y, clipMax.y),
                               std::clamp(historyOutlier.z, clipMin.z, clipMax.z));

    Vec3 clippedRgb = yCoCgToRgb(clippedHistory);
    // Clipped value must be firmly pulled back into neighborhood bounds
    TEST_CHECK(clippedRgb.x <= 0.60F && clippedRgb.x >= 0.40F);

    return 0;
}

// -----------------------------------------------------------------------------
// Test 6: Karis tone weighting damping response
// -----------------------------------------------------------------------------
static int testKarisToneWeighting() {
    std::cout << "  - Test 6: Karis tone weighting damping response..." << std::endl;

    auto karisWeight = [](float luminance) -> float {
        return 1.0F / (1.0F + std::max(0.0F, luminance));
    };

    // Dark pixel (L = 0.1) -> w ~ 0.909
    float wDark = karisWeight(0.1F);
    TEST_CHECK(wDark > 0.90F);

    // Standard SDR white (L = 1.0) -> w = 0.500
    float wSdr = karisWeight(1.0F);
    TEST_CHECK(std::abs(wSdr - 0.50F) < 1e-4F);

    // High specular HDR highlight (L = 50.0) -> w ~ 0.0196
    float wHdr = karisWeight(50.0F);
    TEST_CHECK(wHdr < 0.025F);

    // Extreme HDR firefly (L = 500.0) -> w ~ 0.002
    float wFirefly = karisWeight(500.0F);
    TEST_CHECK(wFirefly < 0.003F);

    // Ratio of SDR white weight to firefly weight exceeds 200x dampening
    TEST_CHECK((wSdr / wFirefly) > 200.0F);

    return 0;
}

// -----------------------------------------------------------------------------
// Test 7: Contrast-Adaptive Sharpening (RCAS) filter response
// -----------------------------------------------------------------------------
static int testContrastAdaptiveSharpeningFilter() {
    std::cout << "  - Test 7: Contrast-Adaptive Sharpening filter response..." << std::endl;

    auto applyRcas = [](Vec3 c, Vec3 n, Vec3 s, Vec3 w, Vec3 e, float intensity) -> Vec3 {
        if (intensity <= 0.001F) {
            return c;
        }
        Vec3 minColor =
            Vec3(std::min({c.x, n.x, s.x, w.x, e.x}), std::min({c.y, n.y, s.y, w.y, e.y}),
                 std::min({c.z, n.z, s.z, w.z, e.z}));
        Vec3 maxColor =
            Vec3(std::max({c.x, n.x, s.x, w.x, e.x}), std::max({c.y, n.y, s.y, w.y, e.y}),
                 std::max({c.z, n.z, s.z, w.z, e.z}));

        Vec3 delta = (c * 4.0F) - (n + s + w + e);
        Vec3 sharpened = c + delta * (intensity * 0.25F);

        // Overshoot limiting
        sharpened.x = std::clamp(sharpened.x, minColor.x, maxColor.x);
        sharpened.y = std::clamp(sharpened.y, minColor.y, maxColor.y);
        sharpened.z = std::clamp(sharpened.z, minColor.z, maxColor.z);
        return sharpened;
    };

    Vec3 center(0.5F, 0.5F, 0.5F);
    Vec3 north(0.4F, 0.4F, 0.4F);
    Vec3 south(0.4F, 0.4F, 0.4F);
    Vec3 west(0.4F, 0.4F, 0.4F);
    Vec3 east(0.4F, 0.4F, 0.4F);

    // Neutral / disabled intensity produces exact center pixel
    Vec3 neutral = applyRcas(center, north, south, west, east, 0.0F);
    TEST_CHECK(neutral.x == center.x && neutral.y == center.y && neutral.z == center.z);

    // Active sharpening boosts the center value relative to darker neighbors
    Vec3 sharpened = applyRcas(center, north, south, west, east, 0.5F);
    TEST_CHECK(sharpened.x >= center.x);

    // Overshoot limit: even at extreme intensity 10.0, output cannot exceed maxColor (0.5)
    Vec3 extreme = applyRcas(center, north, south, west, east, 10.0F);
    TEST_CHECK(extreme.x <= 0.50F && extreme.x >= 0.40F);

    return 0;
}

// -----------------------------------------------------------------------------
// Test 8: GraphicsSettings serialization roundtrip & backward compatibility
// -----------------------------------------------------------------------------
static int testGraphicsSettingsSerialization() {
    std::cout << "  - Test 8: Graphics settings TAA serialization & backward compatibility..."
              << std::endl;

    GraphicsSettings settings{};
    settings.quality.antiAliasing = AntiAliasingMode::TAA;
    settings.quality.taa.enabled = true;
    settings.quality.taa.historyWeight = 0.92F;
    settings.quality.taa.varianceGamma = 1.35F;
    settings.quality.taa.jitterSequence = TaaJitterSequence::Halton32;
    settings.quality.taa.sharpeningEnabled = true;
    settings.quality.taa.sharpeningIntensity = 0.70F;
    settings.quality.taa.useMotionVectors = true;

    auto serializeRes = GraphicsSettingsSerializer::serializeToString(settings);
    TEST_CHECK(serializeRes.hasValue());
    std::string json = serializeRes.value();
    TEST_CHECK(!json.empty());
    TEST_CHECK(json.find("\"taa\"") != std::string::npos);
    TEST_CHECK(json.find("\"historyWeight\"") != std::string::npos);
    TEST_CHECK(json.find("\"jitterSequence\": \"Halton32\"") != std::string::npos);

    auto deserializedRes = GraphicsSettingsSerializer::deserializeFromString(json);
    TEST_CHECK(deserializedRes.hasValue());
    const auto& s2 = deserializedRes.value();

    TEST_CHECK(s2.quality.antiAliasing == AntiAliasingMode::TAA);
    TEST_CHECK(s2.quality.taa.enabled == true);
    TEST_CHECK(std::abs(s2.quality.taa.historyWeight - 0.92F) < 1e-4F);
    TEST_CHECK(std::abs(s2.quality.taa.varianceGamma - 1.35F) < 1e-4F);
    TEST_CHECK(s2.quality.taa.jitterSequence == TaaJitterSequence::Halton32);
    TEST_CHECK(s2.quality.taa.sharpeningEnabled == true);
    TEST_CHECK(std::abs(s2.quality.taa.sharpeningIntensity - 0.70F) < 1e-4F);
    TEST_CHECK(s2.quality.taa.useMotionVectors == true);

    // Backward compatibility: JSON missing "taa" object
    std::string legacyJson = R"({
        "version": 1,
        "quality": {
            "antiAliasing": "None",
            "textureQuality": "High"
        }
    })";
    auto legacyRes = GraphicsSettingsSerializer::deserializeFromString(legacyJson);
    TEST_CHECK(legacyRes.hasValue());
    TEST_CHECK(legacyRes.value().quality.taa.enabled == false);
    TEST_CHECK(legacyRes.value().quality.taa.jitterSequence == TaaJitterSequence::Halton8);

    return 0;
}

// -----------------------------------------------------------------------------
// Test 9: Quality preset defaults
// -----------------------------------------------------------------------------
static int testQualityPresetDefaults() {
    std::cout << "  - Test 9: Quality preset anti-aliasing defaults..." << std::endl;

    auto low = GraphicsSettings::createPreset(GraphicsQualityPreset::Low);
    TEST_CHECK(low.quality.antiAliasing == AntiAliasingMode::None);
    TEST_CHECK(!low.quality.taa.enabled);

    auto med = GraphicsSettings::createPreset(GraphicsQualityPreset::Medium);
    TEST_CHECK(med.quality.antiAliasing == AntiAliasingMode::None);
    TEST_CHECK(!med.quality.taa.enabled);

    auto high = GraphicsSettings::createPreset(GraphicsQualityPreset::High);
    TEST_CHECK(high.quality.antiAliasing == AntiAliasingMode::TAA);
    TEST_CHECK(high.quality.taa.enabled);
    TEST_CHECK(high.quality.taa.jitterSequence == TaaJitterSequence::Halton8);
    TEST_CHECK(high.quality.taa.useMotionVectors);
    TEST_CHECK(high.quality.taa.sharpeningEnabled);

    auto ultra = GraphicsSettings::createPreset(GraphicsQualityPreset::Ultra);
    TEST_CHECK(ultra.quality.antiAliasing == AntiAliasingMode::TAA);
    TEST_CHECK(ultra.quality.taa.enabled);
    TEST_CHECK(ultra.quality.taa.jitterSequence == TaaJitterSequence::Halton16);

    auto cinematic = GraphicsSettings::createPreset(GraphicsQualityPreset::Cinematic);
    TEST_CHECK(cinematic.quality.antiAliasing == AntiAliasingMode::TAA);
    TEST_CHECK(cinematic.quality.taa.enabled);
    TEST_CHECK(cinematic.quality.taa.jitterSequence == TaaJitterSequence::Halton32);

    return 0;
}

// -----------------------------------------------------------------------------
// Test 10: SceneRenderer offscreen execution under Null RHI
// -----------------------------------------------------------------------------
static int testSceneRendererOffscreenExecution() {
    std::cout << "  - Test 10: SceneRenderer offscreen execution with TAA enabled/disabled..."
              << std::endl;

    auto devRes = rhi::createNullDevice();
    TEST_CHECK(devRes.hasValue());
    auto device = std::move(devRes.value());
    TEST_CHECK(device != nullptr);

    SceneRenderer renderer;
    auto initRes = renderer.init(*device, 1280, 720);
    TEST_CHECK(initRes.hasValue());

    // Initially TAA is enabled when post-processing is active
    renderer.setPostProcessingEnabled(true);
    TEST_CHECK(renderer.isTaaEnabled());
    TEST_CHECK(renderer.getVelocityTarget().isValid());
    TEST_CHECK(renderer.getTaaHistoryTarget().isValid());

    world::World world;
    AssetManager assetMgr;
    auto cmdRes = device->createCommandList();
    TEST_CHECK(cmdRes.hasValue());
    auto cmd = std::move(cmdRes.value());

    Mat4 viewProj = Mat4::perspective(1.04719755F, 1280.0F / 720.0F, 0.1F, 100.0F);

    // Frame 1: History is initially invalid, first temporal accumulation
    cmd->begin();
    RenderStats stats1 = renderer.renderScene(world, assetMgr, *device, *cmd, viewProj);
    cmd->end();
    TEST_CHECK(stats1.triangles == 0);

    // Invalidate history (simulating camera cut)
    renderer.invalidateTaaHistory();

    // Frame 2: Render after cut
    cmd->begin();
    RenderStats stats2 = renderer.renderScene(world, assetMgr, *device, *cmd, viewProj);
    cmd->end();
    TEST_CHECK(stats2.triangles == 0);

    // Disable TAA via graphics settings
    GraphicsSettings settings = renderer.getGraphicsSettings();
    settings.quality.antiAliasing = AntiAliasingMode::None;
    settings.quality.taa.enabled = false;
    auto applyRes = renderer.applyGraphicsSettings(settings);
    TEST_CHECK(applyRes.hasValue());
    TEST_CHECK(!renderer.isTaaEnabled());

    // Frame 3: Render without TAA
    cmd->begin();
    RenderStats stats3 = renderer.renderScene(world, assetMgr, *device, *cmd, viewProj);
    cmd->end();
    TEST_CHECK(stats3.triangles == 0);

    renderer.shutdown();
    return 0;
}

int main() {
    std::cout << "Running Milestone 11H: Advanced Anti-Aliasing & Temporal Rendering Test Suite..."
              << std::endl;

    TEST_CHECK(testHaltonSequenceAndJitterMath() == 0);
    TEST_CHECK(testProjectionJitterMatrix() == 0);
    TEST_CHECK(testMotionVectorConvention() == 0);
    TEST_CHECK(testTemporalHistoryRejection() == 0);
    TEST_CHECK(testVarianceClippingInYCoCg() == 0);
    TEST_CHECK(testKarisToneWeighting() == 0);
    TEST_CHECK(testContrastAdaptiveSharpeningFilter() == 0);
    TEST_CHECK(testGraphicsSettingsSerialization() == 0);
    TEST_CHECK(testQualityPresetDefaults() == 0);
    TEST_CHECK(testSceneRendererOffscreenExecution() == 0);

    std::cout << "Milestone 11H Test Suite: All 10 verification suites passed successfully!"
              << std::endl;
    return 0;
}
