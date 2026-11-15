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

static void testMatrixTranslationExtraction() {
    // 1. Identity matrix translation
    const Mat4 identity = Mat4::identity();
    const Vec3 tId = identity.getTranslation();
    TEST_CHECK(std::abs(tId.x) < 1e-5F && std::abs(tId.y) < 1e-5F && std::abs(tId.z) < 1e-5F);

    // 2. Direct translation matrix
    const Vec3 pos{12.5F, -3.2F, 48.0F};
    const Mat4 tMat = Mat4::fromTranslation(pos);
    const Vec3 tExtracted = tMat.getTranslation();
    TEST_CHECK(std::abs(tExtracted.x - pos.x) < 1e-4F);
    TEST_CHECK(std::abs(tExtracted.y - pos.y) < 1e-4F);
    TEST_CHECK(std::abs(tExtracted.z - pos.z) < 1e-4F);

    // Verify that bottom row (m[3], m[7], m[11]) is zero, proving that reading m[3,7,11] would be
    // wrong
    TEST_CHECK(std::abs(tMat.m[3]) < 1e-6F);
    TEST_CHECK(std::abs(tMat.m[7]) < 1e-6F);
    TEST_CHECK(std::abs(tMat.m[11]) < 1e-6F);

    // 3. Composite affine transform (scale + rotation + translation)
    const Quat rot = Quat::fromAxisAngle(Vec3::unitY(), 0.785398F);
    const Vec3 scale{2.0F, 2.0F, 2.0F};
    const Mat4 trs = Mat4::fromTransform(pos, rot, scale);
    const Vec3 trsTranslation = trs.getTranslation();
    TEST_CHECK(std::abs(trsTranslation.x - pos.x) < 1e-4F);
    TEST_CHECK(std::abs(trsTranslation.y - pos.y) < 1e-4F);
    TEST_CHECK(std::abs(trsTranslation.z - pos.z) < 1e-4F);

    // 4. Hierarchical parent-child composition
    const Mat4 parentMat = Mat4::fromTranslation(Vec3(10.0F, 0.0F, 0.0F));
    const Mat4 childMat = Mat4::fromTranslation(Vec3(5.0F, 2.0F, -3.0F));
    const Mat4 worldMat = parentMat * childMat;
    const Vec3 worldTranslation = worldMat.getTranslation();
    TEST_CHECK(std::abs(worldTranslation.x - 15.0F) < 1e-4F);
    TEST_CHECK(std::abs(worldTranslation.y - 2.0F) < 1e-4F);
    TEST_CHECK(std::abs(worldTranslation.z - (-3.0F)) < 1e-4F);

    std::cout << "[PASS] testMatrixTranslationExtraction\n";
}

static void testFrustumNearFarConsistency() {
    CascadeConfig config{
        .cascadeCount = 4,
        .maxDistance = 50.0F,
        .splitLambda = 0.80F,
        .stabilizeCascades = true,
        .atlasResolution = 2048,
    };

    // Camera with 1000m far plane (reproducing the reported Bistro/Editor scenario)
    const Mat4 camView =
        Mat4::lookAt(Vec3(0.0F, 2.0F, 10.0F), Vec3(0.0F, 2.0F, 0.0F), Vec3::unitY());
    const Mat4 camProj = Mat4::perspective(0.90757F, 16.0F / 9.0F, 0.1F, 1000.0F);
    const Vec3 lightDir = Vec3(0.5F, 0.8F, 0.5F).normalized();

    std::vector<ShadowCascade> cascades;
    computeShadowCascades(camView, camProj, 0.1F, 1000.0F, lightDir, config, cascades);
    TEST_CHECK(cascades.size() == 4);

    // Verify Cascade 0 near-field bounds are strictly bounded and not diluted to 56m!
    TEST_CHECK(cascades[0].splitNear >= 0.1F);
    TEST_CHECK(cascades[0].splitFar < 4.0F);
    // Radius must be tightly bounded around ~3.5m, NOT 70m!
    TEST_CHECK(cascades[0].texelSizeWorld < 0.015F); // Sub-centimeter texel resolution (<1.5cm)

    // Verify all 8 slice corners are finite and strictly bounded
    for (size_t c = 0; c < cascades.size(); ++c) {
        TEST_CHECK(std::isfinite(cascades[c].worldBounds.minBound.x));
        TEST_CHECK(std::isfinite(cascades[c].worldBounds.maxBound.x));
        TEST_CHECK(cascades[c].splitNear < cascades[c].splitFar);
        TEST_CHECK(cascades[c].texelSizeWorld > 0.0F);
    }

    std::cout << "[PASS] testFrustumNearFarConsistency\n";
}

static void testOffscreenOccluderRetention() {
    CascadeConfig config{
        .cascadeCount = 4,
        .maxDistance = 50.0F,
        .splitLambda = 0.80F,
        .stabilizeCascades = true,
        .atlasResolution = 2048,
    };

    const Mat4 camView =
        Mat4::lookAt(Vec3(0.0F, 2.0F, 10.0F), Vec3(0.0F, 2.0F, 0.0F), Vec3::unitY());
    const Mat4 camProj = Mat4::perspective(0.90757F, 16.0F / 9.0F, 0.1F, 100.0F);
    const Vec3 lightDir = Vec3(0.5F, 0.8F, 0.5F).normalized();

    std::vector<ShadowCascade> cascades;
    computeShadowCascades(camView, camProj, 0.1F, 100.0F, lightDir, config, cascades);
    TEST_CHECK(cascades.size() == 4);

    // An occluder positioned 15m in front of Cascade 0 center along the sunlight direction
    // (This occluder is outside the camera view frustum, but casts shadows into Cascade 0)
    std::array<Vec3, 8> c0Corners;
    const Mat4 viewProj = camProj * camView;
    getFrustumSliceCorners(viewProj.inverse().value(), cascades[0].splitNear, cascades[0].splitFar,
                           0.1F, 1000.0F, c0Corners);
    Vec3 c0Center = Vec3::zero();
    for (const auto& p : c0Corners) {
        c0Center = c0Center + p;
    }
    c0Center = c0Center * (1.0F / 8.0F);

    const Vec3 occluderPos = c0Center + (lightDir * 15.0F);
    AABB occluderBounds{occluderPos - Vec3(1.0F, 1.0F, 1.0F), occluderPos + Vec3(1.0F, 1.0F, 1.0F)};

    // In 11E-R1, light frustum must retain this offscreen caster without clipping it
    TEST_CHECK(cascades[0].lightFrustum.intersectsAABB(occluderBounds));

    std::cout << "[PASS] testOffscreenOccluderRetention\n";
}

int main() {
    std::cout << "Running Shadow Cascade Unit Tests...\n";
    testLogarithmicUniformSplits();
    testAtlasTransformMatrix();
    testTexelStabilization();
    testMatrixTranslationExtraction();
    testFrustumNearFarConsistency();
    testOffscreenOccluderRetention();
    std::cout << "All Shadow Cascade Unit Tests PASSED!\n";
    return 0;
}
