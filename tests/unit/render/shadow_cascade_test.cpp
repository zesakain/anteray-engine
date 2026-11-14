/// @file shadow_cascade_test.cpp
/// @brief Automated test suite verifying Cascaded Shadow Map (CSM) mathematics and stability.

#include "render/shadow_cascade.h"

#include <cmath>
#include <iostream>
#include <vector>

using namespace eng;
using namespace eng::render;

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << '\n'; \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

static void testLogarithmicUniformSplits() {
    CascadeConfig config{
        .cascadeCount = 4,
        .maxDistance = 100.0F,
        .splitLambda = 0.80F,
        .stabilizeCascades = true,
        .atlasResolution = 2048,
    };

    const float camNear = 0.1F;
    const float camFar = 200.0F;

    CascadeSplits splits = calculateCascadeSplits(camNear, camFar, config);
    TEST_CHECK(splits.count == 4);
    TEST_CHECK(std::abs(splits.distances[0] - 0.1F) < 1e-4F);

    // Monotonically increasing bounds
    for (size_t i = 0; i < 4; ++i) {
        TEST_CHECK(splits.distances[i] < splits.distances[i + 1]);
    }

    // Last cascade far boundary matches maxDistance
    TEST_CHECK(std::abs(splits.distances[4] - 100.0F) < 1e-3F);

    // Pure uniform distribution (lambda = 0.0)
    config.splitLambda = 0.0F;
    CascadeSplits uniformSplits = calculateCascadeSplits(camNear, camFar, config);
    const float uniformStep = (100.0F - 0.1F) / 4.0F;
    TEST_CHECK(std::abs(uniformSplits.distances[1] - (0.1F + uniformStep)) < 1e-3F);
    TEST_CHECK(std::abs(uniformSplits.distances[2] - (0.1F + (uniformStep * 2.0F))) < 1e-3F);

    // Pure logarithmic distribution (lambda = 1.0)
    config.splitLambda = 1.0F;
    CascadeSplits logSplits = calculateCascadeSplits(camNear, camFar, config);
    const float ratio = 100.0F / 0.1F;                        // 1000
    const float expectedLog1 = 0.1F * std::pow(ratio, 0.25F); // ~0.56m
    TEST_CHECK(std::abs(logSplits.distances[1] - expectedLog1) < 1e-2F);

    std::cout << "[PASS] testLogarithmicUniformSplits\n";
}

static void testAtlasTransformMatrix() {
    // 2x2 grid, tile (0, 0) covering UV [0.0, 0.5] x [0.0, 0.5]
    const Vec4 tile00{0.0F, 0.0F, 0.5F, 0.5F};
    const Mat4 m00 = buildAtlasTransformMatrix(tile00);

    // NDC Top-Left (-1, -1, 0.5) -> In Vulkan, NDC Y is down: (-1, -1) is top-left in standard
    // coords u = (-1 * 0.5 + 0.5) * 0.5 + 0 = 0.0 v = (-1 * -0.5 + 0.5) * 0.5 + 0 = 0.5 (or
    // top-left depending on Y convention)
    const Vec3 ndcTopLeft{-1.0F, 1.0F, 0.5F};
    const Vec3 uvTopLeft = m00.transformPoint(ndcTopLeft);
    TEST_CHECK(std::abs(uvTopLeft.x - 0.0F) < 1e-4F);
    TEST_CHECK(std::abs(uvTopLeft.y - 0.0F) < 1e-4F);
    TEST_CHECK(std::abs(uvTopLeft.z - 0.5F) < 1e-4F);

    const Vec3 ndcBottomRight{1.0F, -1.0F, 0.5F};
    const Vec3 uvBottomRight = m00.transformPoint(ndcBottomRight);
    TEST_CHECK(std::abs(uvBottomRight.x - 0.5F) < 1e-4F);
    TEST_CHECK(std::abs(uvBottomRight.y - 0.5F) < 1e-4F);

    // Tile (1, 1) covering UV [0.5, 1.0] x [0.5, 1.0]
    const Vec4 tile11{0.5F, 0.5F, 0.5F, 0.5F};
    const Mat4 m11 = buildAtlasTransformMatrix(tile11);
    const Vec3 uv11TopLeft = m11.transformPoint(ndcTopLeft);
    TEST_CHECK(std::abs(uv11TopLeft.x - 0.5F) < 1e-4F);
    TEST_CHECK(std::abs(uv11TopLeft.y - 0.5F) < 1e-4F);

    const Vec3 uv11BottomRight = m11.transformPoint(ndcBottomRight);
    TEST_CHECK(std::abs(uv11BottomRight.x - 1.0F) < 1e-4F);
    TEST_CHECK(std::abs(uv11BottomRight.y - 1.0F) < 1e-4F);

    std::cout << "[PASS] testAtlasTransformMatrix\n";
}

static void testTexelStabilization() {
    CascadeConfig config{
        .cascadeCount = 4,
        .maxDistance = 50.0F,
        .splitLambda = 0.80F,
        .stabilizeCascades = true,
        .atlasResolution = 2048,
    };

    const Mat4 camView1 =
        Mat4::lookAt(Vec3(0.0F, 2.0F, 10.0F), Vec3(0.0F, 2.0F, 0.0F), Vec3::unitY());
    const Mat4 camProj = Mat4::perspective(1.04719755F, 16.0F / 9.0F, 0.1F, 100.0F);
    const Vec3 lightDir = Vec3(0.5F, 0.8F, 0.5F).normalized();

    std::vector<ShadowCascade> cascades1;
    computeShadowCascades(camView1, camProj, 0.1F, 100.0F, lightDir, config, cascades1);
    TEST_CHECK(cascades1.size() == 4);

    // Minor sub-texel camera translation (e.g. 0.002 meters)
    const Mat4 camView2 =
        Mat4::lookAt(Vec3(0.002F, 2.0F, 10.0F), Vec3(0.002F, 2.0F, 0.0F), Vec3::unitY());
    std::vector<ShadowCascade> cascades2;
    computeShadowCascades(camView2, camProj, 0.1F, 100.0F, lightDir, config, cascades2);
    TEST_CHECK(cascades2.size() == 4);

    // In stabilized cascades, the texel world size and projection dimensions are identical
    for (size_t i = 0; i < 4; ++i) {
        TEST_CHECK(std::abs(cascades1[i].texelSizeWorld - cascades2[i].texelSizeWorld) < 1e-5F);
        // Orthographic projection bounds must be identical
        TEST_CHECK(std::abs(cascades1[i].projMatrix.m[0] - cascades2[i].projMatrix.m[0]) < 1e-5F);
        TEST_CHECK(std::abs(cascades1[i].projMatrix.m[5] - cascades2[i].projMatrix.m[5]) < 1e-5F);
    }

    std::cout << "[PASS] testTexelStabilization\n";
}

int main() {
    std::cout << "Running Shadow Cascade Unit Tests...\n";
    testLogarithmicUniformSplits();
    testAtlasTransformMatrix();
    testTexelStabilization();
    std::cout << "All Shadow Cascade Unit Tests PASSED!\n";
    return 0;
}
