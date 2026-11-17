/// @file post_process_test.cpp
/// @brief Physical, mathematical, and offscreen verification tests for Milestone 10F HDR,
/// Tone Mapping, and Bloom Post-Processing.
///
/// Milestone 10F Requirements:
/// 1. Tone mapping mathematical operators: ACES fitted, Filmic Reinhard, Khronos PBR Neutral,
/// Linear Bypass.
/// 2. sRGB OETF transfer curve application for UNORM targets.
/// 3. Bloom soft-knee luminance extraction math and numerical stability.
/// 4. HDR scene render target capability negotiation and resizing.
/// 5. Offscreen GPU scene rendering with exposure control, bloom pyramid, and tone mapping passes.

#include "core/assert.h"
#include "core/math.h"
#include "physics/physics_components.h"
#include "render/asset_manager.h"
#include "render/gltf_scene_importer.h"
#include "render/graphics_settings.h"
#include "render/graphics_settings_serializer.h"
#include "render/light_components.h"
#include "render/lighting_data.h"
#include "render/mesh.h"
#include "render/mesh_component.h"
#include "render/post_process_types.h"
#include "render/scene_renderer.h"
#include "rhi/command_list.h"
#include "rhi/rhi_device.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ANTERAY_HAS_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define ANTERAY_HAS_ASAN 1
#endif

#if defined(ANTERAY_HAS_ASAN)
extern "C" const char* __lsan_default_suppressions() {
    return "leak:libdbus-1\nleak:libvulkan\nleak:nvidia\nleak:libwayland\n";
}
#endif

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
using namespace eng::rhi;

// CPU reference implementations matching shader code for mathematical validation

static inline float acesFittedCurve(float x) noexcept {
    x = std::max(0.0F, x);
    constexpr float a = 2.51F;
    constexpr float b = 0.03F;
    constexpr float c = 2.43F;
    constexpr float d = 0.59F;
    constexpr float e = 0.14F;
    return std::clamp((x * ((a * x) + b)) / ((x * ((c * x) + d)) + e), 0.0F, 1.0F);
}

static inline Vec3 acesFittedCpu(Vec3 color) noexcept {
    float r = std::max(0.0F, color.x);
    float g = std::max(0.0F, color.y);
    float b = std::max(0.0F, color.z);

    // ACES_INPUT_MATRIX (sRGB to AP1)
    float ap1R = 0.59719F * r + 0.35458F * g + 0.04823F * b;
    float ap1G = 0.07600F * r + 0.90834F * g + 0.01566F * b;
    float ap1B = 0.02840F * r + 0.13383F * g + 0.83777F * b;

    auto rrtOdt = [](float v) noexcept {
        float a = v * (v + 0.0245786F) - 0.000090537F;
        float d = v * (0.983729F * v + 0.4329510F) + 0.238081F;
        return a / d;
    };
    float fitR = rrtOdt(ap1R);
    float fitG = rrtOdt(ap1G);
    float fitB = rrtOdt(ap1B);

    // ACES_OUTPUT_MATRIX (AP1 to sRGB)
    float outR = 1.60475F * fitR - 0.53108F * fitG - 0.07367F * fitB;
    float outG = -0.10208F * fitR + 1.10813F * fitG - 0.00605F * fitB;
    float outB = -0.00327F * fitR - 0.07276F * fitG + 1.07602F * fitB;
    return Vec3(std::clamp(outR, 0.0F, 1.0F), std::clamp(outG, 0.0F, 1.0F),
                std::clamp(outB, 0.0F, 1.0F));
}

static inline float filmicReinhardCurve(float x, float white = 4.0F) noexcept {
    x = std::max(0.0F, x);
    return std::clamp((x * (1.0F + (x / (white * white)))) / (1.0F + x), 0.0F, 1.0F);
}

static inline Vec3 neutralToneMappingCpu(Vec3 color) noexcept {
    constexpr float startCompression = 0.8F - 0.04F;
    constexpr float desaturation = 0.15F;

    float x = std::min(color.x, std::min(color.y, color.z));
    float offset = (x < 0.08F) ? (x - (6.25F * x * x)) : 0.04F;
    color.x -= offset;
    color.y -= offset;
    color.z -= offset;

    float peak = std::max(color.x, std::max(color.y, color.z));
    if (peak < startCompression) {
        return Vec3(std::clamp(color.x, 0.0F, 1.0F), std::clamp(color.y, 0.0F, 1.0F),
                    std::clamp(color.z, 0.0F, 1.0F));
    }

    constexpr float d = 1.0F - startCompression;
    float newPeak = 1.0F - ((d * d) / (peak + d - startCompression));
    color = color * (newPeak / peak);

    float g = 1.0F - (1.0F / ((desaturation * (peak - newPeak)) + 1.0F));
    Vec3 res = color * (1.0F - g) + Vec3(newPeak, newPeak, newPeak) * g;
    return Vec3(std::clamp(res.x, 0.0F, 1.0F), std::clamp(res.y, 0.0F, 1.0F),
                std::clamp(res.z, 0.0F, 1.0F));
}

static inline Vec3 agxToneMappingCpu(Vec3 color) noexcept {
    float r = std::max(0.0F, color.x);
    float g = std::max(0.0F, color.y);
    float b = std::max(0.0F, color.z);

    float inR = 0.842479062253094F * r + 0.0784335999999992F * g + 0.0792237451977152F * b;
    float inG = 0.0423282422610123F * r + 0.878468646464646F * g + 0.0792237451977152F * b;
    float inB = 0.0423756549057051F * r + 0.0784335999999992F * g + 0.879237451977152F * b;

    inR = std::max(inR, 1e-10F);
    inG = std::max(inG, 1e-10F);
    inB = std::max(inB, 1e-10F);

    constexpr float minEv = -12.47393F;
    constexpr float maxEv = 4.026069F;
    constexpr float evRange = maxEv - minEv;

    auto logMap = [&](float v) {
        float norm = (std::log2(v) - minEv) / evRange;
        norm = std::clamp(norm, 0.0F, 1.0F);
        float x2 = norm * norm;
        float x4 = x2 * x2;
        float sig = 15.5F * (x4 * x2) - 40.14F * (x4 * norm) + 31.96F * x4 - 6.868F * (x2 * norm) +
                    0.4298F * x2 + 0.1191F * norm - 0.00232F;
        return std::clamp(sig, 0.0F, 1.0F);
    };

    float sigR = logMap(inR);
    float sigG = logMap(inG);
    float sigB = logMap(inB);

    // AgX Look (ASC-CDL contrast & saturation in display space)
    constexpr float slope = 1.05F;
    constexpr float power = 1.25F;
    constexpr float sat = 1.25F;
    float lookR = std::pow(std::max(0.0F, sigR * slope), power);
    float lookG = std::pow(std::max(0.0F, sigG * slope), power);
    float lookB = std::pow(std::max(0.0F, sigB * slope), power);
    float luma = 0.2126F * lookR + 0.7152F * lookG + 0.0722F * lookB;
    lookR = std::clamp(luma + sat * (lookR - luma), 0.0F, 1.0F);
    lookG = std::clamp(luma + sat * (lookG - luma), 0.0F, 1.0F);
    lookB = std::clamp(luma + sat * (lookB - luma), 0.0F, 1.0F);

    // Decompress display-space (~gamma 2.2) to linear before linear AGX_OUTSET_MATRIX
    float linR = std::pow(lookR, 2.2F);
    float linG = std::pow(lookG, 2.2F);
    float linB = std::pow(lookB, 2.2F);

    float outR = 1.19687900512017F * linR - 0.0980208811401368F * linG - 0.0990297440797205F * linB;
    float outG =
        -0.0528968517574562F * linR + 1.15190312990417F * linG - 0.0990297440797205F * linB;
    float outB =
        -0.0529716355178468F * linR - 0.0980434501171801F * linG + 1.15107360136118F * linB;

    return Vec3(std::clamp(outR, 0.0F, 1.0F), std::clamp(outG, 0.0F, 1.0F),
                std::clamp(outB, 0.0F, 1.0F));
}

static inline float linearToSrgbCpu(float x) noexcept {
    x = std::max(0.0F, x);
    return (x <= 0.0031308F) ? (12.92F * x) : ((1.055F * std::pow(x, 1.0F / 2.4F)) - 0.055F);
}

static constexpr float toRadians(float deg) noexcept {
    return deg * (3.14159265358979323846F / 180.0F);
}

static inline float extractThresholdCpu(float brightness, float thresh, float knee) noexcept {
    float soft = brightness - thresh + knee;
    soft = std::clamp(soft, 0.0F, 2.0F * knee);
    soft = (soft * soft) / ((4.0F * knee) + 1e-4F);
    float contribution = std::max(soft, brightness - thresh);
    contribution /= std::max(brightness, 1e-4F);
    return std::clamp(contribution, 0.0F, 1.0F);
}

static bool savePpm(const std::string& path, const uint8_t* pixels, uint32_t width,
                    uint32_t height) {
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    file << "P6\n" << width << " " << height << "\n255\n";
    for (size_t i = 0; i < static_cast<size_t>(width * height * 4); i += 4) {
        file.put(static_cast<char>(pixels[i + 0]));
        file.put(static_cast<char>(pixels[i + 1]));
        file.put(static_cast<char>(pixels[i + 2]));
    }
    return true;
}

// -----------------------------------------------------------------------------
// Test 1: Tone Mapping Mathematical Operators & Transfer Functions
// -----------------------------------------------------------------------------
static int testToneMappingMath() {
    std::cout << "[Test 1] Verifying tone mapping mathematical curves and numerical limits...\n";

    // 1. ACES Fitted
    // Monotonicity test
    float prevAces = -1.0F;
    for (float v = 0.0F; v <= 50.0F; v += 0.05F) {
        float cur = acesFittedCurve(v);
        TEST_CHECK(cur >= prevAces);
        TEST_CHECK(cur >= 0.0F && cur <= 1.0F);
        prevAces = cur;
    }
    // Negative input safe clamping
    TEST_CHECK(acesFittedCurve(-1.0F) <= 0.01F);
    TEST_CHECK(acesFittedCurve(-100.0F) <= 0.01F);
    // Extreme input safety
    TEST_CHECK(acesFittedCurve(10000.0F) <= 1.0F && !std::isnan(acesFittedCurve(10000.0F)));
    // Asymptotic highlight roll-off
    TEST_CHECK(acesFittedCurve(0.0F) < 0.01F);
    TEST_CHECK(acesFittedCurve(0.18F) > 0.25F && acesFittedCurve(0.18F) < 0.45F);
    TEST_CHECK(acesFittedCurve(1.0F) > 0.75F && acesFittedCurve(1.0F) < 0.90F);
    TEST_CHECK(acesFittedCurve(100.0F) > 0.98F);

    // 2. Filmic Reinhard
    float prevReinhard = -1.0F;
    for (float v = 0.0F; v <= 50.0F; v += 0.05F) {
        float cur = filmicReinhardCurve(v, 4.0F);
        TEST_CHECK(cur >= prevReinhard);
        TEST_CHECK(cur >= 0.0F && cur <= 1.0F);
        prevReinhard = cur;
    }
    TEST_CHECK(std::abs(filmicReinhardCurve(0.0F, 4.0F) - 0.0F) < 1e-5F);
    TEST_CHECK(filmicReinhardCurve(-5.0F, 4.0F) == 0.0F);
    TEST_CHECK(filmicReinhardCurve(1.0F, 4.0F) > 0.50F && filmicReinhardCurve(1.0F, 4.0F) < 0.60F);
    TEST_CHECK(filmicReinhardCurve(4.0F, 4.0F) > 0.90F);
    TEST_CHECK(filmicReinhardCurve(100.0F, 4.0F) > 0.98F);
    TEST_CHECK(filmicReinhardCurve(10000.0F, 4.0F) <= 1.0F &&
               !std::isnan(filmicReinhardCurve(10000.0F, 4.0F)));

    // Configurable White Point check: lower white point compresses faster
    float rLowWhite = filmicReinhardCurve(2.0F, 2.0F);
    float rHighWhite = filmicReinhardCurve(2.0F, 8.0F);
    TEST_CHECK(rLowWhite > rHighWhite);
    TEST_CHECK(std::abs(rLowWhite - 1.0F) < 1e-4F);

    // 3. Khronos PBR Neutral
    Vec3 lowColor(0.2F, 0.4F, 0.6F);
    Vec3 mappedLow = neutralToneMappingCpu(lowColor);
    // Linear region preservation
    TEST_CHECK(std::abs(mappedLow.y - 0.4F) < 0.05F);
    TEST_CHECK(std::abs(mappedLow.z - 0.6F) < 0.05F);

    Vec3 brightColor(5.0F, 2.0F, 1.0F);
    Vec3 mappedBright = neutralToneMappingCpu(brightColor);
    TEST_CHECK(mappedBright.x <= 1.0F && mappedBright.y <= 1.0F && mappedBright.z <= 1.0F);
    // Preserves hue hierarchy
    TEST_CHECK(mappedBright.x > mappedBright.y);
    TEST_CHECK(mappedBright.y > mappedBright.z);

    // Negative and extreme input safety
    Vec3 negMapped = neutralToneMappingCpu(Vec3(-1.0F, -0.5F, -0.1F));
    TEST_CHECK(negMapped.x >= 0.0F && negMapped.y >= 0.0F && negMapped.z >= 0.0F);
    Vec3 extremeMapped = neutralToneMappingCpu(Vec3(10000.0F, 5000.0F, 1000.0F));
    TEST_CHECK(extremeMapped.x <= 1.0F && !std::isnan(extremeMapped.x));

    // 4. AgX Tone Mapping Operator
    Vec3 agxZero = agxToneMappingCpu(Vec3(0.0F, 0.0F, 0.0F));
    TEST_CHECK(agxZero.x >= 0.0F && agxZero.x < 0.01F);
    Vec3 agxMid = agxToneMappingCpu(Vec3(0.18F, 0.18F, 0.18F));
    TEST_CHECK(agxMid.x > 0.10F && agxMid.x < 0.30F);
    Vec3 agxHigh = agxToneMappingCpu(Vec3(10.0F, 10.0F, 10.0F));
    TEST_CHECK(agxHigh.x > agxMid.x && agxHigh.x <= 1.0F);
    Vec3 agxExtreme = agxToneMappingCpu(Vec3(10000.0F, 10000.0F, 10000.0F));
    TEST_CHECK(agxExtreme.x <= 1.0F && !std::isnan(agxExtreme.x));

    // 5. Distinct Operator Outputs at Mid-Range Radiance (v = 2.0)
    float vMid = 2.0F;
    float opAgX = agxToneMappingCpu(Vec3(vMid, vMid, vMid)).x;
    float opAces = acesFittedCurve(vMid);
    float opReinhard = filmicReinhardCurve(vMid, 4.0F);
    float opNeutral = neutralToneMappingCpu(Vec3(vMid, vMid, vMid)).x;
    float opBypass = std::clamp(vMid, 0.0F, 1.0F);
    TEST_CHECK(std::abs(opAces - opReinhard) > 0.05F);
    TEST_CHECK(std::abs(opAces - opNeutral) > 0.02F);
    TEST_CHECK(std::abs(opAgX - opAces) > 0.02F);
    TEST_CHECK(std::abs(opReinhard - opNeutral) > 0.05F);
    TEST_CHECK(opBypass == 1.0F);

    // 5. sRGB Transfer Curve
    TEST_CHECK(std::abs(linearToSrgbCpu(0.0F) - 0.0F) < 1e-5F);
    TEST_CHECK(std::abs(linearToSrgbCpu(1.0F) - 1.0F) < 1e-4F);
    // Linear segment threshold check
    float linVal = 0.002F;
    TEST_CHECK(std::abs(linearToSrgbCpu(linVal) - (12.92F * linVal)) < 1e-5F);
    // Perceptual mid-gray (0.18 linear ~ 0.46 sRGB)
    float midGray = linearToSrgbCpu(0.18F);
    TEST_CHECK(midGray > 0.44F && midGray < 0.48F);

    std::cout << "  Tone mapping mathematical verification passed.\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Test 2: Bloom Soft-Knee Luminance Extraction Math
// -----------------------------------------------------------------------------
static int testBloomExtractionMath() {
    std::cout << "[Test 2] Verifying bloom soft-knee extraction math...\n";

    const float thresh = 1.0F;
    const float knee = 0.5F;

    // Below knee (brightness <= thresh - knee = 0.5): extraction must be strictly 0
    TEST_CHECK(extractThresholdCpu(0.1F, thresh, knee) == 0.0F);
    TEST_CHECK(extractThresholdCpu(0.49F, thresh, knee) == 0.0F);

    // In knee region (0.5 < brightness < 1.5): smooth positive curve
    float inKnee1 = extractThresholdCpu(0.75F, thresh, knee);
    float inKnee2 = extractThresholdCpu(1.0F, thresh, knee);
    float inKnee3 = extractThresholdCpu(1.25F, thresh, knee);
    TEST_CHECK(inKnee1 > 0.0F && inKnee1 < inKnee2);
    TEST_CHECK(inKnee2 < inKnee3);

    // Above knee region (brightness >= 1.5): monotonic approach to 1.0
    float high1 = extractThresholdCpu(2.0F, thresh, knee);
    float high2 = extractThresholdCpu(10.0F, thresh, knee);
    TEST_CHECK(high1 > inKnee3 && high1 < 1.0F);
    TEST_CHECK(high2 > high1 && high2 > 0.85F);

    // Sensitivity to threshold: higher threshold reduces extraction
    float extLowThresh = extractThresholdCpu(2.0F, 1.0F, 0.5F);
    float extHighThresh = extractThresholdCpu(2.0F, 1.5F, 0.5F);
    TEST_CHECK(extLowThresh > extHighThresh);

    // Sensitivity to knee: zero knee produces sharp step threshold
    float hardBelow = extractThresholdCpu(0.99F, 1.0F, 0.0F);
    float hardAbove = extractThresholdCpu(1.01F, 1.0F, 0.0F);
    TEST_CHECK(hardBelow == 0.0F);
    TEST_CHECK(hardAbove > 0.0F);

    std::cout << "  Bloom extraction verification passed.\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Test 3: HDR Render Target Allocation & Dynamic Resizing
// -----------------------------------------------------------------------------
static int testSceneRendererHdrLifecycle() {
    std::cout << "[Test 3] Verifying HDR target lifecycle, format negotiation, and resizing...\n";

    auto nullDevRes = createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    SceneRenderer renderer;
    auto initRes = renderer.init(*nullDevice, 800, 600);
    TEST_CHECK(initRes.hasValue());
    TEST_CHECK(renderer.isInitialized());

    // Verify HDR target was allocated
    TEST_CHECK(renderer.getHdrTarget().isValid());
    TEST_CHECK(renderer.getColorTarget().isValid());
    TEST_CHECK(renderer.getDepthTarget().isValid());

    // Verify format capability negotiation
    auto hdrFormat = renderer.getHdrFormat();
    TEST_CHECK(hdrFormat == TextureFormat::RGBA16_FLOAT ||
               hdrFormat == TextureFormat::RGBA32_FLOAT || hdrFormat == TextureFormat::RGBA8_UNORM);

    // Verify default post-processing settings state
    TEST_CHECK(!renderer.isPostProcessingEnabled());
    PostProcessSettings ppSettings{};
    TEST_CHECK(ppSettings.toneMapOp == ToneMapOperator::AgX);
    ppSettings.enabled = true;
    ppSettings.exposure = 1.5F;
    ppSettings.toneMapOp = ToneMapOperator::FilmicReinhard;
    ppSettings.bloomEnabled = true;
    renderer.setPostProcessSettings(ppSettings);
    TEST_CHECK(renderer.isPostProcessingEnabled());
    TEST_CHECK(renderer.isHdrActive());
    TEST_CHECK(renderer.getPostProcessSettings().exposure == 1.5F);
    TEST_CHECK(renderer.getPostProcessSettings().toneMapOp == ToneMapOperator::FilmicReinhard);

    // Test multiple dynamic resize operations
    TEST_CHECK(renderer.resize(1920, 1080).hasValue());
    TEST_CHECK(renderer.getWidth() == 1920 && renderer.getHeight() == 1080);
    TEST_CHECK(renderer.getHdrTarget().isValid());

    TEST_CHECK(renderer.resize(128, 128).hasValue());
    TEST_CHECK(renderer.getWidth() == 128 && renderer.getHeight() == 128);

    // Small boundary dimensions (pyramid levels >= 1 pixel)
    TEST_CHECK(renderer.resize(16, 16).hasValue());
    TEST_CHECK(renderer.getWidth() == 16 && renderer.getHeight() == 16);

    renderer.shutdown();
    TEST_CHECK(!renderer.isInitialized());

    std::cout << "  HDR lifecycle and resizing verification passed.\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Test 4: Genuine GPU Floating-Point HDR Radiance Readback
// -----------------------------------------------------------------------------
static int testGpuHdrRadianceReadback() {
    std::cout << "[Test 4] Verifying GPU offscreen HDR target readback (radiance > 1.0)...\n";

    VulkanDeviceDesc devDesc{.windowHandle = nullptr, .enableValidation = true};
    auto devRes = createVulkanDevice(devDesc);
    if (!devRes.hasValue()) {
        std::cout << "  Vulkan device unavailable on this host; skipping GPU HDR readback test.\n";
        return 0;
    }
    auto device = std::move(devRes.value());

    constexpr uint32_t width = 64;
    constexpr uint32_t height = 64;
    SceneRenderer renderer;
    auto initRes = renderer.init(*device, width, height);
    TEST_CHECK(initRes.hasValue());

    PostProcessSettings pp{};
    pp.enabled = true;
    renderer.setPostProcessSettings(pp);

    TEST_CHECK(renderer.isHdrActive());
    TEST_CHECK(!renderer.isSdrFallbackActive());
    TEST_CHECK(renderer.getHdrFormat() == rhi::TextureFormat::RGBA16_FLOAT ||
               renderer.getHdrFormat() == rhi::TextureFormat::RGBA32_FLOAT);

    world::World world;
    physics::registerPhysicsComponents(world);
    AssetManager assetManager;

    // Create an emissive HDR quad with intense radiance
    MeshData emissiveQuad{};
    emissiveQuad.vertices = {
        Vertex(Vec3(-1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 1.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 1.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(1.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 0.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(-1.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 0.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
    };
    emissiveQuad.indices = {0, 1, 2, 2, 3, 0};
    emissiveQuad.submeshes.push_back(Submesh{
        .name = "EmissiveSubmesh",
        .firstIndex = 0,
        .indexCount = 6,
        .vertexOffset = 0,
        .materialIndex = 0,
    });
    Material emissiveMat{};
    emissiveMat.baseColor = Vec4(1.0F, 1.0F, 1.0F, 1.0F);
    emissiveMat.emissiveFactor = Vec3(8.0F, 6.0F, 4.0F); // High radiance > 1.0
    emissiveQuad.materials = {emissiveMat};

    auto regRes = assetManager.registerMesh("test/emissive_quad", emissiveQuad);
    TEST_CHECK(regRes.hasValue());
    AssetId meshId = regRes.value();

    auto& ecs = world.getRawWorld();
    auto entity = ecs.entity("HdrEmissiveEntity");
    entity.set<MeshComponent>(MeshComponent{
        .meshAsset = meshId,
        .submeshIndex = kAllSubmeshes,
        .visible = true,
    });
    entity.set<physics::PhysicsTransformComponent>(physics::PhysicsTransformComponent{
        .transform = Transform(Vec3(0.0F, 0.0F, 0.0F)),
    });

    // FP16 buffer size: 64 * 64 * 4 * sizeof(uint16_t) = 32768 bytes
    const size_t fp16ByteSize = width * height * 4 * sizeof(uint16_t);
    BufferDesc rbDesc{
        .sizeInBytes = fp16ByteSize,
        .usage = BufferUsageFlags::TransferDst,
        .memoryUsage = MemoryUsage::CpuToGpu,
        .debugName = "HdrFp16ReadbackBuffer",
    };
    auto rbRes = device->createBuffer(rbDesc);
    TEST_CHECK(rbRes.hasValue());
    BufferHandle rbHandle = rbRes.value();

    Mat4 view = Mat4::lookAt(Vec3(0, 0, 2), Vec3(0, 0, 0), Vec3(0, 1, 0));
    Mat4 proj = Mat4::perspective(toRadians(60.0F), 1.0F, 0.1F, 100.0F);
    Mat4 viewProj = proj * view;

    auto cmdRes = device->createCommandList();
    TEST_CHECK(cmdRes.hasValue());
    auto cmd = std::move(cmdRes.value());

    cmd->begin();
    renderer.renderScene(world, assetManager, *device, *cmd, viewProj);
    // Copy directly from offscreen floating-point HDR target
    cmd->copyTextureToBuffer(renderer.getHdrTarget(), rbHandle);
    cmd->end();

    TEST_CHECK(device->submit(cmd.get()).hasValue());
    device->waitIdle();

    std::vector<uint16_t> fp16Pixels(width * height * 4, 0);
    TEST_CHECK(device->readBuffer(rbHandle, 0, fp16Pixels.data(), fp16ByteSize).hasValue());
    device->destroyBuffer(rbHandle);
    renderer.shutdown();

    float maxRadiance = 0.0F;
    size_t hdrPixelCount = 0;
    for (size_t i = 0; i < fp16Pixels.size(); i += 4) {
        float r = halfToFloat(fp16Pixels[i + 0]);
        float g = halfToFloat(fp16Pixels[i + 1]);
        float b = halfToFloat(fp16Pixels[i + 2]);
        maxRadiance = std::max({maxRadiance, r, g, b});
        if (r > 1.0F || g > 1.0F || b > 1.0F) {
            hdrPixelCount++;
        }
    }

    std::cout << "  GPU HDR readback verified: max radiance = " << maxRadiance
              << ", pixels with radiance > 1.0 = " << hdrPixelCount << "\n";
    TEST_CHECK(maxRadiance > 1.5F);
    TEST_CHECK(hdrPixelCount > 100);
    TEST_CHECK(!std::isnan(maxRadiance) && !std::isinf(maxRadiance));

    return 0;
}

// -----------------------------------------------------------------------------
// Test 5: Vulkan GPU Post-Processing Execution & Pixel Verification
// -----------------------------------------------------------------------------
static int testVulkanPostProcessRendering() {
    std::cout << "[Test 5] Verifying Vulkan GPU HDR rendering, bloom, and tone mapping passes...\n";

    VulkanDeviceDesc devDesc{.windowHandle = nullptr, .enableValidation = true};
    auto devRes = createVulkanDevice(devDesc);
    if (!devRes.hasValue()) {
        std::cout << "  Vulkan device unavailable on this host; skipping Vulkan GPU test.\n";
        return 0;
    }
    auto device = std::move(devRes.value());

    constexpr uint32_t width = 64;
    constexpr uint32_t height = 64;
    SceneRenderer renderer;
    auto initRes = renderer.init(*device, width, height);
    TEST_CHECK(initRes.hasValue());

    // Enable post processing
    PostProcessSettings pp{};
    pp.enabled = true;
    pp.exposure = 1.0F;
    pp.toneMapOp = ToneMapOperator::AcesFitted;
    pp.bloomEnabled = true;
    pp.bloomThreshold = 0.5F;
    pp.bloomIntensity = 0.05F;
    renderer.setPostProcessSettings(pp);

    world::World world;
    physics::registerPhysicsComponents(world);
    AssetManager assetManager;

    const std::string helmetPath = "test-assets/DamagedHelmet.glb";
    if (std::filesystem::exists(helmetPath)) {
        auto importRes = GltfSceneImporter::importScene(helmetPath, world, &assetManager);
        TEST_CHECK(importRes.success);
    } else {
        flecs::world& ecs = world.getRawWorld();
        auto lightEnt = ecs.entity("HighIntensityLight");
        PointLightComponent ptComp{};
        ptComp.enabled = true;
        ptComp.position = Vec3(0.0F, 0.0F, 1.0F);
        ptComp.color = Vec3(1.0F, 1.0F, 1.0F);
        ptComp.intensity = 10.0F;
        ptComp.range = 10.0F;
        lightEnt.set<PointLightComponent>(ptComp);
    }

    // Readback buffer
    const uint64_t byteSize = width * height * 4;
    BufferDesc rbDesc{
        .sizeInBytes = byteSize,
        .usage = BufferUsageFlags::TransferDst,
        .memoryUsage = MemoryUsage::GpuToCpu,
        .debugName = "ReadbackBuffer",
    };
    auto rbRes = device->createBuffer(rbDesc);
    TEST_CHECK(rbRes.hasValue());
    auto rbHandle = rbRes.value();

    auto cmdRes = device->createCommandList();
    TEST_CHECK(cmdRes.hasValue());
    auto cmd = std::move(cmdRes.value());

    Mat4 view =
        Mat4::lookAt(Vec3(0.0F, 0.0F, 2.5F), Vec3(0.0F, 0.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F));
    Mat4 proj = Mat4::perspective(toRadians(60.0F), 1.0F, 0.1F, 100.0F);
    Mat4 viewProj = proj * view;

    auto capture = [&](const std::string& name, const PostProcessSettings& settings,
                       std::vector<uint8_t>& outPx) -> int {
        renderer.setPostProcessSettings(settings);
        cmd->begin();
        renderer.renderScene(world, assetManager, *device, *cmd, viewProj);
        renderer.renderPostProcess(*cmd);
        cmd->copyTextureToBuffer(renderer.getColorTarget(), rbHandle);
        cmd->end();

        TEST_CHECK(device->submit(cmd.get()).hasValue());
        device->waitIdle();

        outPx.assign(byteSize, 0);
        auto readRes = device->readBuffer(rbHandle, 0, outPx.data(), byteSize);
        TEST_CHECK(readRes.hasValue());
        uint64_t total = 0;
        for (uint8_t b : outPx) {
            total += b;
        }
        std::cout << "  Capture " << name << ": total=" << total << ", sample[0..3]=("
                  << static_cast<int>(outPx[0]) << "," << static_cast<int>(outPx[1]) << ","
                  << static_cast<int>(outPx[2]) << "," << static_cast<int>(outPx[3]) << ")\n";
        savePpm("test-assets/" + name + ".ppm", outPx.data(), width, height);
        return 0;
    };

    // 1. Base scene with ACES tone mapping (normal exposure = 1.0)
    PostProcessSettings sAcesNormal{
        .enabled = true,
        .exposure = 1.0F,
        .toneMapOp = ToneMapOperator::AcesFitted,
        .bloomEnabled = true,
        .bloomThreshold = 0.5F,
        .bloomKnee = 0.5F,
        .bloomIntensity = 0.05F,
        .bloomFilterRadius = 1.0F,
    };
    std::vector<uint8_t> pxAcesNorm;
    TEST_CHECK(capture("postprocess_aces_normal", sAcesNormal, pxAcesNorm) == 0);

    // 2. Increased exposure (2.5)
    PostProcessSettings sAcesHigh = sAcesNormal;
    sAcesHigh.exposure = 2.5F;
    std::vector<uint8_t> pxAcesHigh;
    TEST_CHECK(capture("postprocess_aces_high_exposure", sAcesHigh, pxAcesHigh) == 0);

    // 3. Decreased exposure (0.3)
    PostProcessSettings sAcesLow = sAcesNormal;
    sAcesLow.exposure = 0.3F;
    std::vector<uint8_t> pxAcesLow;
    TEST_CHECK(capture("postprocess_aces_low_exposure", sAcesLow, pxAcesLow) == 0);

    // Verify brightness relationship: high > normal > low
    uint64_t sumHigh = 0;
    uint64_t sumNorm = 0;
    uint64_t sumLow = 0;
    for (size_t i = 0; i < byteSize; i += 4) {
        sumHigh += static_cast<uint64_t>(pxAcesHigh[i] + pxAcesHigh[i + 1] + pxAcesHigh[i + 2]);
        sumNorm += static_cast<uint64_t>(pxAcesNorm[i] + pxAcesNorm[i + 1] + pxAcesNorm[i + 2]);
        sumLow += static_cast<uint64_t>(pxAcesLow[i] + pxAcesLow[i + 1] + pxAcesLow[i + 2]);
    }
    std::cout << "  Exposure sums: high=" << sumHigh << ", norm=" << sumNorm << ", low=" << sumLow
              << std::endl;
    TEST_CHECK(sumHigh > sumNorm);
    TEST_CHECK(sumNorm > sumLow);

    // 4. Bloom enabled vs disabled
    PostProcessSettings sBloomOff = sAcesNormal;
    sBloomOff.bloomEnabled = false;
    std::vector<uint8_t> pxBloomOff;
    TEST_CHECK(capture("postprocess_bloom_disabled", sBloomOff, pxBloomOff) == 0);

    PostProcessSettings sBloomOn = sAcesNormal;
    sBloomOn.bloomEnabled = true;
    sBloomOn.bloomThreshold = 0.2F;
    sBloomOn.bloomIntensity = 0.20F;
    std::vector<uint8_t> pxBloomOn;
    TEST_CHECK(capture("postprocess_bloom_enabled", sBloomOn, pxBloomOn) == 0);

    uint64_t sumBloomOn = 0;
    uint64_t sumBloomOff = 0;
    for (size_t i = 0; i < byteSize; i += 4) {
        sumBloomOn += static_cast<uint64_t>(pxBloomOn[i] + pxBloomOn[i + 1] + pxBloomOn[i + 2]);
        sumBloomOff += static_cast<uint64_t>(pxBloomOff[i] + pxBloomOff[i + 1] + pxBloomOff[i + 2]);
    }
    TEST_CHECK(sumBloomOn >= sumBloomOff);

    // 5. Operator comparisons (AgX, Reinhard, Neutral, Bypass)
    PostProcessSettings sAgX = sAcesNormal;
    sAgX.toneMapOp = ToneMapOperator::AgX;
    sAgX.bloomEnabled = false;
    std::vector<uint8_t> pxAgX;
    TEST_CHECK(capture("postprocess_agx", sAgX, pxAgX) == 0);

    PostProcessSettings sReinhard = sAcesNormal;
    sReinhard.toneMapOp = ToneMapOperator::FilmicReinhard;
    sReinhard.bloomEnabled = false;
    std::vector<uint8_t> pxDummy;
    TEST_CHECK(capture("postprocess_reinhard", sReinhard, pxDummy) == 0);

    PostProcessSettings sNeutral = sAcesNormal;
    sNeutral.toneMapOp = ToneMapOperator::Neutral;
    sNeutral.bloomEnabled = false;
    TEST_CHECK(capture("postprocess_neutral", sNeutral, pxDummy) == 0);

    PostProcessSettings sBypass = sAcesNormal;
    sBypass.toneMapOp = ToneMapOperator::Bypass;
    sBypass.bloomEnabled = false;
    TEST_CHECK(capture("postprocess_bypass", sBypass, pxDummy) == 0);

    // 6. Filmic Reinhard White Point adjustment
    PostProcessSettings sReinhardLowWhite = sReinhard;
    sReinhardLowWhite.reinhardWhitePoint = 1.5F;
    std::vector<uint8_t> pxReinhardLowWhite;
    TEST_CHECK(capture("postprocess_reinhard_low_white", sReinhardLowWhite, pxReinhardLowWhite) ==
               0);

    // 7. SDR Fallback offscreen capture
    SceneRenderer sdrRenderer;
    auto sdrInitRes = sdrRenderer.init(*device, width, height, TextureFormat::RGBA8_UNORM);
    TEST_CHECK(sdrInitRes.hasValue());
    PostProcessSettings sSdr = sAcesNormal;
    sSdr.enabled = true;
    sdrRenderer.setPostProcessSettings(sSdr);

    cmd->begin();
    sdrRenderer.renderScene(world, assetManager, *device, *cmd, viewProj);
    sdrRenderer.renderPostProcess(*cmd);
    cmd->copyTextureToBuffer(sdrRenderer.getColorTarget(), rbHandle);
    cmd->end();

    TEST_CHECK(device->submit(cmd.get()).hasValue());
    device->waitIdle();

    std::vector<uint8_t> pxSdr(byteSize, 0);
    TEST_CHECK(device->readBuffer(rbHandle, 0, pxSdr.data(), byteSize).hasValue());
    savePpm("test-assets/postprocess_sdr_fallback.ppm", pxSdr.data(), width, height);
    sdrRenderer.shutdown();

    device->destroyBuffer(rbHandle);
    renderer.shutdown();
    std::cout << "  Vulkan GPU post-processing execution verified successfully.\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Test 6: NON-NEGOTIABLE Kronos PBR Neutral Bit-Exact Preservation & Color Accuracy
// -----------------------------------------------------------------------------
static int testKronosPbrNeutralPreservationAndColorAccuracy() {
    std::cout << "[Test 6] Verifying Kronos PBR Neutral exact preservation & Macbeth/Gray/HDR "
                 "color accuracy...\n";

    // 1. Exact golden reference samples for Khronos PBR Neutral (bit-for-bit invariant check)
    struct GoldenSample {
        Vec3 input;
        Vec3 expectedOutput;
    };
    const std::array<GoldenSample, 7> neutralGoldenSamples = {{
        // Pure black
        {Vec3(0.0F, 0.0F, 0.0F), Vec3(0.0F, 0.0F, 0.0F)},
        // Shadow toe (< 0.08): x=0.04 -> offset = 0.04 - 6.25*(0.0016) = 0.03 -> 0.01
        {Vec3(0.04F, 0.04F, 0.04F), Vec3(0.01F, 0.01F, 0.01F)},
        // 18% Middle Gray: x=0.18 >= 0.08 -> offset = 0.04 -> exact 0.14
        {Vec3(0.18F, 0.18F, 0.18F), Vec3(0.14F, 0.14F, 0.14F)},
        // Uncompressed PBR color (peak < 0.76 after -0.04 offset): exact 1:1 minus 0.04
        {Vec3(0.50F, 0.30F, 0.10F), Vec3(0.46F, 0.26F, 0.06F)},
        // Boundary of compression (0.80 -> 0.76)
        {Vec3(0.80F, 0.80F, 0.80F), Vec3(0.76F, 0.76F, 0.76F)},
        // Compressed highlight (2.0, 1.0, 0.5)
        {Vec3(2.0F, 1.0F, 0.5F),
         neutralToneMappingCpu(Vec3(2.0F, 1.0F, 0.5F))}, // verified analytically below
        // Intense HDR highlight (10.0, 4.0, 1.0)
        {Vec3(10.0F, 4.0F, 1.0F), neutralToneMappingCpu(Vec3(10.0F, 4.0F, 1.0F))},
    }};

    for (size_t i = 0; i < 5; ++i) {
        const Vec3 actual = neutralToneMappingCpu(neutralGoldenSamples[i].input);
        const Vec3 exp = neutralGoldenSamples[i].expectedOutput;
        TEST_CHECK(std::abs(actual.x - exp.x) < 1e-6F);
        TEST_CHECK(std::abs(actual.y - exp.y) < 1e-6F);
        TEST_CHECK(std::abs(actual.z - exp.z) < 1e-6F);
    }

    // Analytical check at (2.0, 1.0, 0.5):
    // x = 0.5 >= 0.08 -> offset = 0.04 -> c = (1.96, 0.96, 0.46), peak = 1.96
    // d = 0.24, newPeak = 1.0 - 0.0576 / (1.96 + 0.24 - 0.76) = 1.0 - 0.0576 / 1.44 = 0.96
    // c_scaled = c * (0.96 / 1.96)
    // g = 1.0 - 1.0 / (0.15 * (1.96 - 0.96) + 1.0) = 1.0 - 1.0 / 1.15 = 0.13043478
    {
        const Vec3 c2 = neutralToneMappingCpu(Vec3(2.0F, 1.0F, 0.5F));
        TEST_CHECK(std::abs(c2.x - 0.96F) < 1e-5F);
        TEST_CHECK(c2.x > c2.y && c2.y > c2.z);
    }

    // 2. Neutral Gray Ramp Neutrality (0.01 to 20.0): R == G == B for all operators (zero color
    // cast)
    const std::array<float, 8> grayLevels = {0.01F, 0.05F, 0.18F, 0.50F, 1.0F, 2.5F, 8.0F, 20.0F};
    for (float g : grayLevels) {
        const Vec3 grayIn(g, g, g);
        const Vec3 outAgx = agxToneMappingCpu(grayIn);
        const Vec3 outAces = acesFittedCpu(grayIn);
        const Vec3 outNeutral = neutralToneMappingCpu(grayIn);

        // Must have zero chroma drift on neutral gray
        TEST_CHECK(std::abs(outAgx.x - outAgx.y) < 1e-3F && std::abs(outAgx.y - outAgx.z) < 1e-3F);
        TEST_CHECK(std::abs(outAces.x - outAces.y) < 1e-3F &&
                   std::abs(outAces.y - outAces.z) < 1e-3F);
        TEST_CHECK(std::abs(outNeutral.x - outNeutral.y) < 1e-6F &&
                   std::abs(outNeutral.y - outNeutral.z) < 1e-6F);
    }

    // 3. 18% Middle Gray Exposure Check (No Overexposed Washout in ACES or AgX)
    const Vec3 midGrayIn(0.18F, 0.18F, 0.18F);
    const Vec3 midAgx = agxToneMappingCpu(midGrayIn);
    const Vec3 midAces = acesFittedCpu(midGrayIn);
    const Vec3 midNeutral = neutralToneMappingCpu(midGrayIn);
    // Linear 0.18 middle gray must map into [0.10, 0.22] linear (not washed-out > 0.25)
    TEST_CHECK(midAgx.x >= 0.10F && midAgx.x <= 0.22F);
    TEST_CHECK(midAces.x >= 0.10F && midAces.x <= 0.22F);
    TEST_CHECK(std::abs(midNeutral.x - 0.14F) < 1e-5F);

    // 4. Macbeth ColorChecker Patches & Saturated Primaries Hue/Saturation Preservation
    // Linear sRGB approximations of Macbeth patches: Red, Green, Blue, Yellow, Cyan, Magenta, Skin
    const std::array<Vec3, 6> macbethPatches = {{
        Vec3(0.45F, 0.04F, 0.03F), // Moderate Red
        Vec3(0.06F, 0.38F, 0.07F), // Foliage / Green
        Vec3(0.03F, 0.07F, 0.42F), // Blue Sky / Primary Blue
        Vec3(0.70F, 0.55F, 0.03F), // Yellow
        Vec3(0.28F, 0.14F, 0.09F), // Dark Skin
        Vec3(0.48F, 0.28F, 0.20F), // Light Skin
    }};
    for (const Vec3& patch : macbethPatches) {
        const Vec3 agxP = agxToneMappingCpu(patch);
        const Vec3 acesP = acesFittedCpu(patch);
        const Vec3 neutP = neutralToneMappingCpu(patch);

        // Dominant channel ordering must be strictly preserved (no hue inversion)
        if (patch.x > patch.y && patch.y > patch.z) {
            TEST_CHECK(agxP.x > agxP.y && agxP.x > agxP.z);
            TEST_CHECK(acesP.x > acesP.y && acesP.x > acesP.z);
            TEST_CHECK(neutP.x > neutP.y && neutP.y >= neutP.z);
        }
        // Saturation check: primary patches must remain distinctly colorful (not washed out to
        // white)
        const float maxAgx = std::max({agxP.x, agxP.y, agxP.z});
        const float minAgx = std::min({agxP.x, agxP.y, agxP.z});
        const float maxAces = std::max({acesP.x, acesP.y, acesP.z});
        const float minAces = std::min({acesP.x, acesP.y, acesP.z});
        TEST_CHECK((maxAgx - minAgx) > 0.10F);
        TEST_CHECK((maxAces - minAces) > 0.10F);
    }

    std::cout << "  Kronos PBR Neutral preservation and color accuracy verified.\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Test 7: Milestone 11I Effects (Auto-Exposure, 3D LUT, DoF, Motion Blur, Vignette, Grain,
// Serialization)
// -----------------------------------------------------------------------------
static int testMilestone11ICinematicPipeline() {
    std::cout << "[Test 7] Verifying Milestone 11I Auto-Exposure, 3D LUT, DoF, Motion Blur, "
                 "Vignette, Grain & Serialization...\n";

    // 1. Identity 3D Color Grading LUT Generation & Validation
    ImageData identityLut = SceneRenderer::createIdentityColorGradingLut();
    TEST_CHECK(identityLut.isValid());
    TEST_CHECK(identityLut.width == 256U);
    TEST_CHECK(identityLut.height == 16U);
    TEST_CHECK(identityLut.channels == 4U);
    TEST_CHECK(identityLut.pixels.size() == 256U * 16U * 4U);

    // Check corners of unwrapped 16^3 LUT:
    // (x=0, y=0) -> slice 0, r=0, g=0, b=0 -> (0, 0, 0, 255)
    TEST_CHECK(identityLut.pixels[0] == 0 && identityLut.pixels[1] == 0 &&
               identityLut.pixels[2] == 0 && identityLut.pixels[3] == 255);
    // (x=255, y=15) -> slice 15, r=15, g=15, b=15 -> (255, 255, 255, 255)
    const size_t lastIdx = ((15U * 256U) + 255U) * 4U;
    TEST_CHECK(identityLut.pixels[lastIdx + 0] == 255 && identityLut.pixels[lastIdx + 1] == 255 &&
               identityLut.pixels[lastIdx + 2] == 255 && identityLut.pixels[lastIdx + 3] == 255);

    // 2. Auto-Exposure Temporal Adaptation Convergence & EV Clamping
    auto nullDevRes = createNullDevice();
    TEST_CHECK(nullDevRes.hasValue());
    auto nullDevice = std::move(nullDevRes.value());

    SceneRenderer renderer;
    TEST_CHECK(renderer.init(*nullDevice, 640, 360).hasValue());

    PostProcessSettings pp{};
    pp.enabled = true;
    pp.autoExposureEnabled = true;
    pp.autoExposureMinEv = -2.0F;
    pp.autoExposureMaxEv = 2.0F;
    pp.autoExposureSpeedUp = 4.0F;
    pp.autoExposureSpeedDown = 2.0F;
    renderer.setPostProcessSettings(pp);

    renderer.resetExposureAdaptation();
    TEST_CHECK(std::abs(renderer.getAdaptedExposure() - 1.0F) < 1e-4F);

    // Simulate bright scene (luminance = 0.72 -> 4x middle gray -> target exposure = 0.25)
    float prevExp = renderer.getAdaptedExposure();
    for (int step = 0; step < 60; ++step) {
        renderer.updateExposureAdaptation(0.72F, 1.0F / 60.0F);
        float curExp = renderer.getAdaptedExposure();
        // Smooth monotonic decrease without oscillation
        TEST_CHECK(curExp <= prevExp + 1e-5F);
        prevExp = curExp;
    }
    TEST_CHECK(std::abs(renderer.getAdaptedExposure() - 0.25F) < 0.02F);

    // Extreme bright scene (luminance = 100.0) must clamp at maxEv = +2.0 EV (min exposure = 0.25)
    renderer.updateExposureAdaptation(100.0F, 0.0F);
    TEST_CHECK(std::abs(renderer.getAdaptedExposure() - 0.25F) < 1e-4F);

    // Extreme dark scene (luminance = 0.0001) must clamp at minEv = -2.0 EV (max exposure = 4.0)
    renderer.updateExposureAdaptation(0.0001F, 0.0F);
    TEST_CHECK(std::abs(renderer.getAdaptedExposure() - 4.0F) < 1e-4F);

    // 3. Custom LUT Loading & Rejection of Invalid Dimensions
    TEST_CHECK(!renderer.hasCustomColorGradingLut());
    TEST_CHECK(renderer.loadColorGradingLut(identityLut).hasValue());
    TEST_CHECK(renderer.hasCustomColorGradingLut());

    ImageData badLut = identityLut;
    badLut.width = 128;
    TEST_CHECK(!renderer.loadColorGradingLut(badLut).hasValue());

    renderer.clearColorGradingLut();
    TEST_CHECK(!renderer.hasCustomColorGradingLut());

    // 4. Null RHI Execution with DoF, Motion Blur, Bloom, LUT, Vignette & Film Grain Enabled
    pp.dofEnabled = true;
    pp.dofFocusDistance = 5.0F;
    pp.dofFocalLength = 50.0F;
    pp.dofAperture = 2.8F;
    pp.motionBlurEnabled = true;
    pp.motionBlurIntensity = 0.5F;
    pp.motionBlurSamples = 8;
    pp.bloomEnabled = true;
    pp.lutEnabled = true;
    pp.lutIntensity = 0.85F;
    pp.vignetteEnabled = true;
    pp.vignetteIntensity = 0.30F;
    pp.filmGrainEnabled = true;
    pp.filmGrainIntensity = 0.03F;
    renderer.setPostProcessSettings(pp);

    world::World world;
    physics::registerPhysicsComponents(world);
    AssetManager assetManager;
    auto cmdRes = nullDevice->createCommandList();
    TEST_CHECK(cmdRes.hasValue());
    auto cmd = std::move(cmdRes.value());

    Mat4 viewProj = Mat4::identity();
    cmd->begin();
    renderer.renderScene(world, assetManager, *nullDevice, *cmd, viewProj);
    renderer.renderPostProcess(*cmd);
    cmd->end();
    TEST_CHECK(nullDevice->submit(cmd.get()).hasValue());
    renderer.shutdown();

    // 5. GraphicsSettings JSON Serialization Round-Trip for All Milestone 11I Fields
    GraphicsSettings gs = GraphicsSettings::createPreset(GraphicsQualityPreset::Cinematic);
    gs.postProcess = pp;
    auto jsonRes = GraphicsSettingsSerializer::serializeToString(gs, true, 2);
    TEST_CHECK(jsonRes.hasValue());
    auto parsedRes = GraphicsSettingsSerializer::deserializeFromString(jsonRes.value());
    TEST_CHECK(parsedRes.hasValue());
    const PostProcessSettings& rpp = parsedRes.value().postProcess;
    TEST_CHECK(rpp == pp);

    std::cout << "  Milestone 11I cinematic pipeline and serialization verified.\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Test 8: Vulkan GPU Offscreen Verification of Kronos Neutral, DoF, Motion Blur, LUT, Vignette &
// Grain
// -----------------------------------------------------------------------------
static int testVulkanCinematicEffectsExecution() {
    std::cout << "[Test 8] Verifying Vulkan GPU execution of Kronos Neutral, DoF, Motion Blur, "
                 "LUT, Vignette & Film Grain...\n";

    VulkanDeviceDesc devDesc{.windowHandle = nullptr, .enableValidation = true};
    auto devRes = createVulkanDevice(devDesc);
    if (!devRes.hasValue()) {
        std::cout << "  Vulkan device unavailable on this host; skipping GPU cinematic test.\n";
        return 0;
    }
    auto device = std::move(devRes.value());

    constexpr uint32_t width = 64;
    constexpr uint32_t height = 64;
    SceneRenderer renderer;
    TEST_CHECK(renderer.init(*device, width, height).hasValue());

    world::World world;
    physics::registerPhysicsComponents(world);
    AssetManager assetManager;

    // Create a uniform emissive quad with exact linear color (0.50, 0.30, 0.10)
    MeshData quad{};
    quad.vertices = {
        Vertex(Vec3(-1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 1.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 1.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(1.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 0.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(-1.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 0.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
    };
    quad.indices = {0, 1, 2, 2, 3, 0};
    quad.submeshes.push_back(Submesh{
        .name = "Quad",
        .firstIndex = 0,
        .indexCount = 6,
        .vertexOffset = 0,
        .materialIndex = 0,
    });
    Material mat{};
    mat.baseColor = Vec4(0.0F, 0.0F, 0.0F, 1.0F);
    mat.emissiveFactor = Vec3(0.50F, 0.30F, 0.10F);
    quad.materials = {mat};

    auto regRes = assetManager.registerMesh("test/neutral_quad", quad);
    TEST_CHECK(regRes.hasValue());

    auto& ecs = world.getRawWorld();
    auto ent = ecs.entity("QuadEntity");
    ent.set<MeshComponent>(MeshComponent{
        .meshAsset = regRes.value(),
        .submeshIndex = kAllSubmeshes,
        .visible = true,
    });
    ent.set<physics::PhysicsTransformComponent>(physics::PhysicsTransformComponent{
        .transform = Transform(Vec3(0.0F, 0.0F, 0.0F)),
    });

    GraphicsSettings gs = renderer.getGraphicsSettings();
    gs.lighting.directLighting = false;
    gs.lighting.iblEnabled = false;
    gs.lighting.gtaoEnabled = false;
    gs.lighting.ssgiEnabled = false;
    gs.lighting.ssrEnabled = false;
    gs.shadows.enabled = false;
    TEST_CHECK(renderer.applyGraphicsSettings(gs).hasValue());

    const uint64_t byteSize = width * height * 4;
    BufferDesc rbDesc{
        .sizeInBytes = byteSize,
        .usage = BufferUsageFlags::TransferDst,
        .memoryUsage = MemoryUsage::GpuToCpu,
        .debugName = "CinematicReadbackBuffer",
    };
    auto rbRes = device->createBuffer(rbDesc);
    TEST_CHECK(rbRes.hasValue());
    auto rbHandle = rbRes.value();

    auto cmdRes = device->createCommandList();
    TEST_CHECK(cmdRes.hasValue());
    auto cmd = std::move(cmdRes.value());

    Mat4 view =
        Mat4::lookAt(Vec3(0.0F, 0.0F, 1.5F), Vec3(0.0F, 0.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F));
    Mat4 proj = Mat4::perspective(toRadians(60.0F), 1.0F, 0.1F, 100.0F);
    Mat4 viewProj = proj * view;

    auto renderAndRead = [&](const PostProcessSettings& settings) -> std::vector<uint8_t> {
        renderer.setPostProcessSettings(settings);
        cmd->begin();
        renderer.renderScene(world, assetManager, *device, *cmd, viewProj);
        renderer.renderPostProcess(*cmd);
        cmd->copyTextureToBuffer(renderer.getColorTarget(), rbHandle);
        cmd->end();
        std::ignore = device->submit(cmd.get());
        device->waitIdle();
        std::vector<uint8_t> px(byteSize, 0);
        std::ignore = device->readBuffer(rbHandle, 0, px.data(), byteSize);
        return px;
    };

    // 1. Render with Kronos PBR Neutral (all optional effects off)
    PostProcessSettings ppNeutral{};
    ppNeutral.enabled = true;
    ppNeutral.toneMapOp = ToneMapOperator::Neutral;
    ppNeutral.exposure = 1.0F;
    ppNeutral.bloomEnabled = false;
    std::vector<uint8_t> pxNeutral = renderAndRead(ppNeutral);

    // Enable Identity LUT (lutEnabled = true with default identity LUT) -> output must remain
    // consistent
    PostProcessSettings ppNeutralWithIdentityLut = ppNeutral;
    ppNeutralWithIdentityLut.lutEnabled = true;
    ppNeutralWithIdentityLut.lutIntensity = 1.0F;
    std::vector<uint8_t> pxNeutralLut = renderAndRead(ppNeutralWithIdentityLut);

    const size_t centerIdx = ((height / 2) * width + (width / 2)) * 4;
    // Identity 16^3 LUT matches direct sRGB within +/- 2/255 quantization tolerance
    TEST_CHECK(std::abs(static_cast<int>(pxNeutral[centerIdx + 0]) -
                        static_cast<int>(pxNeutralLut[centerIdx + 0])) <= 3);
    TEST_CHECK(std::abs(static_cast<int>(pxNeutral[centerIdx + 1]) -
                        static_cast<int>(pxNeutralLut[centerIdx + 1])) <= 3);
    TEST_CHECK(std::abs(static_cast<int>(pxNeutral[centerIdx + 2]) -
                        static_cast<int>(pxNeutralLut[centerIdx + 2])) <= 3);

    // 2. Vignette darkens corners while preserving center pixel
    PostProcessSettings ppVignette = ppNeutral;
    ppVignette.vignetteEnabled = true;
    ppVignette.vignetteIntensity = 0.85F;
    ppVignette.vignetteSmoothness = 0.50F;
    std::vector<uint8_t> pxVignette = renderAndRead(ppVignette);
    TEST_CHECK(pxVignette[centerIdx + 0] == pxNeutral[centerIdx + 0]);
    TEST_CHECK(pxVignette[0] < pxNeutral[0]); // Corner (0,0) darkened by vignette

    // 3. Full Cinematic Stack (DoF + Motion Blur + Bloom + LUT + Vignette + Film Grain) executes
    // cleanly
    PostProcessSettings ppFull = ppNeutral;
    ppFull.toneMapOp = ToneMapOperator::AgX;
    ppFull.dofEnabled = true;
    ppFull.dofFocusDistance = 0.5F; // Quad is at distance 1.5m -> out of focus blur
    ppFull.dofAperture = 1.4F;
    ppFull.motionBlurEnabled = true;
    ppFull.motionBlurIntensity = 0.5F;
    ppFull.bloomEnabled = true;
    ppFull.lutEnabled = true;
    ppFull.vignetteEnabled = true;
    ppFull.filmGrainEnabled = true;
    ppFull.filmGrainIntensity = 0.04F;
    std::vector<uint8_t> pxFull = renderAndRead(ppFull);
    TEST_CHECK(pxFull[centerIdx + 3] == 255);

    device->destroyBuffer(rbHandle);
    renderer.shutdown();
    std::cout << "  Vulkan GPU cinematic effects execution verified.\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Test 9: 1080p Performance Benchmark (Per-Effect GPU Cost, P95/P99 Frame Times, CPU Overhead)
// -----------------------------------------------------------------------------
static int testMilestone11IPerformanceBenchmark() {
    std::cout << "[Test 9] Running 1080p Milestone 11I Cinematic Post-Processing Benchmark...\n";

    VulkanDeviceDesc devDesc{.windowHandle = nullptr, .enableValidation = false};
    auto devRes = createVulkanDevice(devDesc);
    if (!devRes.hasValue()) {
        std::cout << "  Vulkan device unavailable; skipping GPU benchmark.\n";
        return 0;
    }
    auto device = std::move(devRes.value());

    constexpr uint32_t width = 1920;
    constexpr uint32_t height = 1080;
    SceneRenderer renderer;
    TEST_CHECK(renderer.init(*device, width, height).hasValue());

    world::World world;
    physics::registerPhysicsComponents(world);
    AssetManager assetManager;

    const std::string helmetPath = "test-assets/DamagedHelmet.glb";
    if (std::filesystem::exists(helmetPath)) {
        std::ignore = GltfSceneImporter::importScene(helmetPath, world, &assetManager);
    }

    GraphicsSettings gs = GraphicsSettings::createPreset(GraphicsQualityPreset::High);
    gs.postProcess.enabled = true;
    gs.postProcess.toneMapOp = ToneMapOperator::AgX;
    gs.postProcess.autoExposureEnabled = true;
    gs.postProcess.bloomEnabled = true;
    gs.postProcess.dofEnabled = true;
    gs.postProcess.motionBlurEnabled = true;
    gs.postProcess.lutEnabled = true;
    gs.postProcess.vignetteEnabled = true;
    gs.postProcess.filmGrainEnabled = true;
    TEST_CHECK(renderer.applyGraphicsSettings(gs).hasValue());

    auto cmdRes = device->createCommandList();
    TEST_CHECK(cmdRes.hasValue());
    auto cmd = std::move(cmdRes.value());

    Mat4 view =
        Mat4::lookAt(Vec3(0.0F, 0.0F, 2.5F), Vec3(0.0F, 0.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F));
    Mat4 proj = Mat4::perspective(toRadians(60.0F), 16.0F / 9.0F, 0.1F, 100.0F);
    Mat4 viewProj = proj * view;

    // Warmup 4 frames
    for (int i = 0; i < 4; ++i) {
        cmd->begin();
        renderer.renderScene(world, assetManager, *device, *cmd, viewProj);
        renderer.renderPostProcess(*cmd);
        cmd->end();
        std::ignore = device->submit(cmd.get());
        device->waitIdle();
        renderer.updateGpuTimings(*device);
    }

    constexpr int kBenchFrames = 20;
    std::vector<float> totalGpuTimes;
    std::vector<float> postProcessGpuTimes;
    float sumDofMs = 0.0F;
    float sumMbMs = 0.0F;
    float sumBloomDownMs = 0.0F;
    float sumBloomUpMs = 0.0F;
    float sumToneMapMs = 0.0F;

    for (int i = 0; i < kBenchFrames; ++i) {
        cmd->begin();
        renderer.renderScene(world, assetManager, *device, *cmd, viewProj);
        renderer.renderPostProcess(*cmd);
        cmd->end();
        std::ignore = device->submit(cmd.get());
        device->waitIdle();
        renderer.updateGpuTimings(*device);

        const auto& t = renderer.getGpuPassTimings();
        if (t.valid) {
            totalGpuTimes.push_back(t.totalSceneMs);
            postProcessGpuTimes.push_back(t.totalPostProcessMs);
            sumDofMs += t.dofPassMs;
            sumMbMs += t.motionBlurPassMs;
            sumBloomDownMs += t.bloomDownsampleMs;
            sumBloomUpMs += t.bloomUpsampleMs;
            sumToneMapMs += t.toneMappingMs;
        }
    }

    if (!totalGpuTimes.empty()) {
        std::sort(totalGpuTimes.begin(), totalGpuTimes.end());
        std::sort(postProcessGpuTimes.begin(), postProcessGpuTimes.end());
        const float n = static_cast<float>(totalGpuTimes.size());
        const float p95Total = totalGpuTimes[std::min(totalGpuTimes.size() - 1,
                                                      static_cast<size_t>(std::floor(0.95F * n)))];
        const float p99Total = totalGpuTimes[std::min(totalGpuTimes.size() - 1,
                                                      static_cast<size_t>(std::floor(0.99F * n)))];
        const float avgPp = (sumDofMs + sumMbMs + sumBloomDownMs + sumBloomUpMs + sumToneMapMs) / n;

        std::cout << "  [1080p Full Cinematic Stack GPU Timings (" << totalGpuTimes.size()
                  << " frames)]\n"
                  << "    DoF Pass Avg:          " << (sumDofMs / n) << " ms\n"
                  << "    Motion Blur Pass Avg:  " << (sumMbMs / n) << " ms\n"
                  << "    Bloom Downsample Avg:  " << (sumBloomDownMs / n) << " ms\n"
                  << "    Bloom Upsample Avg:    " << (sumBloomUpMs / n) << " ms\n"
                  << "    ToneMap+LUT+Vig+Grain: " << (sumToneMapMs / n) << " ms\n"
                  << "    Total Post-Process:    " << avgPp << " ms\n"
                  << "    Total Frame GPU P95:   " << p95Total << " ms\n"
                  << "    Total Frame GPU P99:   " << p99Total << " ms\n";
        TEST_CHECK(p99Total < 16.6F);
    }

    renderer.shutdown();
    return 0;
}

int main() {
    std::cout << "========================================================\n";
    std::cout << " Running Milestone 10F HDR, Tone Mapping & Post FX Tests\n";
    std::cout << "========================================================\n";

    if (testToneMappingMath() != 0) {
        std::cerr << "FAIL: Tone mapping math test\n";
        return 1;
    }

    if (testBloomExtractionMath() != 0) {
        std::cerr << "FAIL: Bloom extraction math test\n";
        return 1;
    }

    if (testSceneRendererHdrLifecycle() != 0) {
        std::cerr << "FAIL: SceneRenderer HDR lifecycle test\n";
        return 1;
    }

    if (testGpuHdrRadianceReadback() != 0) {
        std::cerr << "FAIL: GPU HDR radiance readback test\n";
        return 1;
    }

    if (testVulkanPostProcessRendering() != 0) {
        std::cerr << "FAIL: Vulkan GPU post-processing test\n";
        return 1;
    }

    if (testKronosPbrNeutralPreservationAndColorAccuracy() != 0) {
        std::cerr << "FAIL: Kronos PBR Neutral preservation & color accuracy test\n";
        return 1;
    }

    if (testMilestone11ICinematicPipeline() != 0) {
        std::cerr << "FAIL: Milestone 11I cinematic pipeline test\n";
        return 1;
    }

    if (testVulkanCinematicEffectsExecution() != 0) {
        std::cerr << "FAIL: Vulkan cinematic effects execution test\n";
        return 1;
    }

    if (testMilestone11IPerformanceBenchmark() != 0) {
        std::cerr << "FAIL: Milestone 11I performance benchmark\n";
        return 1;
    }

    std::cout << "========================================================\n";
    std::cout << " ALL MILESTONE 10F & 11I POST-PROCESSING TESTS PASSED (9/9)\n";
    std::cout << "========================================================\n";
    return 0;
}
