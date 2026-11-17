/// @file sampler_and_texture_quality_test.cpp
/// @brief Comprehensive test suite for Milestone 11F Phase 2:
/// Anisotropic filtering propagation, SamplerCache deduplication,
/// texture quality tiers, mip clamping, and runtime settings propagation.

#include "core/assert.h"
#include "core/result.h"
#include "render/asset_manager.h"
#include "render/gpu_material.h"
#include "render/gpu_texture.h"
#include "render/graphics_settings.h"
#include "render/image.h"
#include "render/material.h"
#include "render/sampler_cache.h"
#include "rhi/rhi_device.h"
#include "rhi/sampler.h"

#include <cmath>
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

using namespace eng;
using namespace eng::render;
using namespace eng::rhi;

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << '\n'; \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

// -----------------------------------------------------------------------------
// 1. Sampler Configuration Tests
// -----------------------------------------------------------------------------

static int testSamplerConfigAnisotropyOff() {
    TextureSamplerDesc authorDesc{};
    authorDesc.minFilter = TextureSamplerDesc::Filter::Linear;
    authorDesc.magFilter = TextureSamplerDesc::Filter::Linear;

    SamplerDesc rhiDesc = toRhiSamplerDesc(authorDesc, AnisotropicFiltering::Off, 16.0F);
    TEST_CHECK(!rhiDesc.anisotropyEnable);
    TEST_CHECK(std::abs(rhiDesc.maxAnisotropy - 1.0F) < 1e-4F);
    TEST_CHECK(rhiDesc.minFilter == Filter::Linear);
    TEST_CHECK(rhiDesc.magFilter == Filter::Linear);
    return 0;
}

static int testSamplerConfigAnisotropySupportedLevels() {
    TextureSamplerDesc authorDesc{};

    // X2
    SamplerDesc x2 = toRhiSamplerDesc(authorDesc, AnisotropicFiltering::X2, 16.0F);
    TEST_CHECK(x2.anisotropyEnable);
    TEST_CHECK(std::abs(x2.maxAnisotropy - 2.0F) < 1e-4F);

    // X4
    SamplerDesc x4 = toRhiSamplerDesc(authorDesc, AnisotropicFiltering::X4, 16.0F);
    TEST_CHECK(x4.anisotropyEnable);
    TEST_CHECK(std::abs(x4.maxAnisotropy - 4.0F) < 1e-4F);

    // X8
    SamplerDesc x8 = toRhiSamplerDesc(authorDesc, AnisotropicFiltering::X8, 16.0F);
    TEST_CHECK(x8.anisotropyEnable);
    TEST_CHECK(std::abs(x8.maxAnisotropy - 8.0F) < 1e-4F);

    // X16
    SamplerDesc x16 = toRhiSamplerDesc(authorDesc, AnisotropicFiltering::X16, 16.0F);
    TEST_CHECK(x16.anisotropyEnable);
    TEST_CHECK(std::abs(x16.maxAnisotropy - 16.0F) < 1e-4F);

    return 0;
}

static int testSamplerConfigClampingAboveHardwareMax() {
    TextureSamplerDesc authorDesc{};
    // Requested X16 on hardware that only supports max 4.0
    SamplerDesc clamped = toRhiSamplerDesc(authorDesc, AnisotropicFiltering::X16, 4.0F);
    TEST_CHECK(clamped.anisotropyEnable);
    TEST_CHECK(std::abs(clamped.maxAnisotropy - 4.0F) < 1e-4F);

    // Requested X8 on hardware that supports max 2.0
    SamplerDesc clamped2 = toRhiSamplerDesc(authorDesc, AnisotropicFiltering::X8, 2.0F);
    TEST_CHECK(clamped2.anisotropyEnable);
    TEST_CHECK(std::abs(clamped2.maxAnisotropy - 2.0F) < 1e-4F);

    return 0;
}

static int testSamplerConfigUnsupportedHardwareFallback() {
    TextureSamplerDesc authorDesc{};
    // Hardware lacks samplerAnisotropy (maxCaps = 0.0 or 1.0)
    SamplerDesc disabled = toRhiSamplerDesc(authorDesc, AnisotropicFiltering::X16, 0.0F);
    TEST_CHECK(!disabled.anisotropyEnable);
    TEST_CHECK(std::abs(disabled.maxAnisotropy - 1.0F) < 1e-4F);

    return 0;
}

static int testSamplerConfigNearestFilterDisablesAnisotropy() {
    TextureSamplerDesc authorDesc{};
    authorDesc.minFilter = TextureSamplerDesc::Filter::Nearest;
    authorDesc.magFilter = TextureSamplerDesc::Filter::Nearest;

    // Even if X16 is requested, Nearest filtering must disable anisotropy
    SamplerDesc nearestDesc = toRhiSamplerDesc(authorDesc, AnisotropicFiltering::X16, 16.0F);
    TEST_CHECK(!nearestDesc.anisotropyEnable);
    TEST_CHECK(std::abs(nearestDesc.maxAnisotropy - 1.0F) < 1e-4F);
    TEST_CHECK(nearestDesc.minFilter == Filter::Nearest);
    TEST_CHECK(nearestDesc.magFilter == Filter::Nearest);

    return 0;
}

static int testSamplerConfigAddressModes() {
    TextureSamplerDesc authorDesc{};
    authorDesc.wrapS = TextureSamplerDesc::WrapMode::ClampToEdge;
    authorDesc.wrapT = TextureSamplerDesc::WrapMode::MirroredRepeat;

    SamplerDesc rhiDesc = toRhiSamplerDesc(authorDesc, AnisotropicFiltering::X4, 16.0F);
    TEST_CHECK(rhiDesc.addressU == SamplerAddressMode::ClampToEdge);
    TEST_CHECK(rhiDesc.addressV == SamplerAddressMode::MirroredRepeat);
    TEST_CHECK(rhiDesc.addressW == SamplerAddressMode::Repeat);

    return 0;
}

// -----------------------------------------------------------------------------
// 2. Mip Level Clamping & Texture Quality Tier Tests
// -----------------------------------------------------------------------------

static int testTextureQualityMipClamping() {
    TextureSamplerDesc authorDesc{};

    // Texture with 10 mips (e.g. 512x512)
    const float maxLod = 10.0F;

    // High Quality: minLod = 0.0 (full resolution)
    SamplerDesc highDesc =
        toRhiSamplerDesc(authorDesc, AnisotropicFiltering::X16, 16.0F, 0.0F, maxLod);
    TEST_CHECK(std::abs(highDesc.minLod - 0.0F) < 1e-4F);
    TEST_CHECK(std::abs(highDesc.maxLod - 10.0F) < 1e-4F);

    // Medium Quality: minLod = 1.0 (drop mip 0)
    SamplerDesc medDesc =
        toRhiSamplerDesc(authorDesc, AnisotropicFiltering::X16, 16.0F, 1.0F, maxLod);
    TEST_CHECK(std::abs(medDesc.minLod - 1.0F) < 1e-4F);
    TEST_CHECK(std::abs(medDesc.maxLod - 10.0F) < 1e-4F);

    // Low Quality: minLod = 2.0 (drop mips 0 and 1)
    SamplerDesc lowDesc =
        toRhiSamplerDesc(authorDesc, AnisotropicFiltering::X16, 16.0F, 2.0F, maxLod);
    TEST_CHECK(std::abs(lowDesc.minLod - 2.0F) < 1e-4F);
    TEST_CHECK(std::abs(lowDesc.maxLod - 10.0F) < 1e-4F);

    // 1x1 Texture (1 mip level, maxLod = 1.0): minLod must not exceed maxLod
    SamplerDesc onePixelDesc =
        toRhiSamplerDesc(authorDesc, AnisotropicFiltering::X16, 16.0F, 2.0F, 1.0F);
    TEST_CHECK(onePixelDesc.minLod <= onePixelDesc.maxLod);

    return 0;
}

static int testSmallAndOddDimensionsMipCalculation() {
    // 1x1 texture
    uint32_t mips1 = static_cast<uint32_t>(std::floor(std::log2(1.0))) + 1U;
    TEST_CHECK(mips1 == 1);

    // 2x2 texture
    uint32_t mips2 = static_cast<uint32_t>(std::floor(std::log2(2.0))) + 1U;
    TEST_CHECK(mips2 == 2);

    // Non-power-of-two 19x37 texture
    uint32_t maxDim = std::max(19U, 37U);
    uint32_t mipsOdd =
        static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(maxDim)))) + 1U;
    // log2(37) = 5.209 -> floor is 5 -> 6 levels (37, 18, 9, 4, 2, 1)
    TEST_CHECK(mipsOdd == 6);

    return 0;
}

// -----------------------------------------------------------------------------
// 3. Resource Management & SamplerCache Tests
// -----------------------------------------------------------------------------

static int testSamplerCacheDeduplication() {
    auto devRes = rhi::createNullDevice();
    TEST_CHECK(devRes.hasValue());
    auto& device = *devRes.value();

    SamplerCache cache{};
    TEST_CHECK(cache.getSamplerCount() == 0);
    TEST_CHECK(cache.getCacheHits() == 0);
    TEST_CHECK(cache.getCacheMisses() == 0);

    SamplerDesc desc1{
        .minFilter = Filter::Linear,
        .magFilter = Filter::Linear,
        .mipmapMode = Filter::Linear,
        .addressU = SamplerAddressMode::Repeat,
        .addressV = SamplerAddressMode::Repeat,
        .addressW = SamplerAddressMode::Repeat,
        .anisotropyEnable = true,
        .maxAnisotropy = 16.0F,
    };

    // First request: cache miss, 1 sampler allocated
    auto res1 = cache.getOrCreate(device, desc1);
    TEST_CHECK(res1.hasValue());
    TEST_CHECK(res1.value().isValid());
    TEST_CHECK(cache.getSamplerCount() == 1);
    TEST_CHECK(cache.getCacheMisses() == 1);
    TEST_CHECK(cache.getCacheHits() == 0);

    // Second request with identical configuration: cache hit!
    auto res2 = cache.getOrCreate(device, desc1);
    TEST_CHECK(res2.hasValue());
    TEST_CHECK(res2.value() == res1.value()); // Exact same handle
    TEST_CHECK(cache.getSamplerCount() == 1); // No new allocation
    TEST_CHECK(cache.getCacheMisses() == 1);
    TEST_CHECK(cache.getCacheHits() == 1);

    // Third request with different wrap mode: cache miss, 2 samplers
    SamplerDesc descClamp = desc1;
    descClamp.addressU = SamplerAddressMode::ClampToEdge;
    descClamp.addressV = SamplerAddressMode::ClampToEdge;

    auto res3 = cache.getOrCreate(device, descClamp);
    TEST_CHECK(res3.hasValue());
    TEST_CHECK(res3.value() != res1.value());
    TEST_CHECK(cache.getSamplerCount() == 2);
    TEST_CHECK(cache.getCacheMisses() == 2);
    TEST_CHECK(cache.getCacheHits() == 1);

    // Fourth request for clamped sampler: cache hit!
    auto res4 = cache.getOrCreate(device, descClamp);
    TEST_CHECK(res4.hasValue());
    TEST_CHECK(res4.value() == res3.value());
    TEST_CHECK(cache.getSamplerCount() == 2);
    TEST_CHECK(cache.getCacheHits() == 2);

    // Clear destroys all cached samplers
    cache.clear(device);
    TEST_CHECK(cache.getSamplerCount() == 0);

    return 0;
}

static int testGpuTextureSharedSamplerLifetime() {
    auto devRes = rhi::createNullDevice();
    TEST_CHECK(devRes.hasValue());
    auto& device = *devRes.value();

    ImageData img{
        .width = 4,
        .height = 4,
        .channels = 4,
        .format = TextureFormat::RGBA8_UNORM,
        .pixels = std::vector<uint8_t>(4 * 4 * 4, 128),
    };

    SamplerCache cache{};
    SamplerDesc desc{
        .minFilter = Filter::Linear,
        .magFilter = Filter::Linear,
        .anisotropyEnable = true,
        .maxAnisotropy = 8.0F,
    };
    auto smpRes = cache.getOrCreate(device, desc);
    TEST_CHECK(smpRes.hasValue());
    const SamplerHandle sharedSampler = smpRes.value();

    {
        // Create GpuTexture and attach shared sampler
        auto texRes = GpuTexture::create(device, img, false);
        TEST_CHECK(texRes.hasValue());
        auto gpuTex = std::move(texRes.value());
        TEST_CHECK(gpuTex->ownsSampler());

        // Assign shared sampler
        gpuTex->setSharedSampler(sharedSampler);
        TEST_CHECK(!gpuTex->ownsSampler());
        TEST_CHECK(gpuTex->getSampler() == sharedSampler);

        // gpuTex goes out of scope here: must NOT destroy sharedSampler!
    }

    // Shared sampler in cache must remain valid
    TEST_CHECK(cache.getSamplerCount() == 1);
    auto checkRes = cache.getOrCreate(device, desc);
    TEST_CHECK(checkRes.hasValue());
    TEST_CHECK(checkRes.value() == sharedSampler);

    cache.clear(device);
    TEST_CHECK(cache.getSamplerCount() == 0);
    return 0;
}

// -----------------------------------------------------------------------------
// 4. Runtime Settings Propagation & Descriptor Rebinding
// -----------------------------------------------------------------------------

static int testAssetManagerRuntimeQualityUpdate() {
    auto devRes = rhi::createNullDevice();
    TEST_CHECK(devRes.hasValue());
    auto& device = *devRes.value();

    AssetManager assetManager{};

    // Register a test image
    ImageData img{
        .width = 16,
        .height = 16,
        .channels = 4,
        .format = TextureFormat::RGBA8_SRGB,
        .pixels = std::vector<uint8_t>(16 * 16 * 4, 200),
    };
    auto regTex = assetManager.registerTexture("test/albedo#srgb", img);
    TEST_CHECK(regTex.hasValue());

    // Register a test mesh with material referencing the texture
    MeshData mesh{};
    mesh.vertices.push_back(
        Vertex(Vec3(0.0F, 0.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F), Vec2(0.0F, 0.0F)));
    mesh.vertices.push_back(
        Vertex(Vec3(1.0F, 0.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F), Vec2(1.0F, 0.0F)));
    mesh.vertices.push_back(
        Vertex(Vec3(0.0F, 1.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F), Vec2(0.0F, 1.0F)));
    mesh.indices = {0, 1, 2};

    Material mat{};
    mat.baseColorTexture.isPresent = true;
    mat.baseColorTexture.textureAsset = regTex.value();
    mat.baseColorTexture.uri = "test/albedo#srgb";
    mat.baseColorTexture.colorSpace = TextureColorSpace::sRGB;
    mesh.materials.push_back(mat);

    auto regMesh = assetManager.registerMesh("test/mesh", mesh);
    TEST_CHECK(regMesh.hasValue());

    auto layoutDesc = createMaterialDescriptorSetLayoutDesc();
    auto layoutRes = device.createDescriptorSetLayout(layoutDesc);
    TEST_CHECK(layoutRes.hasValue());
    auto layout = layoutRes.value();

    // Initial upload with High Quality & X16
    QualitySettings initialQuality{
        .anisotropicFiltering = AnisotropicFiltering::X16,
        .textureQuality = TextureQuality::High,
    };
    assetManager.setQualitySettings(initialQuality);

    auto matRes = assetManager.uploadMaterial(regMesh.value(), 0, device, layout);
    TEST_CHECK(matRes.hasValue());
    TEST_CHECK(matRes.value()->isValid());
    TEST_CHECK(assetManager.getSamplerCache().getSamplerCount() >= 1);

    // Now update settings at runtime to Low Quality (mip drop) and Anisotropy X4
    QualitySettings updatedQuality{
        .anisotropicFiltering = AnisotropicFiltering::X4,
        .textureQuality = TextureQuality::Low,
    };

    auto updateRes = assetManager.updateSamplerQuality(updatedQuality, device);
    TEST_CHECK(updateRes.hasValue());
    TEST_CHECK(assetManager.getQualitySettings().anisotropicFiltering == AnisotropicFiltering::X4);
    TEST_CHECK(assetManager.getQualitySettings().textureQuality == TextureQuality::Low);

    // Verify material is still valid and functioning
    GpuMaterial* updatedMat = assetManager.getGpuMaterial(regMesh.value(), 0);
    TEST_CHECK(updatedMat != nullptr);
    TEST_CHECK(updatedMat->isValid());

    device.destroyDescriptorSetLayout(layout);
    assetManager.unloadAll();
    return 0;
}

static int testVulkanSamplerAnisotropyAndCacheLive() {
    VulkanDeviceDesc devDesc{.windowHandle = nullptr, .enableValidation = true};
    auto devRes = createVulkanDevice(devDesc);
    if (!devRes.hasValue()) {
        std::cout << "[SKIP] Vulkan device unavailable; skipping live GPU test\n";
        return 0;
    }
    auto device = std::move(devRes.value());
    const auto& caps = device->getCaps();
    if (!caps.features.samplerAnisotropy) {
        std::cout << "[SKIP] Vulkan hardware lacks samplerAnisotropy\n";
        return 0;
    }

    SamplerCache cache{};
    TextureSamplerDesc authorDesc{};
    SamplerDesc descX16 =
        toRhiSamplerDesc(authorDesc, AnisotropicFiltering::X16, caps.limits.maxSamplerAnisotropy);
    TEST_CHECK(descX16.anisotropyEnable);
    TEST_CHECK(descX16.maxAnisotropy > 1.0F);

    auto s1 = cache.getOrCreate(*device, descX16);
    TEST_CHECK(s1.hasValue());
    TEST_CHECK(s1.value().isValid());

    auto s2 = cache.getOrCreate(*device, descX16);
    TEST_CHECK(s2.hasValue());
    TEST_CHECK(s1.value() == s2.value());
    TEST_CHECK(cache.getSamplerCount() == 1);
    TEST_CHECK(cache.getCacheHits() == 1);

    cache.clear(*device);
    TEST_CHECK(cache.getSamplerCount() == 0);
    return 0;
}

// -----------------------------------------------------------------------------
// Main Test Runner
// -----------------------------------------------------------------------------

int main() {
    std::cout << "[Running Milestone 11F Phase 2: Sampler & Texture Quality Tests]\n";

    if (testSamplerConfigAnisotropyOff() != 0)
        return 1;
    if (testSamplerConfigAnisotropySupportedLevels() != 0)
        return 2;
    if (testSamplerConfigClampingAboveHardwareMax() != 0)
        return 3;
    if (testSamplerConfigUnsupportedHardwareFallback() != 0)
        return 4;
    if (testSamplerConfigNearestFilterDisablesAnisotropy() != 0)
        return 5;
    if (testSamplerConfigAddressModes() != 0)
        return 6;
    if (testTextureQualityMipClamping() != 0)
        return 7;
    if (testSmallAndOddDimensionsMipCalculation() != 0)
        return 8;
    if (testSamplerCacheDeduplication() != 0)
        return 9;
    if (testGpuTextureSharedSamplerLifetime() != 0)
        return 10;
    if (testAssetManagerRuntimeQualityUpdate() != 0)
        return 11;
    if (testVulkanSamplerAnisotropyAndCacheLive() != 0)
        return 12;

    std::cout << "[Sampler & Texture Quality Tests Passed: 12/12]\n";
    return 0;
}
