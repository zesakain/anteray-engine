/// @file irradiance_probe.h
/// @brief Spherical Harmonics L2 representation and Irradiance Probe ECS components.
///
/// Purpose:
/// Implements mathematically rigorous L2 Spherical Harmonics (9 RGB coefficients)
/// for diffuse irradiance representation conforming to Ramamoorthi & Hanrahan (2001).
/// Provides ECS components for localized scene-aware irradiance probes and probe volumes.
///
/// Ownership and Lifetime:
/// Value types with static/stack storage. Component data owned by flecs World.
///
/// Thread-safety:
/// Pure mathematical functions and const value access are thread-safe and re-entrant.
///
/// Performance:
/// Fast analytical evaluation using only additions and multiplications (no trigonometric
/// calls). Zero heap allocation in hot path.

#pragma once

#include "core/math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace eng::render {

/// @struct SphericalHarmonicsL2
/// @brief 9 RGB coefficients for order-2 Spherical Harmonics (27 floats total).
///
/// Represents an incoming radiance or diffuse irradiance distribution over the sphere S2.
/// Evaluates diffuse irradiance analytically in O(1) time without numerical integration.
struct SphericalHarmonicsL2 {
    /// Coefficients indexed as:
    /// l=0, m=0:   index 0 (ambient mean)
    /// l=1, m=-1:  index 1 (y direction)
    /// l=1, m=0:   index 2 (z direction)
    /// l=1, m=1:   index 3 (x direction)
    /// l=2, m=-2:  index 4 (xy direction)
    /// l=2, m=-1:  index 5 (yz direction)
    /// l=2, m=0:   index 6 (3z^2 - 1)
    /// l=2, m=1:   index 7 (xz direction)
    /// l=2, m=2:   index 8 (x^2 - y^2)
    std::array<Vec3, 9> coefficients{};

    /// @brief Creates zero-initialized Spherical Harmonics.
    constexpr SphericalHarmonicsL2() noexcept = default;

    /// @brief Creates uniform ambient Spherical Harmonics with constant radiance in all directions.
    [[nodiscard]] static constexpr SphericalHarmonicsL2 fromUniform(const Vec3& color) noexcept {
        SphericalHarmonicsL2 sh{};
        // Y_00 = 0.5 * sqrt(1 / pi) => ~0.282095
        // Radiance integral over sphere gives L_00 = color / (2 * sqrt(pi))
        sh.coefficients[0] = color * 3.5449077F; // Integral multiplier for diffuse convolution
        return sh;
    }

    /// @brief Creates Spherical Harmonics from a directional light source.
    /// @param dir Normalized direction pointing towards the light source.
    /// @param color Emitted radiance or illuminance.
    [[nodiscard]] static SphericalHarmonicsL2 fromDirectional(const Vec3& dir,
                                                              const Vec3& color) noexcept {
        SphericalHarmonicsL2 sh{};
        Vec3 d = (dir.lengthSq() > 1e-6F) ? dir.normalized() : Vec3(0.0F, 1.0F, 0.0F);

        // Basis projection constants for delta light
        constexpr float c0 = 0.282095F;
        constexpr float c1 = 0.488603F;
        constexpr float c2 = 1.092548F;
        constexpr float c3 = 0.315392F;
        constexpr float c4 = 0.546274F;

        sh.coefficients[0] = color * c0;
        sh.coefficients[1] = color * (c1 * d.y);
        sh.coefficients[2] = color * (c1 * d.z);
        sh.coefficients[3] = color * (c1 * d.x);
        sh.coefficients[4] = color * (c2 * d.x * d.y);
        sh.coefficients[5] = color * (c2 * d.y * d.z);
        sh.coefficients[6] = color * (c3 * (3.0F * d.z * d.z - 1.0F));
        sh.coefficients[7] = color * (c2 * d.x * d.z);
        sh.coefficients[8] = color * (c4 * (d.x * d.x - d.y * d.y));

        return sh;
    }

    /// @brief Accumulates directional sample into the SH distribution with solid angle weighting.
    void addSample(const Vec3& dir, const Vec3& radiance, float solidAngle) noexcept {
        Vec3 d = (dir.lengthSq() > 1e-6F) ? dir.normalized() : Vec3(0.0F, 1.0F, 0.0F);

        constexpr float c0 = 0.282095F;
        constexpr float c1 = 0.488603F;
        constexpr float c2 = 1.092548F;
        constexpr float c3 = 0.315392F;
        constexpr float c4 = 0.546274F;

        const float weight = solidAngle;
        coefficients[0] += radiance * (c0 * weight);
        coefficients[1] += radiance * (c1 * d.y * weight);
        coefficients[2] += radiance * (c1 * d.z * weight);
        coefficients[3] += radiance * (c1 * d.x * weight);
        coefficients[4] += radiance * (c2 * d.x * d.y * weight);
        coefficients[5] += radiance * (c2 * d.y * d.z * weight);
        coefficients[6] += radiance * (c3 * (3.0F * d.z * d.z - 1.0F) * weight);
        coefficients[7] += radiance * (c2 * d.x * d.z * weight);
        coefficients[8] += radiance * (c4 * (d.x * d.x - d.y * d.y) * weight);
    }

    /// @brief Evaluates diffuse irradiance E(N) for a surface with normal N.
    ///
    /// Implements Ramamoorthi & Hanrahan (2001) analytical convolution with the Lambertian
    /// cosine lobe. Accuracy is within 1-2% of ground truth Monte Carlo hemispherical integral.
    [[nodiscard]] Vec3 evaluateIrradiance(const Vec3& normal) const noexcept {
        Vec3 n = (normal.lengthSq() > 1e-6F) ? normal.normalized() : Vec3(0.0F, 1.0F, 0.0F);

        // Ramamoorthi & Hanrahan (2001) pre-multiplied convolution constants
        constexpr float c1 = 0.429043F; // A_2 * c_2
        constexpr float c2 = 0.511664F; // A_1 * c_1
        constexpr float c3 = 0.743125F; // A_2 * c_3
        constexpr float c4 = 0.886227F; // A_0 * c_0
        constexpr float c5 = 0.247708F; // A_2 * c_4 (note: c5 = c3 / 3)

        Vec3 irradiance =
            c4 * coefficients[0] +
            (2.0F * c2) * (coefficients[3] * n.x + coefficients[1] * n.y + coefficients[2] * n.z) +
            c1 * coefficients[8] * (n.x * n.x - n.y * n.y) + c3 * coefficients[6] * (n.z * n.z) -
            c5 * coefficients[6] +
            (2.0F * c1) * (coefficients[4] * (n.x * n.y) + coefficients[7] * (n.x * n.z) +
                           coefficients[5] * (n.y * n.z));

        // Clamp negative results caused by SH ringing/truncation artifacts
        return Vec3(std::max(0.0F, irradiance.x), std::max(0.0F, irradiance.y),
                    std::max(0.0F, irradiance.z));
    }

    /// @brief Linearly interpolates between two SH distributions.
    [[nodiscard]] static SphericalHarmonicsL2
    lerp(const SphericalHarmonicsL2& a, const SphericalHarmonicsL2& b, float t) noexcept {
        SphericalHarmonicsL2 res{};
        float tClamped = std::clamp(t, 0.0F, 1.0F);
        for (size_t i = 0; i < 9; ++i) {
            res.coefficients[i] =
                a.coefficients[i] * (1.0F - tClamped) + b.coefficients[i] * tClamped;
        }
        return res;
    }

    SphericalHarmonicsL2& operator+=(const SphericalHarmonicsL2& other) noexcept {
        for (size_t i = 0; i < 9; ++i) {
            coefficients[i] += other.coefficients[i];
        }
        return *this;
    }

    SphericalHarmonicsL2& operator*=(float scalar) noexcept {
        for (size_t i = 0; i < 9; ++i) {
            coefficients[i] *= scalar;
        }
        return *this;
    }
};

/// @struct IrradianceProbeComponent
/// @brief ECS component representing a localized scene irradiance probe.
struct IrradianceProbeComponent {
    /// World-space position of probe center.
    Vec3 position{0.0F, 0.0F, 0.0F};

    /// Influence radius in meters for radial distance falloff.
    float radius{10.0F};

    /// Bounding box minimum for box-constrained probe influence.
    Vec3 boxMin{-5.0F, -5.0F, -5.0F};

    /// Bounding box maximum for box-constrained probe influence.
    Vec3 boxMax{5.0F, 5.0F, 5.0F};

    /// Multiplier on probe irradiance contribution.
    float intensity{1.0F};

    /// Stored order-2 Spherical Harmonics coefficients.
    SphericalHarmonicsL2 sh{};

    /// Whether probe is actively evaluated in scene GI.
    bool enabled{true};

    /// Whether probe uses box bounds (true) or radial sphere bounds (false).
    bool useBoxBounds{false};

    /// @brief Computes blending weight at a world-space point P.
    [[nodiscard]] float computeWeight(const Vec3& point) const noexcept {
        if (!enabled || intensity <= 0.0F) {
            return 0.0F;
        }

        if (useBoxBounds) {
            if (point.x < boxMin.x || point.x > boxMax.x || point.y < boxMin.y ||
                point.y > boxMax.y || point.z < boxMin.z || point.z > boxMax.z) {
                return 0.0F;
            }
            // Linear distance to box edge for smooth perimeter falloff
            float dx = std::min(point.x - boxMin.x, boxMax.x - point.x);
            float dy = std::min(point.y - boxMin.y, boxMax.y - point.y);
            float dz = std::min(point.z - boxMin.z, boxMax.z - point.z);
            float dMin = std::min({dx, dy, dz});
            float margin = 0.5F;
            return std::clamp(dMin / margin, 0.0F, 1.0F) * intensity;
        }

        float dist = (point - position).length();
        if (dist >= radius) {
            return 0.0F;
        }
        float normDist = dist / radius;
        // Smooth cubic Hermite falloff: (1 - x^2)^2
        float f = 1.0F - normDist * normDist;
        return (f * f) * intensity;
    }
};

/// @struct ReflectionProbeComponent
/// @brief ECS component representing localized specular reflection capture.
struct ReflectionProbeComponent {
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 boxMin{-5.0F, -5.0F, -5.0F};
    Vec3 boxMax{5.0F, 5.0F, 5.0F};
    float radius{10.0F};
    float intensity{1.0F};
    bool boxProjection{true};
    bool enabled{true};

    /// @brief Computes spatial blending weight at world position P with smooth margin falloff.
    [[nodiscard]] float computeWeight(const Vec3& point) const noexcept {
        if (!enabled || intensity <= 0.0F) {
            return 0.0F;
        }
        if (boxProjection) {
            if (point.x < boxMin.x || point.x > boxMax.x || point.y < boxMin.y ||
                point.y > boxMax.y || point.z < boxMin.z || point.z > boxMax.z) {
                return 0.0F;
            }
            float dx = std::min(point.x - boxMin.x, boxMax.x - point.x);
            float dy = std::min(point.y - boxMin.y, boxMax.y - point.y);
            float dz = std::min(point.z - boxMin.z, boxMax.z - point.z);
            float dMin = std::min({dx, dy, dz});
            float margin = 0.5F;
            return std::clamp(dMin / margin, 0.0F, 1.0F) * intensity;
        }

        float dist = (point - position).length();
        if (dist >= radius) {
            return 0.0F;
        }
        float normDist = dist / radius;
        float f = 1.0F - normDist * normDist;
        return (f * f) * intensity;
    }

    /// @brief Applies local box-projection parallax correction to a reflection direction R.
    [[nodiscard]] Vec3 boxProject(const Vec3& R, const Vec3& worldPos) const noexcept {
        if (!boxProjection) {
            return R;
        }
        Vec3 r = (R.lengthSq() > 1e-6F) ? R.normalized() : Vec3(0.0F, 1.0F, 0.0F);

        float invRx = (std::abs(r.x) > 1e-5F) ? (1.0F / r.x) : (r.x >= 0.0F ? 1e5F : -1e5F);
        float invRy = (std::abs(r.y) > 1e-5F) ? (1.0F / r.y) : (r.y >= 0.0F ? 1e5F : -1e5F);
        float invRz = (std::abs(r.z) > 1e-5F) ? (1.0F / r.z) : (r.z >= 0.0F ? 1e5F : -1e5F);

        float t1x = (boxMax.x - worldPos.x) * invRx;
        float t2x = (boxMin.x - worldPos.x) * invRx;
        float t1y = (boxMax.y - worldPos.y) * invRy;
        float t2y = (boxMin.y - worldPos.y) * invRy;
        float t1z = (boxMax.z - worldPos.z) * invRz;
        float t2z = (boxMin.z - worldPos.z) * invRz;

        float tMaxX = std::max(t1x, t2x);
        float tMaxY = std::max(t1y, t2y);
        float tMaxZ = std::max(t1z, t2z);

        float dist = std::min({tMaxX, tMaxY, tMaxZ});
        if (dist <= 0.0F) {
            return r;
        }
        Vec3 intersectPos = worldPos + r * dist;
        Vec3 corrected = intersectPos - position;
        return (corrected.lengthSq() > 1e-6F) ? corrected.normalized() : r;
    }
};

} // namespace eng::render
