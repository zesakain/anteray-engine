/// @file shadow_cascade.h
/// @brief Cascaded Shadow Map (CSM) split mathematics, frustum fitting, and atlas transformations.
///
/// Purpose:
/// Provides mathematically rigorous, stabilized cascade computation for directional sunlight.
/// Computes logarithmic-uniform split distributions, camera frustum slice fitting via bounding
/// spheres, sub-texel light-space snapping to eliminate shadow shimmering during camera motion,
/// and pre-calculated atlas transformations.
///
/// Contract:
/// - Fully pure mathematical functions, zero heap allocations, no locks.
/// - Suitable for execution in hot-path simulation/rendering jobs.
/// - Generates std140-compatible matrices and atlas UV coordinates.

#pragma once

#include "core/math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace eng::render {

inline constexpr uint32_t kMaxShadowCascades = 4;

/// @struct CascadeConfig
/// @brief Configuration governing cascaded shadow map generation.
struct CascadeConfig {
    uint32_t cascadeCount{4};       ///< Number of cascades: 1, 2, 3, or 4
    float maxDistance{50.0F};       ///< Maximum shadow distance in meters
    float splitLambda{0.80F};       ///< Logarithmic (1.0) vs Uniform (0.0) split blending
    bool stabilizeCascades{true};   ///< Quantize light-space bounds to texel boundaries
    uint32_t atlasResolution{2048}; ///< Total dimension of the shadow map atlas
};

/// @struct ShadowCascade
/// @brief Calculated transform matrices, bounding volumes, and atlas regions for one cascade.
struct ShadowCascade {
    Mat4 viewMatrix{Mat4::identity()};
    Mat4 projMatrix{Mat4::identity()};
    Mat4 viewProjMatrix{Mat4::identity()};
    Mat4 atlasMatrix{Mat4::identity()};

    float splitNear{0.1F};
    float splitFar{10.0F};
    float texelSizeWorld{0.01F};

    Vec4 atlasTileUV{0.0F, 0.0F, 1.0F, 1.0F}; // x: uMin, y: vMin, z: width, w: height
    uint32_t viewportX{0};
    uint32_t viewportY{0};
    uint32_t viewportWidth{2048};
    uint32_t viewportHeight{2048};

    AABB worldBounds{};
    Frustum lightFrustum{};
};

/// @struct CascadeSplits
/// @brief Evaluated view-space split boundary distances for up to 4 cascades.
struct CascadeSplits {
    uint32_t count{4};
    std::array<float, kMaxShadowCascades + 1>
        distances{}; // distances[0] = near, distances[i] = far of cascade i-1
};

/// @brief Calculates logarithmic-uniform split distances for shadow cascades.
[[nodiscard]] inline CascadeSplits calculateCascadeSplits(float cameraNear, float cameraFar,
                                                          const CascadeConfig& config) noexcept {
    CascadeSplits result{};
    result.count = std::clamp(config.cascadeCount, 1U, kMaxShadowCascades);

    const float effectiveFar = std::min(cameraFar, std::max(cameraNear + 1.0F, config.maxDistance));
    const float lambda = std::clamp(config.splitLambda, 0.0F, 1.0F);

    result.distances[0] = cameraNear;

    const float clipRange = effectiveFar - cameraNear;
    const float minZ = std::max(0.01F, cameraNear);
    const float ratio = effectiveFar / minZ;

    for (uint32_t i = 1; i <= result.count; ++i) {
        const float p = static_cast<float>(i) / static_cast<float>(result.count);
        const float logDist = minZ * std::pow(ratio, p);
        const float uniformDist = cameraNear + (clipRange * p);
        result.distances[i] = (lambda * logDist) + ((1.0F - lambda) * uniformDist);
    }

    return result;
}

/// @brief Computes 8 corner vertices of a camera view frustum slice between two view depths.
inline void getFrustumSliceCorners(const Mat4& invViewProj, float sliceNearZ, float sliceFarZ,
                                   float camNearZ, float camFarZ,
                                   std::array<Vec3, 8>& outCorners) noexcept {
    (void)camNearZ;
    (void)camFarZ;

    // Extract camera center of projection (eye) in world space
    Vec3 eye = Vec3::zero();
    if (std::abs(invViewProj.m[11]) > 1e-6F) {
        eye = Vec3(-invViewProj.m[8] / invViewProj.m[11], -invViewProj.m[9] / invViewProj.m[11],
                   -invViewProj.m[10] / invViewProj.m[11]);
    } else {
        float w0 = 0.0F;
        eye = invViewProj.transformPointWithW(Vec3::zero(), w0);
    }

    // Extract camera forward direction in world space
    float wn = 0.0F;
    float wf = 0.0F;
    Vec3 nearCenter = invViewProj.transformPointWithW(Vec3(0.0F, 0.0F, 0.0F), wn);
    if (std::abs(wn) > 1e-6F) {
        nearCenter = nearCenter * (1.0F / wn);
    }
    Vec3 farCenter = invViewProj.transformPointWithW(Vec3(0.0F, 0.0F, 1.0F), wf);
    if (std::abs(wf) > 1e-6F) {
        farCenter = farCenter * (1.0F / wf);
    }
    Vec3 forward = farCenter - nearCenter;
    if (forward.lengthSq() > 1e-6F) {
        forward = forward.normalized();
    } else {
        forward = Vec3(0.0F, 0.0F, -1.0F);
    }

    // 4 NDC corners on the far plane (Vulkan depth Z = 1.0)
    static constexpr std::array<Vec3, 4> kNdcFarCorners = {{
        {-1.0F, -1.0F, 1.0F},
        {1.0F, -1.0F, 1.0F},
        {-1.0F, 1.0F, 1.0F},
        {1.0F, 1.0F, 1.0F},
    }};

    std::array<Vec3, 4> rayDirs{};
    for (size_t i = 0; i < 4; ++i) {
        float wc = 0.0F;
        Vec3 pt = invViewProj.transformPointWithW(kNdcFarCorners[i], wc);
        if (std::abs(wc) > 1e-6F) {
            pt = pt * (1.0F / wc);
        }
        const Vec3 dir = pt - eye;
        const float depth = Vec3::dot(dir, forward);
        if (std::abs(depth) > 1e-4F) {
            rayDirs[i] = dir * (1.0F / depth);
        } else {
            rayDirs[i] = dir;
        }
    }

    const float nearZ = std::max(0.01F, sliceNearZ);
    const float farZ = std::max(nearZ + 0.01F, sliceFarZ);

    for (size_t i = 0; i < 4; ++i) {
        outCorners[i] = eye + (rayDirs[i] * nearZ);
        outCorners[i + 4] = eye + (rayDirs[i] * farZ);
    }
}

/// @brief Builds a 4x4 matrix that converts light NDC [-1, 1]^2 to normalized atlas UV coordinates.
[[nodiscard]] inline Mat4 buildAtlasTransformMatrix(const Vec4& atlasTileUV) noexcept {
    // Vulkan NDC: X in [-1, 1] -> u in [0, 1]; Y in [-1, 1] -> v in [0, 1] (Y-down)
    // u = (X * 0.5 + 0.5) * scaleX + offsetX
    // v = (Y * -0.5 + 0.5) * scaleY + offsetY
    // z = Z
    const float sx = 0.5F * atlasTileUV.z;
    const float sy = -0.5F * atlasTileUV.w;
    const float ox = (0.5F * atlasTileUV.z) + atlasTileUV.x;
    const float oy = (0.5F * atlasTileUV.w) + atlasTileUV.y;

    Mat4 m = Mat4::identity();
    m.m[0] = sx;
    m.m[5] = sy;
    m.m[10] = 1.0F;
    m.m[12] = ox;
    m.m[13] = oy;
    return m;
}

/// @brief Computes stabilized, frustum-fitted shadow cascades for directional illumination.
inline void computeShadowCascades(const Mat4& cameraView, const Mat4& cameraProj, float cameraNear,
                                  float cameraFar, const Vec3& lightDir,
                                  const CascadeConfig& config,
                                  std::vector<ShadowCascade>& outCascades) noexcept {
    outCascades.clear();
    const CascadeSplits splits = calculateCascadeSplits(cameraNear, cameraFar, config);
    outCascades.resize(splits.count);

    const Mat4 viewProj = cameraProj * cameraView;
    auto invVpOpt = viewProj.inverse();
    const Mat4 invViewProj = invVpOpt.value_or(Mat4::identity());

    const Vec3 normLightDir =
        (lightDir.lengthSq() > 1e-6F) ? lightDir.normalized() : Vec3(0.5F, 0.8F, 0.5F).normalized();
    const Vec3 up = (std::abs(normLightDir.y) > 0.99F) ? Vec3::unitZ() : Vec3::unitY();

    for (uint32_t i = 0; i < splits.count; ++i) {
        ShadowCascade& cascade = outCascades[i];
        cascade.splitNear = splits.distances[i];
        cascade.splitFar = splits.distances[i + 1];

        // 1. Calculate atlas viewport & UV allocation (2x2 grid for 4 cascades, 2x1 for 2, 1x1 for
        // 1)
        if (splits.count == 4) {
            const uint32_t halfRes = config.atlasResolution / 2;
            const uint32_t col = i % 2;
            const uint32_t row = i / 2;
            cascade.viewportX = col * halfRes;
            cascade.viewportY = row * halfRes;
            cascade.viewportWidth = halfRes;
            cascade.viewportHeight = halfRes;
            cascade.atlasTileUV =
                Vec4(static_cast<float>(col) * 0.5F, static_cast<float>(row) * 0.5F, 0.5F, 0.5F);
        } else if (splits.count == 2) {
            const uint32_t halfRes = config.atlasResolution / 2;
            cascade.viewportX = i * halfRes;
            cascade.viewportY = 0;
            cascade.viewportWidth = halfRes;
            cascade.viewportHeight = config.atlasResolution;
            cascade.atlasTileUV = Vec4(static_cast<float>(i) * 0.5F, 0.0F, 0.5F, 1.0F);
        } else {
            cascade.viewportX = 0;
            cascade.viewportY = 0;
            cascade.viewportWidth = config.atlasResolution;
            cascade.viewportHeight = config.atlasResolution;
            cascade.atlasTileUV = Vec4(0.0F, 0.0F, 1.0F, 1.0F);
        }

        // 2. Extract frustum slice corners in world space
        std::array<Vec3, 8> sliceCorners{};
        getFrustumSliceCorners(invViewProj, cascade.splitNear, cascade.splitFar, cameraNear,
                               cameraFar, sliceCorners);

        // 3. Compute bounding sphere center and radius for rotation stability
        Vec3 center = Vec3::zero();
        for (const auto& pt : sliceCorners) {
            center = center + pt;
        }
        center = center * (1.0F / 8.0F);

        float sphereRadius = 0.0F;
        for (const auto& pt : sliceCorners) {
            sphereRadius = std::max(sphereRadius, (pt - center).length());
        }
        // Round sphere radius up to 0.1m to eliminate floating point oscillation
        sphereRadius = std::ceil(sphereRadius * 10.0F) * 0.1F;

        // 4. Stabilize cascade center by snapping to whole shadow texel increments in light space
        const float cascadeResolution = static_cast<float>(cascade.viewportWidth);
        const float worldTexelSize = (2.0F * sphereRadius) / cascadeResolution;
        cascade.texelSizeWorld = worldTexelSize;

        // Pull light eye backward along light direction to encompass all offscreen occluders
        const float maxOccluderDistance = std::max(sphereRadius * 4.0F, config.maxDistance * 2.0F);
        Mat4 lightView = Mat4::lookAt(center + (normLightDir * maxOccluderDistance), center, up);

        if (config.stabilizeCascades && worldTexelSize > 1e-6F) {
            const Vec3 centerLight = lightView.transformPoint(center);
            const float snappedX = std::floor(centerLight.x / worldTexelSize) * worldTexelSize;
            const float snappedY = std::floor(centerLight.y / worldTexelSize) * worldTexelSize;
            const Vec3 snappedLightCenter{snappedX, snappedY, centerLight.z};

            auto invLvOpt = lightView.inverse();
            if (invLvOpt.has_value()) {
                center = invLvOpt->transformPoint(snappedLightCenter);
                lightView = Mat4::lookAt(center + (normLightDir * maxOccluderDistance), center, up);
            }
        }

        // 5. Build orthographic projection matrix covering the sphere bounds
        // Extend z-range backwards to encompass potential shadow casters behind camera
        const float zNear = 0.0F;
        const float zFar = maxOccluderDistance + (sphereRadius * 2.0F);
        const Mat4 lightProj = Mat4::orthographic(-sphereRadius, sphereRadius, -sphereRadius,
                                                  sphereRadius, zNear, zFar);

        cascade.viewMatrix = lightView;
        cascade.projMatrix = lightProj;
        cascade.viewProjMatrix = lightProj * lightView;

        // 6. Precompute atlas-space transformation matrix for direct shader evaluation
        const Mat4 atlasTransform = buildAtlasTransformMatrix(cascade.atlasTileUV);
        cascade.atlasMatrix = atlasTransform * cascade.viewProjMatrix;

        // 7. Compute world-space AABB and light frustum for conservative culling
        cascade.lightFrustum = Frustum::fromViewProjection(cascade.viewProjMatrix);

        AABB bounds{};
        bounds.expand(center - Vec3(sphereRadius, sphereRadius, sphereRadius));
        bounds.expand(center + Vec3(sphereRadius, sphereRadius, sphereRadius));
        // Expand bounds along light direction to include offscreen casters
        bounds.expand(bounds.minBound + (normLightDir * maxOccluderDistance));
        bounds.expand(bounds.maxBound + (normLightDir * maxOccluderDistance));
        cascade.worldBounds = bounds;
    }
}

} // namespace eng::render
