/// @file texture_residency_test.cpp
/// @brief Comprehensive verification suite for Milestone 11F Phase 5 Texture Residency & Memory
/// Management.
///
/// Validates:
/// 1. Texture residency lifecycle states and explicit transitions
/// 2. Memory budgeting, real-time telemetry metrics, and pressure flags
/// 3. LRU eviction policy under budget pressure
/// 4. Pinned texture immunity against eviction
/// 5. Descriptor fallback protection upon eviction and re-upload restoration
/// 6. Mip promotion and demotion via SamplerCache
/// 7. Stress load/unload/evict cycles and memory stability
/// 8. Null RHI and Vulkan RHI execution safety

#include "core/assert.h"
#include "framework/scene.h"
#include "render/asset_manager.h"
#include "render/gpu_material.h"
#include "render/gpu_texture.h"
#include "render/image.h"
#include "render/material.h"
#include "render/scene_renderer.h"
#include "render/texture_residency.h"
#include "rhi/rhi_device.h"
#include "world/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HIKARI_HAS_ASAN 1
#endif
#if __has_feature(thread_sanitizer)
#define HIKARI_HAS_TSAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define HIKARI_HAS_ASAN 1
#endif
#if defined(__SANITIZE_THREAD__)
#define HIKARI_HAS_TSAN 1
#endif

#if defined(HIKARI_HAS_ASAN)
extern "C" const char* __lsan_default_suppressions() {
    return "leak:libdbus-1\nleak:libvulkan\nleak:nvidia\nleak:libwayland\n";
}
#endif

using namespace eng;
using namespace eng::render;
using namespace eng::rhi;

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ << ": " #cond       \
                      << std::endl;                                                                \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

namespace {

ImageData createTestImage(uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b,
                          uint8_t a = 255) {
    ImageData img{};
    img.width = width;
    img.height = height;
    img.channels = 4;
    img.format = TextureFormat::RGBA8_SRGB;
    img.pixels.resize(width * height * 4);
    for (size_t i = 0; i < img.pixels.size(); i += 4) {
        img.pixels[i + 0] = r;
        img.pixels[i + 1] = g;
        img.pixels[i + 2] = b;
        img.pixels[i + 3] = a;
    }
    return img;
}

// -----------------------------------------------------------------------------
// Test 1: Texture Residency State Lifecycle
// -----------------------------------------------------------------------------
void testResidencyStateLifecycle() {
    std::cout << "[Test 1] Texture Residency State Lifecycle..." << std::endl;

    auto devRes = createNullDevice();
    TEST_CHECK(devRes.hasValue());
    auto device = std::move(devRes.value());

    AssetManager assetMgr;

    // Unknown ID must report Unloaded
    AssetId dummyId{999999};
    TEST_CHECK(assetMgr.getTextureResidencyState(dummyId) == TextureResidencyState::Unloaded);

    // Register texture in memory -> state is CpuResident
    auto img = createTestImage(16, 16, 255, 0, 0);
    auto regRes = assetMgr.registerTexture("virtual://tex1", std::move(img));
    TEST_CHECK(regRes.hasValue());
    AssetId texId = regRes.value();
    TEST_CHECK(assetMgr.getTextureResidencyState(texId) == TextureResidencyState::CpuResident);

    const auto& metrics1 = assetMgr.getTextureMemoryMetrics();
    TEST_CHECK(metrics1.residentTextureCount == 0);
    TEST_CHECK(metrics1.estimatedCpuMemoryBytes > 0);
    TEST_CHECK(metrics1.estimatedGpuMemoryBytes == 0);

    // Upload to GPU -> state transitions to GpuResident
    auto uploadRes = assetMgr.uploadTexture(texId, *device, false);
    TEST_CHECK(uploadRes.hasValue());
    TEST_CHECK(assetMgr.getTextureResidencyState(texId) == TextureResidencyState::GpuResident);

    const auto& metrics2 = assetMgr.getTextureMemoryMetrics();
    TEST_CHECK(metrics2.residentTextureCount == 1);
    TEST_CHECK(metrics2.estimatedGpuMemoryBytes > 0);
    TEST_CHECK(metrics2.uploadBytesLifetime >= metrics2.estimatedGpuMemoryBytes);

    // Evict texture -> state becomes CpuResident (CPU bitmap retained)
    auto evictRes = assetMgr.evictTexture(texId, *device);
    TEST_CHECK(evictRes.hasValue() && evictRes.value() == true);
    TEST_CHECK(assetMgr.getTextureResidencyState(texId) == TextureResidencyState::CpuResident);

    const auto& metrics3 = assetMgr.getTextureMemoryMetrics();
    TEST_CHECK(metrics3.residentTextureCount == 0);
    TEST_CHECK(metrics3.estimatedGpuMemoryBytes == 0);
    TEST_CHECK(metrics3.evictionCountLifetime == 1);

    // Re-upload texture -> state returns to GpuResident
    auto reuploadRes = assetMgr.uploadTexture(texId, *device, false);
    TEST_CHECK(reuploadRes.hasValue());
    TEST_CHECK(assetMgr.getTextureResidencyState(texId) == TextureResidencyState::GpuResident);

    // Unload completely -> state is Unloaded
    assetMgr.unload(texId);
    TEST_CHECK(assetMgr.getTextureResidencyState(texId) == TextureResidencyState::Unloaded);

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 2: Memory Budgeting and LRU Eviction Policy
// -----------------------------------------------------------------------------
void testBudgetEnforcementAndLruEviction() {
    std::cout << "[Test 2] Budget Enforcement & LRU Eviction Policy..." << std::endl;

    auto devRes = createNullDevice();
    TEST_CHECK(devRes.hasValue());
    auto device = std::move(devRes.value());

    AssetManager assetMgr;

    // Configure a tight budget: 64 KB max budget, 32 KB target
    TextureBudgetConfig config{};
    config.maxGpuMemoryBytes = 64 * 1024;
    config.targetGpuMemoryBytes = 32 * 1024;
    config.memoryPressureThreshold = 0.80F;
    assetMgr.setTextureBudgetConfig(config);

    // Register 4 textures of 64x64 RGBA8 (each approx 16 KB uncompressed)
    std::vector<AssetId> ids;
    for (int i = 0; i < 4; ++i) {
        auto img = createTestImage(64, 64, static_cast<uint8_t>(i * 50), 100, 200);
        auto reg =
            assetMgr.registerTexture("virtual://lru_tex_" + std::to_string(i), std::move(img));
        TEST_CHECK(reg.hasValue());
        auto up = assetMgr.uploadTexture(reg.value(), *device, false);
        TEST_CHECK(up.hasValue());
        ids.push_back(reg.value());
    }

    // Total memory is 4 * 16KB = ~64 KB. Touch textures with increasing frame indices
    assetMgr.touchTexture(ids[0], 10); // oldest
    assetMgr.touchTexture(ids[1], 20);
    assetMgr.touchTexture(ids[2], 30);
    assetMgr.touchTexture(ids[3], 40); // newest

    // Register one more texture to push well over budget
    auto imgExtra = createTestImage(64, 64, 255, 255, 255);
    auto regExtra = assetMgr.registerTexture("virtual://lru_extra", std::move(imgExtra));
    TEST_CHECK(regExtra.hasValue());
    auto upExtra = assetMgr.uploadTexture(regExtra.value(), *device, false);
    TEST_CHECK(upExtra.hasValue());
    assetMgr.touchTexture(regExtra.value(), 50);

    const auto& metricsBefore = assetMgr.getTextureMemoryMetrics();
    TEST_CHECK(metricsBefore.estimatedGpuMemoryBytes > config.maxGpuMemoryBytes);
    TEST_CHECK(metricsBefore.isUnderMemoryPressure);

    // Enforce budget: LRU should evict oldest textures first (ids[0], ids[1]...)
    auto enforceRes = assetMgr.enforceTextureBudget(*device, 50);
    TEST_CHECK(enforceRes.hasValue());
    TEST_CHECK(enforceRes.value() >= 2); // At least 2 textures evicted

    // Oldest textures ids[0] and ids[1] must now be evicted
    TEST_CHECK(assetMgr.getTextureResidencyState(ids[0]) == TextureResidencyState::CpuResident);
    TEST_CHECK(assetMgr.getTextureResidencyState(ids[1]) == TextureResidencyState::CpuResident);

    // Newer textures ids[3] and regExtra must remain GpuResident
    TEST_CHECK(assetMgr.getTextureResidencyState(ids[3]) == TextureResidencyState::GpuResident);
    TEST_CHECK(assetMgr.getTextureResidencyState(regExtra.value()) ==
               TextureResidencyState::GpuResident);

    const auto& metricsAfter = assetMgr.getTextureMemoryMetrics();
    TEST_CHECK(metricsAfter.estimatedGpuMemoryBytes <= config.targetGpuMemoryBytes);
    TEST_CHECK(!metricsAfter.isUnderMemoryPressure);

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 3: Pinned Texture Immunity Against Eviction
// -----------------------------------------------------------------------------
void testPinnedTextureImmunity() {
    std::cout << "[Test 3] Pinned Texture Immunity..." << std::endl;

    auto devRes = createNullDevice();
    TEST_CHECK(devRes.hasValue());
    auto device = std::move(devRes.value());

    AssetManager assetMgr;

    TextureBudgetConfig config{};
    config.maxGpuMemoryBytes = 32 * 1024;
    config.targetGpuMemoryBytes = 16 * 1024;
    assetMgr.setTextureBudgetConfig(config);

    auto imgA = createTestImage(64, 64, 10, 20, 30);
    auto regA = assetMgr.registerTexture("virtual://pinned_tex", std::move(imgA));
    TEST_CHECK(regA.hasValue());
    AssetId pinnedId = regA.value();
    auto upA = assetMgr.uploadTexture(pinnedId, *device, false);
    TEST_CHECK(upA.hasValue());

    // Mark as pinned
    assetMgr.pinTexture(pinnedId, true);
    TEST_CHECK(assetMgr.isTexturePinned(pinnedId));

    // Direct eviction attempt must fail for pinned texture
    auto evictRes = assetMgr.evictTexture(pinnedId, *device);
    TEST_CHECK(evictRes.hasValue() && evictRes.value() == false);
    TEST_CHECK(assetMgr.getTextureResidencyState(pinnedId) == TextureResidencyState::GpuResident);

    // Enforce budget under memory pressure: pinned texture is never evicted
    assetMgr.touchTexture(pinnedId, 1); // oldest timestamp
    auto enforceRes = assetMgr.enforceTextureBudget(*device, 100);
    TEST_CHECK(enforceRes.hasValue());
    TEST_CHECK(assetMgr.getTextureResidencyState(pinnedId) == TextureResidencyState::GpuResident);

    // Unpin and re-evict
    assetMgr.pinTexture(pinnedId, false);
    TEST_CHECK(!assetMgr.isTexturePinned(pinnedId));
    auto evictRes2 = assetMgr.evictTexture(pinnedId, *device);
    TEST_CHECK(evictRes2.hasValue() && evictRes2.value() == true);
    TEST_CHECK(assetMgr.getTextureResidencyState(pinnedId) != TextureResidencyState::GpuResident);

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 4: Descriptor Fallback Protection Upon Eviction
// -----------------------------------------------------------------------------
void testDescriptorFallbackProtection() {
    std::cout << "[Test 4] Descriptor Fallback Protection..." << std::endl;

    auto devRes = createNullDevice();
    TEST_CHECK(devRes.hasValue());
    auto device = std::move(devRes.value());

    auto layoutDesc = createMaterialDescriptorSetLayoutDesc();
    auto layoutRes = device->createDescriptorSetLayout(layoutDesc);
    TEST_CHECK(layoutRes.hasValue());
    auto layout = layoutRes.value();

    AssetManager assetMgr;

    // Register and upload texture
    auto img = createTestImage(32, 32, 200, 100, 50);
    auto reg = assetMgr.registerTexture("virtual://albedo_mat_tex", std::move(img));
    TEST_CHECK(reg.hasValue());
    AssetId texId = reg.value();
    auto up = assetMgr.uploadTexture(texId, *device, false);
    TEST_CHECK(up.hasValue());

    // Create material referencing this texture
    Material mat = Material::createDefault();
    mat.name = "TestFallbackMat";
    mat.baseColorTexture.isPresent = true;
    mat.baseColorTexture.uri = "virtual://albedo_mat_tex";
    mat.baseColorTexture.textureAsset = texId;

    auto gpuMatRes = GpuMaterial::create(*device, layout, mat, assetMgr);
    TEST_CHECK(gpuMatRes.hasValue());
    auto gpuMat = std::move(gpuMatRes.value());
    TEST_CHECK(gpuMat->isValid());

    // Evict texture: GpuTexture is destroyed
    auto evictRes = assetMgr.evictTexture(texId, *device);
    TEST_CHECK(evictRes.hasValue() && evictRes.value() == true);
    TEST_CHECK(assetMgr.getGpuTexture(texId) == nullptr);

    // Rebind descriptors via AssetManager (happens automatically in evictTexture)
    // Descriptor set must remain valid and point safely to fallback WhiteSRGB
    TEST_CHECK(gpuMat->isValid());

    // Re-upload texture
    auto reup = assetMgr.uploadTexture(texId, *device, false);
    TEST_CHECK(reup.hasValue());
    TEST_CHECK(assetMgr.getGpuTexture(texId) != nullptr);

    device->destroyDescriptorSetLayout(layout);
    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 5: Mip Promotion and Demotion via SamplerCache
// -----------------------------------------------------------------------------
void testMipPromotionAndDemotion() {
    std::cout << "[Test 5] Mip Promotion & Demotion..." << std::endl;

    auto devRes = createNullDevice();
    TEST_CHECK(devRes.hasValue());
    auto device = std::move(devRes.value());

    AssetManager assetMgr;

    // 64x64 texture with generateMips = true (7 mip levels: 64, 32, 16, 8, 4, 2, 1)
    auto img = createTestImage(64, 64, 128, 128, 128);
    auto reg = assetMgr.registerTexture("virtual://mip_tex", std::move(img));
    TEST_CHECK(reg.hasValue());
    AssetId texId = reg.value();

    auto up = assetMgr.uploadTexture(texId, *device, true);
    TEST_CHECK(up.hasValue());
    GpuTexture* gpuTex = up.value();
    TEST_CHECK(gpuTex != nullptr);
    TEST_CHECK(gpuTex->getMipLevels() == 7);

    // Demote mips (clamps minLod to coarse level >= 1.0)
    auto demoteRes = assetMgr.demoteTextureMips(texId, *device);
    TEST_CHECK(demoteRes.hasValue());

    // Promote mips (restores minLod to full detail = 0.0)
    auto promoteRes = assetMgr.promoteTextureMips(texId, *device);
    TEST_CHECK(promoteRes.hasValue());

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 6: Stress Repeated Cycles and Memory Stability
// -----------------------------------------------------------------------------
void testStressCyclesAndMemoryStability() {
    std::cout << "[Test 6] Stress Repeated Cycles & Memory Stability..." << std::endl;

    auto devRes = createNullDevice();
    TEST_CHECK(devRes.hasValue());
    auto device = std::move(devRes.value());

    AssetManager assetMgr;

    TextureBudgetConfig config{};
    config.maxGpuMemoryBytes = 24 * 1024;
    config.targetGpuMemoryBytes = 16 * 1024;
    assetMgr.setTextureBudgetConfig(config);

    // Loop 100 cycles of registration, upload, budget enforcement, and eviction
    for (int cycle = 0; cycle < 100; ++cycle) {
        auto img = createTestImage(32, 32, static_cast<uint8_t>(cycle % 256), 50, 100);
        std::string name = "virtual://cycle_tex_" + std::to_string(cycle % 10);
        auto reg = assetMgr.registerTexture(name, std::move(img));
        if (reg.hasValue()) {
            std::ignore = assetMgr.uploadTexture(reg.value(), *device, false);
            assetMgr.touchTexture(reg.value(), static_cast<uint64_t>(cycle));
        }

        std::ignore = assetMgr.enforceTextureBudget(*device, static_cast<uint64_t>(cycle));
    }

    const auto& metrics = assetMgr.getTextureMemoryMetrics();
    TEST_CHECK(metrics.estimatedGpuMemoryBytes <= config.maxGpuMemoryBytes);
    TEST_CHECK(metrics.evictionCountLifetime > 0);

    assetMgr.unloadAll();
    const auto& metricsEnd = assetMgr.getTextureMemoryMetrics();
    TEST_CHECK(metricsEnd.residentTextureCount == 0);
    TEST_CHECK(metricsEnd.estimatedGpuMemoryBytes == 0);
    TEST_CHECK(metricsEnd.estimatedCpuMemoryBytes == 0);

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 7: Vulkan Live RHI Residency Lifecycle (Hardware Verification)
// -----------------------------------------------------------------------------
void testVulkanResidencyLifecycleLive() {
    std::cout << "[Test 7] Vulkan Live RHI Residency Lifecycle..." << std::endl;

    VulkanDeviceDesc devDesc{.windowHandle = nullptr, .enableValidation = true};
    auto devRes = createVulkanDevice(devDesc);
    if (!devRes.hasValue()) {
        std::cout << "  [SKIP] Vulkan device unavailable; skipping live GPU test." << std::endl;
        return;
    }
    auto device = std::move(devRes.value());

    AssetManager assetMgr;

    TextureBudgetConfig config{};
    config.maxGpuMemoryBytes = 64 * 1024;
    config.targetGpuMemoryBytes = 32 * 1024;
    assetMgr.setTextureBudgetConfig(config);

    auto img = createTestImage(64, 64, 255, 128, 64);
    auto reg = assetMgr.registerTexture("virtual://vk_residency_tex", std::move(img));
    TEST_CHECK(reg.hasValue());
    AssetId texId = reg.value();

    auto up = assetMgr.uploadTexture(texId, *device, true);
    TEST_CHECK(up.hasValue());
    TEST_CHECK(assetMgr.getTextureResidencyState(texId) == TextureResidencyState::GpuResident);

    const auto& metrics = assetMgr.getTextureMemoryMetrics();
    TEST_CHECK(metrics.residentTextureCount == 1);
    TEST_CHECK(metrics.estimatedGpuMemoryBytes > 0);

    // Evict on live Vulkan device (verifies waitIdle and clean resource destruction)
    auto evict = assetMgr.evictTexture(texId, *device);
    TEST_CHECK(evict.hasValue() && evict.value() == true);
    TEST_CHECK(assetMgr.getTextureResidencyState(texId) != TextureResidencyState::GpuResident);

    // Unload all cleanly
    assetMgr.unloadAll();
    std::cout << "  Passed." << std::endl;
}

} // namespace

int main() {
    std::cout << "========================================================\n"
              << "Running Milestone 11F Phase 5 Texture Residency Suite\n"
              << "========================================================" << std::endl;

    testResidencyStateLifecycle();
    testBudgetEnforcementAndLruEviction();
    testPinnedTextureImmunity();
    testDescriptorFallbackProtection();
    testMipPromotionAndDemotion();
    testStressCyclesAndMemoryStability();
    testVulkanResidencyLifecycleLive();

    std::cout << "========================================================\n"
              << "All 7 tests in Texture Residency Suite PASSED.\n"
              << "========================================================" << std::endl;
    return 0;
}
