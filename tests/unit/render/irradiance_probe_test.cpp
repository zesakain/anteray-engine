/// @file irradiance_probe_test.cpp
/// @brief Unit tests for order-2 Spherical Harmonics and Irradiance Probe evaluations.

#include "core/assert.h"
#include "render/irradiance_probe.h"

#include <cmath>
#include <iostream>

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

int main() {
    std::cout << "[irradiance_probe_test] Starting Spherical Harmonics & Probe tests...\n";

    // 1. Uniform ambient irradiance
    {
        std::cout << "  - Test 1: Uniform ambient irradiance...\n";
        Vec3 ambientColor(0.8F, 0.5F, 0.2F);
        auto sh = SphericalHarmonicsL2::fromUniform(ambientColor);

        // Evaluation in any direction should be identical and proportional to ambientColor
        Vec3 eUp = sh.evaluateIrradiance(Vec3(0.0F, 1.0F, 0.0F));
        Vec3 eRight = sh.evaluateIrradiance(Vec3(1.0F, 0.0F, 0.0F));
        Vec3 eFwd = sh.evaluateIrradiance(Vec3(0.0F, 0.0F, 1.0F));

        TEST_CHECK(std::abs(eUp.x - eRight.x) < 1e-4F);
        TEST_CHECK(std::abs(eUp.y - eRight.y) < 1e-4F);
        TEST_CHECK(std::abs(eUp.z - eRight.z) < 1e-4F);

        TEST_CHECK(std::abs(eUp.x - eFwd.x) < 1e-4F);
        TEST_CHECK(std::abs(eUp.y - eFwd.y) < 1e-4F);
        TEST_CHECK(std::abs(eUp.z - eFwd.z) < 1e-4F);

        // Irradiance should be positive
        TEST_CHECK(eUp.x > 0.0F && eUp.y > 0.0F && eUp.z > 0.0F);
    }

    // 2. Directional light response
    {
        std::cout << "  - Test 2: Directional light cosine response...\n";
        Vec3 sunDir = Vec3(0.0F, 1.0F, 0.0F); // Sun directly overhead
        Vec3 sunColor(2.0F, 1.5F, 1.0F);
        auto sh = SphericalHarmonicsL2::fromDirectional(sunDir, sunColor);

        // Surface facing up towards sun receives maximum irradiance
        Vec3 eFacing = sh.evaluateIrradiance(Vec3(0.0F, 1.0F, 0.0F));
        // Surface facing perpendicular receives lower irradiance
        Vec3 ePerp = sh.evaluateIrradiance(Vec3(1.0F, 0.0F, 0.0F));
        // Surface facing down (away from sun) receives near zero irradiance
        Vec3 eOpposite = sh.evaluateIrradiance(Vec3(0.0F, -1.0F, 0.0F));

        TEST_CHECK(eFacing.x > ePerp.x);
        TEST_CHECK(eFacing.y > ePerp.y);
        TEST_CHECK(eFacing.z > ePerp.z);

        TEST_CHECK(ePerp.x >= eOpposite.x);
        TEST_CHECK(eOpposite.x < eFacing.x * 0.1F);
    }

    // 3. Monte Carlo numerical sample accumulation
    {
        std::cout << "  - Test 3: Sample accumulation...\n";
        SphericalHarmonicsL2 sh{};
        sh.addSample(Vec3(1.0F, 0.0F, 0.0F), Vec3(1.0F, 0.0F, 0.0F), 0.5F);
        sh.addSample(Vec3(-1.0F, 0.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), 0.5F);

        Vec3 eRight = sh.evaluateIrradiance(Vec3(1.0F, 0.0F, 0.0F));
        Vec3 eLeft = sh.evaluateIrradiance(Vec3(-1.0F, 0.0F, 0.0F));

        // +X normal should receive primarily red
        TEST_CHECK(eRight.x > eRight.z);
        // -X normal should receive primarily blue
        TEST_CHECK(eLeft.z > eLeft.x);
    }

    // 4. IrradianceProbeComponent weight falloff
    {
        std::cout << "  - Test 4: Probe weight falloff...\n";
        IrradianceProbeComponent probe{};
        probe.position = Vec3(0.0F, 0.0F, 0.0F);
        probe.radius = 10.0F;
        probe.intensity = 1.0F;
        probe.enabled = true;
        probe.useBoxBounds = false;

        // Center must have maximum weight (1.0)
        float wCenter = probe.computeWeight(Vec3(0.0F, 0.0F, 0.0F));
        TEST_CHECK(std::abs(wCenter - 1.0F) < 1e-4F);

        // Halfway to boundary must have smooth falloff: (1 - 0.5^2)^2 = 0.75^2 = 0.5625
        float wMid = probe.computeWeight(Vec3(5.0F, 0.0F, 0.0F));
        TEST_CHECK(std::abs(wMid - 0.5625F) < 1e-3F);

        // At or beyond radius must be exactly 0
        float wEdge = probe.computeWeight(Vec3(10.0F, 0.0F, 0.0F));
        float wOutside = probe.computeWeight(Vec3(15.0F, 0.0F, 0.0F));
        TEST_CHECK(wEdge == 0.0F);
        TEST_CHECK(wOutside == 0.0F);
    }

    std::cout << "[irradiance_probe_test] All Spherical Harmonics & Probe tests PASSED!\n";
    return 0;
}
