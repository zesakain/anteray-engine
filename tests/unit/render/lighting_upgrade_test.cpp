/// @file lighting_upgrade_test.cpp
/// @brief Physical and visual verification tests for Daylight, Sunset, and Night atmosphere
/// (Milestone 11C).

#include "core/assert.h"
#include "core/math.h"
#include "render/environment_map.h"
#include "render/gpu_environment_map.h"
#include "render/scene_renderer.h"
#include "rhi/rhi_device.h"
#include "world/world.h"

#include <cmath>
#include <iostream>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ANTERAY_HAS_ASAN 1
#endif
#if __has_feature(thread_sanitizer)
#define ANTERAY_HAS_TSAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define ANTERAY_HAS_ASAN 1
#endif
#if defined(__SANITIZE_THREAD__)
#define ANTERAY_HAS_TSAN 1
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

namespace {

static int testAtmosphereParameterGeneration() {
    std::cout << "  [1/4] Verifying atmospheric presets and parameter invariants...\n";

    const auto day = AtmosphereParameters::createDaylight();
    TEST_CHECK(day.timeOfDay == EnvironmentTimeOfDay::Daylight);
    TEST_CHECK(std::abs(day.sunDirection.length() - 1.0F) < 1e-4F);
    TEST_CHECK(day.sunIntensity > 0.0F);
    TEST_CHECK(day.zenithColor.z > day.zenithColor.x); // Deep blue zenith
    TEST_CHECK(day.whiteBalance == 6500.0F);

    const auto sunset = AtmosphereParameters::createSunset();
    TEST_CHECK(sunset.timeOfDay == EnvironmentTimeOfDay::Sunset);
    TEST_CHECK(std::abs(sunset.sunDirection.length() - 1.0F) < 1e-4F);
    TEST_CHECK(sunset.sunIntensity > 0.0F);
    TEST_CHECK(sunset.horizonColor.x > sunset.horizonColor.z); // Amber/crimson horizon
    TEST_CHECK(sunset.whiteBalance < 6000.0F);                 // Warm golden hour shift

    const auto night = AtmosphereParameters::createNight();
    TEST_CHECK(night.timeOfDay == EnvironmentTimeOfDay::Night);
    TEST_CHECK(std::abs(night.sunDirection.length() - 1.0F) < 1e-4F);
    TEST_CHECK(night.sunIntensity < day.sunIntensity);                      // Moonlight is delicate
    TEST_CHECK(night.zenithColor.x < 0.05F && night.zenithColor.y < 0.05F); // Dark midnight sky
    TEST_CHECK(night.whiteBalance > 7000.0F);                               // Cold scotopic shift

    return 0;
}

static int testAtmosphereImageGeneration() {
    std::cout << "  [2/4] Verifying procedural atmosphere HDR generation...\n";

    const auto dayParams = AtmosphereParameters::createDaylight();
    const HdrImage dayImg = IblProcessor::generateAtmosphere(dayParams, 128, 64);
    TEST_CHECK(dayImg.isValid());
    TEST_CHECK(dayImg.width == 128 && dayImg.height == 64);

    const auto sunsetParams = AtmosphereParameters::createSunset();
    const HdrImage sunsetImg = IblProcessor::generateAtmosphere(sunsetParams, 128, 64);
    TEST_CHECK(sunsetImg.isValid());

    const auto nightParams = AtmosphereParameters::createNight();
    const HdrImage nightImg = IblProcessor::generateAtmosphere(nightParams, 128, 64);
    TEST_CHECK(nightImg.isValid());

    // Physical checks on sampled rays
    const Vec3 dayZenith = dayImg.sampleEquirectangular(Vec3(0.0F, 1.0F, 0.0F));
    const Vec3 nightZenith = nightImg.sampleEquirectangular(Vec3(0.0F, 1.0F, 0.0F));
    TEST_CHECK(dayZenith.y > nightZenith.y * 5.0F);
    TEST_CHECK(dayZenith.z > nightZenith.z * 5.0F);

    // Sunset horizon in sun azimuth direction should be richer in red/amber than blue
    const Vec3 sunsetHorizon = sunsetImg.sampleEquirectangular(sunsetParams.sunDirection);
    TEST_CHECK(sunsetHorizon.x > sunsetHorizon.z);

    return 0;
}

static int testAtmosphereCubemapAndConvolution() {
    std::cout << "  [3/4] Verifying atmosphere cubemap and split-sum convolution...\n";

    const auto sunsetParams = AtmosphereParameters::createSunset();
    const HdrImage sunsetImg = IblProcessor::generateAtmosphere(sunsetParams, 64, 32);

    const CubemapData cubemap = IblProcessor::equirectangularToCubemap(sunsetImg, 32);
    TEST_CHECK(cubemap.isValid());
    TEST_CHECK(cubemap.size == 32);

    const CubemapData irradiance = IblProcessor::convolveDiffuseIrradiance(cubemap, 16, 0.15F);
    TEST_CHECK(irradiance.isValid());
    TEST_CHECK(irradiance.size == 16);

    const CubemapData prefilter = IblProcessor::prefilterSpecularEnvironment(cubemap, 32, 3, 32);
    TEST_CHECK(prefilter.isValid());
    TEST_CHECK(prefilter.mipLevels == 3);

    return 0;
}

static int testSceneRendererAtmosphereTransitions(RhiDevice& device) {
    std::cout << "  [4/4] Verifying SceneRenderer live atmospheric transitions...\n";

    SceneRenderer renderer{};
    auto initRes = renderer.init(device, 64, 64);
    TEST_CHECK(initRes.hasValue());

    GraphicsSettings settings = GraphicsSettings::createPreset(GraphicsQualityPreset::High);
    settings.lighting.iblEnabled = true;
    TEST_CHECK(renderer.applyGraphicsSettings(settings).hasValue());

    // 1. Daylight transition
    TEST_CHECK(renderer.applyAtmosphere(AtmosphereParameters::createDaylight()).hasValue());
    TEST_CHECK(renderer.getAtmosphereParameters().timeOfDay == EnvironmentTimeOfDay::Daylight);
    TEST_CHECK(renderer.getEnvironmentMap().isValid());

    // 2. Sunset transition
    TEST_CHECK(renderer.applyAtmosphere(AtmosphereParameters::createSunset()).hasValue());
    TEST_CHECK(renderer.getAtmosphereParameters().timeOfDay == EnvironmentTimeOfDay::Sunset);
    TEST_CHECK(renderer.getEnvironmentMap().isValid());

    // 3. Night transition
    world::World world{};
    TEST_CHECK(renderer.applyAtmosphere(world, AtmosphereParameters::createNight()).hasValue());
    TEST_CHECK(renderer.getAtmosphereParameters().timeOfDay == EnvironmentTimeOfDay::Night);
    TEST_CHECK(renderer.getEnvironmentMap().isValid());

    // Verify DirectionalLightComponent was synchronized in the World
    bool foundSunMoon = false;
    float foundIntensity = 0.0F;
    bool foundCastShadows = false;
    world.getRawWorld().query<const DirectionalLightComponent>().each(
        [&](flecs::entity, const DirectionalLightComponent& comp) {
            foundSunMoon = true;
            foundIntensity = comp.intensity;
            foundCastShadows = comp.castShadows;
        });
    TEST_CHECK(foundSunMoon);
    TEST_CHECK(foundIntensity < 2.0F); // Moonlight intensity
    TEST_CHECK(foundCastShadows);

    renderer.shutdown();
    return 0;
}

} // namespace

int main() {
    std::cout << "[Running Milestone 11C Atmosphere & Lighting Upgrade Suite]\n";

    if (testAtmosphereParameterGeneration() != 0) {
        return 1;
    }
    if (testAtmosphereImageGeneration() != 0) {
        return 2;
    }
    if (testAtmosphereCubemapAndConvolution() != 0) {
        return 3;
    }

#if defined(ANTERAY_HAS_TSAN)
    std::cout << "ThreadSanitizer active: skipping live GPU atmosphere tests\n";
    return 0;
#endif

    VulkanDeviceDesc devDesc{.windowHandle = nullptr, .enableValidation = true};
    auto devRes = createVulkanDevice(devDesc);
    if (!devRes.hasValue()) {
        std::cout << "Vulkan device unavailable; skipping hardware atmosphere tests\n";
        return 0;
    }
    auto device = std::move(devRes.value());

    if (testSceneRendererAtmosphereTransitions(*device) != 0) {
        return 4;
    }

    std::cout << "[All Milestone 11C Atmosphere & Lighting Tests Passed: 4/4]\n";
    return 0;
}
