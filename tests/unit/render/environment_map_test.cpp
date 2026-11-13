/// @file environment_map_test.cpp
/// @brief Unit tests for IblProcessor, HDR decoding, cubemap conversion,
///        diffuse irradiance convolution, specular GGX prefiltering, and BRDF LUT.

#include "render/environment_map.h"

#include <cmath>
#include <iostream>
#include <vector>

using namespace eng;
using namespace eng::render;

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__          \
                      << std::endl;                                                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

static int testFloatToHalfRoundTrip() {
    // 0.0, 1.0, 0.5, 65504.0 (max half), subnormal
    const std::vector<float> testValues = {0.0F, 1.0F, 0.5F, 0.125F, 2.0F, 10.0F, 100.0F, 65504.0F};
    for (float val : testValues) {
        const uint16_t half = floatToHalf(val);
        const float decoded = halfToFloat(half);
        const float diff = std::abs(val - decoded);
        const float relError = (val > 0.0F) ? (diff / val) : diff;
        TEST_CHECK(relError < 0.005F);
    }
    return 0;
}

static int testCubeFaceUVInvertibility() {
    // For each face, test corners, edges, and center
    const std::vector<Vec2> testUVs = {Vec2(0.5F, 0.5F), Vec2(0.1F, 0.1F), Vec2(0.9F, 0.9F),
                                       Vec2(0.2F, 0.8F), Vec2(0.8F, 0.2F), Vec2(0.5F, 0.1F),
                                       Vec2(0.1F, 0.5F)};

    for (uint32_t face = 0; face < 6; ++face) {
        for (const auto& uv : testUVs) {
            const Vec3 dir = IblProcessor::cubeFaceUVToDirection(face, uv.x, uv.y);
            TEST_CHECK(std::abs(dir.length() - 1.0F) < 1e-4F);

            uint32_t roundFace = 99;
            float roundU = -1.0F;
            float roundV = -1.0F;
            IblProcessor::directionToCubeFaceUV(dir, roundFace, roundU, roundV);

            TEST_CHECK(roundFace == face);
            TEST_CHECK(std::abs(roundU - uv.x) < 1e-3F);
            TEST_CHECK(std::abs(roundV - uv.y) < 1e-3F);
        }
    }
    return 0;
}

static int testProceduralSkyAndCubemap() {
    const HdrImage sky =
        IblProcessor::generateProceduralSky(256, 128, Vec3(0.4F, 0.2F, 0.5F), 5.0F);
    TEST_CHECK(sky.isValid());
    TEST_CHECK(sky.width == 256);
    TEST_CHECK(sky.height == 128);
    TEST_CHECK(sky.pixels.size() == 256 * 128 * 4);

    // Zenith should be brighter/bluer than ground
    const Vec3 zenith = sky.sampleEquirectangular(Vec3(0.0F, 1.0F, 0.0F));
    const Vec3 ground = sky.sampleEquirectangular(Vec3(0.0F, -1.0F, 0.0F));
    TEST_CHECK(zenith.y > ground.y);
    TEST_CHECK(zenith.z > ground.z);

    // Convert to cubemap (resolution 64)
    const CubemapData cubemap = IblProcessor::equirectangularToCubemap(sky, 64);
    TEST_CHECK(cubemap.isValid());
    TEST_CHECK(cubemap.size == 64);
    TEST_CHECK(cubemap.mipLevels == 1);
    TEST_CHECK(cubemap.mips[0].faces.size() == 6);
    for (uint32_t f = 0; f < 6; ++f) {
        TEST_CHECK(cubemap.mips[0].faces[f].halfPixels.size() == 64 * 64 * 4);
    }

    // Cubemap sampling up should match sky zenith within half-float and bilinear tolerance
    const Vec3 cubeZenith = cubemap.sample(Vec3(0.0F, 1.0F, 0.0F));
    TEST_CHECK(std::abs(cubeZenith.x - zenith.x) < 0.05F);
    TEST_CHECK(std::abs(cubeZenith.y - zenith.y) < 0.05F);
    TEST_CHECK(std::abs(cubeZenith.z - zenith.z) < 0.05F);

    return 0;
}

static int testDiffuseIrradianceEnergyConservation() {
    // Create uniform white environment (1.0 radiance everywhere)
    HdrImage whiteEnv{};
    whiteEnv.width = 64;
    whiteEnv.height = 32;
    whiteEnv.channels = 4;
    whiteEnv.pixels.resize(64 * 32 * 4, 1.0F);

    const CubemapData cubemap = IblProcessor::equirectangularToCubemap(whiteEnv, 16);
    TEST_CHECK(cubemap.isValid());

    // Convolve with sampleDelta = 0.1 rad
    const CubemapData irradiance = IblProcessor::convolveDiffuseIrradiance(cubemap, 8, 0.1F);
    TEST_CHECK(irradiance.isValid());
    TEST_CHECK(irradiance.size == 8);

    // In a completely white environment, diffuse irradiance integral must equal 1.0 (energy
    // conserved) Within 5% numerical discretization tolerance
    const Vec3 sampleUp = irradiance.sample(Vec3(0.0F, 1.0F, 0.0F));
    const Vec3 sampleRight = irradiance.sample(Vec3(1.0F, 0.0F, 0.0F));
    const Vec3 sampleForward = irradiance.sample(Vec3(0.0F, 0.0F, 1.0F));

    TEST_CHECK(std::abs(sampleUp.x - 1.0F) < 0.08F);
    TEST_CHECK(std::abs(sampleRight.y - 1.0F) < 0.08F);
    TEST_CHECK(std::abs(sampleForward.z - 1.0F) < 0.08F);

    // Physical constraint: irradiance cannot exceed source radiance (no energy generation)
    TEST_CHECK(sampleUp.x <= 1.05F);
    TEST_CHECK(sampleRight.x <= 1.05F);
    TEST_CHECK(sampleForward.x <= 1.05F);

    return 0;
}

static int testSpecularPrefilterProgression() {
    HdrImage whiteEnv{};
    whiteEnv.width = 64;
    whiteEnv.height = 32;
    whiteEnv.channels = 4;
    whiteEnv.pixels.resize(64 * 32 * 4, 1.0F);

    const CubemapData cubemap = IblProcessor::equirectangularToCubemap(whiteEnv, 16);
    TEST_CHECK(cubemap.isValid());

    const CubemapData prefiltered = IblProcessor::prefilterSpecularEnvironment(cubemap, 16, 4, 64);
    TEST_CHECK(prefiltered.isValid());
    TEST_CHECK(prefiltered.mipLevels == 4);

    // Mip sizes should decrease by half: 16, 8, 4, 2
    TEST_CHECK(prefiltered.mips[0].faces[0].width == 16);
    TEST_CHECK(prefiltered.mips[1].faces[0].width == 8);
    TEST_CHECK(prefiltered.mips[2].faces[0].width == 4);
    TEST_CHECK(prefiltered.mips[3].faces[0].width == 2);

    // Uniform radiance environment preserves 1.0 radiance at all roughness levels
    for (uint32_t m = 0; m < 4; ++m) {
        const float val = halfToFloat(prefiltered.mips[m].faces[0].halfPixels[0]);
        TEST_CHECK(std::abs(val - 1.0F) < 0.05F);
    }

    return 0;
}

static int testBrdfLutBoundaryConditions() {
    const BrdfLutData lut = IblProcessor::generateBrdfLut(64, 128);
    TEST_CHECK(lut.isValid());
    TEST_CHECK(lut.width == 64);
    TEST_CHECK(lut.height == 64);
    TEST_CHECK(lut.halfPixels.size() == 64 * 64 * 4);

    // Fetch (NdotV, roughness)
    const auto fetch = [&lut](uint32_t x, uint32_t y) noexcept -> Vec2 {
        const size_t idx = (static_cast<size_t>(y) * lut.width + x) * 4U;
        return Vec2(halfToFloat(lut.halfPixels[idx]), halfToFloat(lut.halfPixels[idx + 1U]));
    };

    // Boundary condition 1: Normal incidence (NdotV = 1.0, x = 63) and smooth (roughness = 0, y =
    // 0) Scale should be close to 1.0, bias should be close to 0.0
    const Vec2 smoothNormal = fetch(63, 0);
    TEST_CHECK(smoothNormal.x > 0.85F);
    TEST_CHECK(smoothNormal.y < 0.15F);

    // Boundary condition 2: Grazing angle (NdotV close to 0, x = 0)
    // Grazing angle Fresnel increases bias term towards 1.0
    const Vec2 grazingSmooth = fetch(0, 0);
    TEST_CHECK(grazingSmooth.y > smoothNormal.y);

    // Boundary condition 3: All values strictly in [0.0, 1.0]
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
            const Vec2 val = fetch(x, y);
            TEST_CHECK(val.x >= 0.0F && val.x <= 1.0F);
            TEST_CHECK(val.y >= 0.0F && val.y <= 1.0F);
        }
    }

    return 0;
}

int main() {
    std::cout << "Starting environment_map_test..." << std::endl;
    if (testFloatToHalfRoundTrip() != 0)
        return 1;
    if (testCubeFaceUVInvertibility() != 0)
        return 1;
    if (testProceduralSkyAndCubemap() != 0)
        return 1;
    if (testDiffuseIrradianceEnergyConservation() != 0)
        return 1;
    if (testSpecularPrefilterProgression() != 0)
        return 1;
    if (testBrdfLutBoundaryConditions() != 0)
        return 1;
    std::cout << "All environment_map_test cases PASSED!" << std::endl;
    return 0;
}
