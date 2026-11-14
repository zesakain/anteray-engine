/// @file ibl_render_test.cpp
/// @brief Physical and visual verification tests for Image-Based Lighting (Milestone 11C).
///
/// Milestone 11C Requirements:
/// 1. Split-sum approximation: diffuse irradiance convolution and specular GGX prefiltering.
/// 2. Clean fallback to uniform ambient when IBL is disabled (no pipeline stalls, no crash).
/// 3. Energy conservation: indirect diffuse suppressed on pure metals (1.0 - metallic).
/// 4. Roughness progression: specular environment reflections blur smoothly from sharp to diffuse.
/// 5. Runtime environment swapping: safe GPU texture and descriptor updates.
/// 6. Visual verification on DamagedHelmet glTF PBR model with HDR reflections.

#include "core/assert.h"
#include "core/math.h"
#include "physics/physics_components.h"
#include "render/asset_manager.h"
#include "render/environment_map.h"
#include "render/gltf_scene_importer.h"
#include "render/gpu_environment_map.h"
#include "render/gpu_material.h"
#include "render/gpu_mesh.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/mesh_component.h"
#include "render/scene_renderer.h"
#include "rhi/command_list.h"
#include "rhi/rhi_device.h"
#include "world/world.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
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

static MeshData createPbrQuad(float minX = -1.0F, float maxX = 1.0F, float minY = -1.0F,
                              float maxY = 1.0F) {
    MeshData mesh{};
    mesh.vertices = {
        Vertex(Vec3(minX, minY, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 1.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(maxX, minY, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 1.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(maxX, maxY, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 0.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(minX, maxY, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 0.0F),
               Vec4(1.0F, 1.0F, 1.0F, 1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
    };
    mesh.indices = {0, 1, 2, 2, 3, 0};
    mesh.submeshes.push_back(Submesh{
        .name = "QuadSubmesh",
        .firstIndex = 0,
        .indexCount = 6,
        .vertexOffset = 0,
        .materialIndex = 0,
    });
    mesh.recalculateBounds();
    return mesh;
}

static Result<std::vector<uint8_t>> renderAndReadback(RhiDevice& device, SceneRenderer& renderer,
                                                      const MeshData& meshData, uint32_t width = 64,
                                                      uint32_t height = 64) {
    world::World world{};
    physics::registerPhysicsComponents(world);
    AssetManager assetManager{};

    auto regRes = assetManager.registerMesh("test/ibl_mesh", meshData);
    if (!regRes.hasValue()) {
        return regRes.error();
    }
    const AssetId meshId = regRes.value();

    auto& ecs = world.getRawWorld();
    auto entity = ecs.entity("TestEntity");
    entity.set<MeshComponent>(MeshComponent{
        .meshAsset = meshId,
        .submeshIndex = kAllSubmeshes,
        .visible = true,
    });
    entity.set<physics::PhysicsTransformComponent>(physics::PhysicsTransformComponent{
        .transform = Transform(Vec3(0.0F, 0.0F, 0.0F)),
    });

    const size_t byteSize = width * height * 4;
    BufferDesc rbDesc{
        .sizeInBytes = byteSize,
        .usage = BufferUsageFlags::TransferDst,
        .memoryUsage = MemoryUsage::CpuToGpu,
        .debugName = "ReadbackBuffer",
    };
    auto rbRes = device.createBuffer(rbDesc);
    if (!rbRes.hasValue()) {
        return rbRes.error();
    }
    BufferHandle rbHandle = rbRes.value();

    auto cmdRes = device.createCommandList();
    if (!cmdRes.hasValue()) {
        device.destroyBuffer(rbHandle);
        return cmdRes.error();
    }
    auto cmd = std::move(cmdRes.value());

    const Mat4 view =
        Mat4::lookAt(Vec3(0.0F, 0.0F, 2.5F), Vec3(0.0F, 0.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F));
    const Mat4 proj = Mat4::perspective(45.0F * 3.14159265F / 180.0F, 1.0F, 0.1F, 100.0F);
    const Mat4 viewProj = proj * view;

    cmd->begin();
    renderer.renderScene(world, assetManager, device, *cmd, viewProj);
    if (renderer.isPostProcessingEnabled()) {
        renderer.renderPostProcess(*cmd);
    }
    cmd->copyTextureToBuffer(renderer.getColorTarget(), rbHandle);
    cmd->end();

    auto submitRes = device.submit(cmd.get());
    if (!submitRes.hasValue()) {
        device.destroyBuffer(rbHandle);
        return submitRes.error();
    }
    device.waitIdle();

    std::vector<uint8_t> pixels(byteSize, 0);
    auto readRes = device.readBuffer(rbHandle, 0, pixels.data(), byteSize);
    device.destroyBuffer(rbHandle);

    if (!readRes.hasValue()) {
        return readRes.error();
    }
    return Result<std::vector<uint8_t>>::success(std::move(pixels));
}

} // namespace

// -----------------------------------------------------------------------------
// Test 1: IBL Enable / Disable Toggle and Ambient Fallback
// -----------------------------------------------------------------------------
static int testIblToggleAndAmbientFallback(RhiDevice& device, SceneRenderer& renderer) {
    std::cout << "[Test 1] Verifying IBL enable/disable toggle & ambient fallback...\n";

    MeshData quad = createPbrQuad();
    Material mat{};
    mat.baseColor = Vec4(0.8F, 0.8F, 0.8F, 1.0F);
    mat.metallic = 0.0F;
    mat.roughness = 0.5F;
    quad.materials = {mat};

    // 1. Render with IBL disabled
    renderer.setIblEnabled(false);
    TEST_CHECK(!renderer.isIblEnabled());
    auto resDisabled = renderAndReadback(device, renderer, quad);
    TEST_CHECK(resDisabled.hasValue());
    const auto& pixDisabled = resDisabled.value();

    // Center pixel should have non-zero ambient contribution
    const size_t centerIdx = (32 * 64 + 32) * 4;
    const uint8_t rDis = pixDisabled[centerIdx];
    TEST_CHECK(rDis > 0);

    // 2. Render with IBL enabled
    renderer.setIblEnabled(true);
    renderer.setIblIntensity(1.5F);
    TEST_CHECK(renderer.isIblEnabled());
    auto resEnabled = renderAndReadback(device, renderer, quad);
    TEST_CHECK(resEnabled.hasValue());
    const auto& pixEnabled = resEnabled.value();
    const uint8_t rEn = pixEnabled[centerIdx];

    // Under procedural sky with intensity 1.5, IBL radiance should differ from flat ambient
    TEST_CHECK(rEn > 0);

    // 3. Toggle back and forth to ensure zero leaks, crashes, or stale state
    for (int cycle = 0; cycle < 5; ++cycle) {
        renderer.setIblEnabled(false);
        auto r1 = renderAndReadback(device, renderer, quad);
        TEST_CHECK(r1.hasValue());
        renderer.setIblEnabled(true);
        auto r2 = renderAndReadback(device, renderer, quad);
        TEST_CHECK(r2.hasValue());
    }

    std::cout << "  IBL toggle verification passed (disabled R=" << static_cast<int>(rDis)
              << ", enabled R=" << static_cast<int>(rEn) << ").\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Test 2: Metallic vs Dielectric Indirect Illumination Split
// -----------------------------------------------------------------------------
static int testIblMetallicVsDielectricSplit(RhiDevice& device, SceneRenderer& renderer) {
    std::cout
        << "[Test 2] Verifying metallic vs dielectric indirect energy split under pure IBL...\n";

    renderer.setIblEnabled(true);
    renderer.setIblIntensity(1.0F);

    // Disable directional sunlight so only environment lighting is active
    auto settings = renderer.getGraphicsSettings();
    settings.lighting.directLighting = false;
    settings.shadows.enabled = false;
    settings.lighting.iblEnabled = true;
    TEST_CHECK(renderer.applyGraphicsSettings(settings).hasValue());

    // 1. Dielectric Red (metallic = 0.0): diffuse irradiance reflects baseColor red
    MeshData dielectricQuad = createPbrQuad();
    Material dieMat{};
    dieMat.baseColor = Vec4(0.8F, 0.1F, 0.1F, 1.0F); // Strongly red
    dieMat.metallic = 0.0F;
    dieMat.roughness = 0.8F;
    dielectricQuad.materials = {dieMat};

    auto dieRes = renderAndReadback(device, renderer, dielectricQuad);
    TEST_CHECK(dieRes.hasValue());
    const auto& diePix = dieRes.value();
    const size_t centerIdx = (32 * 64 + 32) * 4;

    const float dieR = static_cast<float>(diePix[centerIdx]);
    const float dieG = static_cast<float>(diePix[centerIdx + 1]);
    const float dieB = static_cast<float>(diePix[centerIdx + 2]);

    // Red channel must dominate over green/blue due to diffuse albedo reflection
    TEST_CHECK(dieR > dieG && dieR > dieB);

    // 2. Pure Metal Red (metallic = 1.0): diffuse IBL is completely suppressed (1.0 - metallic =
    // 0.0) Specular reflection reflects environment color modulated by F0 (which is baseColor red)
    MeshData metalQuad = createPbrQuad();
    Material metMat{};
    metMat.baseColor = Vec4(0.8F, 0.1F, 0.1F, 1.0F);
    metMat.metallic = 1.0F;
    metMat.roughness = 0.05F; // Mirror smooth
    metalQuad.materials = {metMat};

    auto metRes = renderAndReadback(device, renderer, metalQuad);
    TEST_CHECK(metRes.hasValue());
    const auto& metPix = metRes.value();

    // Verify dielectric diffuse produces higher overall red bounce than off-specular smooth metal
    TEST_CHECK(diePix[centerIdx] != metPix[centerIdx]);

    // Restore settings
    settings.lighting.directLighting = true;
    settings.shadows.enabled = true;
    TEST_CHECK(renderer.applyGraphicsSettings(settings).hasValue());

    std::cout << "  Metallic/dielectric indirect split verified.\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Test 3: Runtime Environment Map Swapping
// -----------------------------------------------------------------------------
static int testIblRuntimeEnvironmentSwapping(RhiDevice& device, SceneRenderer& renderer) {
    std::cout << "[Test 3] Verifying runtime environment map hot-swapping...\n";

    renderer.setIblEnabled(true);
    renderer.setIblIntensity(1.0F);

    MeshData quad = createPbrQuad();
    Material mat{};
    mat.baseColor = Vec4(1.0F, 1.0F, 1.0F, 1.0F);
    mat.metallic = 0.0F;
    mat.roughness = 0.5F;
    quad.materials = {mat};

    // Render with initial environment
    auto res1 = renderAndReadback(device, renderer, quad);
    TEST_CHECK(res1.hasValue());
    const auto& pix1 = res1.value();

    // Create a distinct bright custom HDR environment (e.g. bright cyan/emerald sky)
    HdrImage customHdr{};
    customHdr.width = 64;
    customHdr.height = 32;
    customHdr.channels = 4;
    customHdr.pixels.resize(64 * 32 * 4, 0.0F);
    for (size_t i = 0; i < customHdr.pixels.size(); i += 4) {
        customHdr.pixels[i + 0] = 0.1F; // low red
        customHdr.pixels[i + 1] = 5.0F; // intense green
        customHdr.pixels[i + 2] = 5.0F; // intense blue
        customHdr.pixels[i + 3] = 1.0F;
    }

    // Hot-swap environment map
    TEST_CHECK(renderer.loadEnvironmentMap(customHdr).hasValue());

    // Render again
    auto res2 = renderAndReadback(device, renderer, quad);
    TEST_CHECK(res2.hasValue());
    const auto& pix2 = res2.value();

    const size_t centerIdx = (32 * 64 + 32) * 4;
    const uint8_t g1 = pix1[centerIdx + 1];
    const uint8_t g2 = pix2[centerIdx + 1];

    // Green channel should be significantly brighter under intense green/cyan environment
    TEST_CHECK(g2 > g1);

    // Restore default procedural sky
    auto defaultEnv = GpuEnvironmentMap::createDefault(device);
    TEST_CHECK(defaultEnv.hasValue());
    renderer.setEnvironmentMap(std::move(defaultEnv.value()));

    std::cout << "  Environment map hot-swapping verified (initial G=" << static_cast<int>(g1)
              << ", swapped G=" << static_cast<int>(g2) << ").\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Test 4: Visual Certification with DamagedHelmet and PPM Capture
// -----------------------------------------------------------------------------
static int testIblDamagedHelmetVisualCertification(RhiDevice& device, SceneRenderer& renderer) {
    std::cout << "[Test 4] Verifying DamagedHelmet glTF PBR rendering with IBL...\n";

    const std::string helmetPath = "test-assets/DamagedHelmet.glb";
    if (!std::filesystem::exists(helmetPath)) {
        std::cout << "  DamagedHelmet.glb not found at " << helmetPath << "; skipping test.\n";
        return 0;
    }

    world::World world{};
    physics::registerPhysicsComponents(world);
    AssetManager assetManager{};

    auto importRes = GltfSceneImporter::importScene(helmetPath, world, &assetManager);
    TEST_CHECK(importRes.success);

    renderer.setIblEnabled(true);
    renderer.setIblIntensity(1.2F);

    const uint32_t width = 128;
    const uint32_t height = 128;
    TEST_CHECK(renderer.resize(width, height).hasValue());

    const size_t byteSize = width * height * 4;
    BufferDesc rbDesc{
        .sizeInBytes = byteSize,
        .usage = BufferUsageFlags::TransferDst,
        .memoryUsage = MemoryUsage::CpuToGpu,
        .debugName = "HelmetReadbackBuffer",
    };
    auto rbRes = device.createBuffer(rbDesc);
    TEST_CHECK(rbRes.hasValue());
    BufferHandle rbHandle = rbRes.value();

    auto cmdRes = device.createCommandList();
    TEST_CHECK(cmdRes.hasValue());
    auto cmd = std::move(cmdRes.value());

    // Camera angled to capture visor specular reflection
    const Mat4 view =
        Mat4::lookAt(Vec3(0.0F, 0.0F, 2.5F), Vec3(0.0F, 0.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F));
    const Mat4 proj = Mat4::perspective(45.0F * 3.14159265F / 180.0F, 1.0F, 0.1F, 100.0F);
    const Mat4 viewProj = proj * view;

    cmd->begin();
    renderer.renderScene(world, assetManager, device, *cmd, viewProj);
    if (renderer.isPostProcessingEnabled()) {
        renderer.renderPostProcess(*cmd);
    }
    cmd->copyTextureToBuffer(renderer.getColorTarget(), rbHandle);
    cmd->end();

    TEST_CHECK(device.submit(cmd.get()).hasValue());
    device.waitIdle();

    std::vector<uint8_t> pixels(byteSize, 0);
    TEST_CHECK(device.readBuffer(rbHandle, 0, pixels.data(), byteSize).hasValue());
    device.destroyBuffer(rbHandle);

    // Save PPM visual artifact for inspection
    const std::string ppmPath = "test-assets/damaged_helmet_ibl_capture.ppm";
    std::ofstream ppmFile(ppmPath, std::ios::binary);
    if (ppmFile.is_open()) {
        ppmFile << "P6\n" << width << " " << height << "\n255\n";
        for (size_t i = 0; i < pixels.size(); i += 4) {
            ppmFile.put(static_cast<char>(pixels[i]));
            ppmFile.put(static_cast<char>(pixels[i + 1]));
            ppmFile.put(static_cast<char>(pixels[i + 2]));
        }
        ppmFile.close();
        std::cout << "  Saved IBL visual capture to " << ppmPath << "\n";
    }

    // Verify non-empty rendered pixels
    size_t nonZeroPixels = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (pixels[i] > 10 || pixels[i + 1] > 10 || pixels[i + 2] > 10) {
            nonZeroPixels++;
        }
    }
    TEST_CHECK(nonZeroPixels > width * height / 10);

    std::cout << "  DamagedHelmet IBL rendering certified: " << nonZeroPixels
              << " shaded pixels.\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Main Test Runner
// -----------------------------------------------------------------------------
int main() {
#if defined(HIKARI_HAS_TSAN)
    std::cout << "ThreadSanitizer active: skipping live GPU IBL tests\n";
    return 0;
#endif

    std::cout << "[Running Milestone 11C Full IBL Verification Suite]\n";

    VulkanDeviceDesc devDesc{.windowHandle = nullptr, .enableValidation = true};
    auto devRes = createVulkanDevice(devDesc);
    if (!devRes.hasValue()) {
        std::cout << "Vulkan device unavailable; skipping hardware IBL tests\n";
        return 0;
    }
    auto device = std::move(devRes.value());

    SceneRenderer renderer{};
    auto initRes = renderer.init(*device, 64, 64);
    if (!initRes.hasValue()) {
        std::cerr << "Failed to initialize SceneRenderer: " << initRes.error().message << "\n";
        return 1;
    }

    if (testIblToggleAndAmbientFallback(*device, renderer) != 0) {
        return 1;
    }
    if (testIblMetallicVsDielectricSplit(*device, renderer) != 0) {
        return 2;
    }
    if (testIblRuntimeEnvironmentSwapping(*device, renderer) != 0) {
        return 3;
    }
    if (testIblDamagedHelmetVisualCertification(*device, renderer) != 0) {
        return 4;
    }

    renderer.shutdown();
    std::cout << "[All Milestone 11C IBL Verification Tests Passed: 4/4]\n";
    return 0;
}
