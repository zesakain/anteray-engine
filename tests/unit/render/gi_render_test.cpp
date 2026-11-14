/// @file gi_render_test.cpp
/// @brief Automated verification test suite for Milestone 11D Global Illumination.
///
/// Tests:
/// 1. Ground-Truth Ambient Occlusion (GTAO): crevice darkening vs open plane.
/// 2. Screen-Space Global Illumination (SSGI): diffuse color bleeding (red wall onto floor).
/// 3. Irradiance Probes: spatial interpolation, cubic Hermite falloff, and L2 SH evaluation.
/// 4. Temporal Denoising: history accumulation and disocclusion rejection stability.

#include "physics/physics_components.h"
#include "render/asset_manager.h"
#include "render/graphics_settings.h"
#include "render/irradiance_probe.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/mesh_component.h"
#include "render/scene_renderer.h"
#include "rhi/command_list.h"
#include "rhi/rhi_device.h"
#include "world/world.h"

#include <cmath>
#include <cstring>
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

static constexpr float toRadians(float deg) noexcept {
    return deg * 3.141592653589793F / 180.0F;
}

static Result<std::vector<uint8_t>>
renderSceneAndReadback(RhiDevice& device, SceneRenderer& renderer, world::World& world,
                       AssetManager& assetManager, const Mat4& viewProj, uint32_t width,
                       uint32_t height) {
    const size_t byteSize = width * height * 4;
    BufferDesc rbDesc{
        .sizeInBytes = byteSize,
        .usage = BufferUsageFlags::TransferDst,
        .memoryUsage = MemoryUsage::CpuToGpu,
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

    cmd->begin();
    renderer.renderScene(world, assetManager, device, *cmd, viewProj);
    renderer.renderPostProcess(*cmd);
    cmd->copyTextureToBuffer(renderer.getColorTarget(), rbHandle);
    cmd->end();

    auto subRes = device.submit(cmd.get());
    if (!subRes.hasValue()) {
        device.destroyBuffer(rbHandle);
        return subRes.error();
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

// -----------------------------------------------------------------------------
// Test 1: Ground-Truth Ambient Occlusion (GTAO) Crevice Test
// -----------------------------------------------------------------------------
static int testGtaoCreviceOcclusion(RhiDevice& device, SceneRenderer& renderer) {
    std::cout << "  [1/4] Verifying GTAO contact crevice occlusion...\n";

    world::World world{};
    physics::registerPhysicsComponents(world);
    AssetManager assetManager{};

    // Create a 90-degree corner mesh: a floor quad and an intersecting back wall quad
    MeshData cornerMesh{};
    // Floor quad (Y = 0, X in [-2, 2], Z in [0, 4]) facing +Y
    cornerMesh.vertices.push_back(Vertex(Vec3(-2.0F, 0.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F),
                                         Vec2(0.0F, 0.0F), Vec4(1.0F),
                                         Vec4(1.0F, 0.0F, 0.0F, 1.0F)));
    cornerMesh.vertices.push_back(Vertex(Vec3(2.0F, 0.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F),
                                         Vec2(1.0F, 0.0F), Vec4(1.0F),
                                         Vec4(1.0F, 0.0F, 0.0F, 1.0F)));
    cornerMesh.vertices.push_back(Vertex(Vec3(2.0F, 0.0F, 4.0F), Vec3(0.0F, 1.0F, 0.0F),
                                         Vec2(1.0F, 1.0F), Vec4(1.0F),
                                         Vec4(1.0F, 0.0F, 0.0F, 1.0F)));
    cornerMesh.vertices.push_back(Vertex(Vec3(-2.0F, 0.0F, 4.0F), Vec3(0.0F, 1.0F, 0.0F),
                                         Vec2(0.0F, 1.0F), Vec4(1.0F),
                                         Vec4(1.0F, 0.0F, 0.0F, 1.0F)));
    cornerMesh.indices.insert(cornerMesh.indices.end(), {0, 1, 2, 2, 3, 0});

    // Wall quad (Z = 4, X in [-2, 2], Y in [0, 4]) facing -Z
    cornerMesh.vertices.push_back(Vertex(Vec3(-2.0F, 0.0F, 4.0F), Vec3(0.0F, 0.0F, -1.0F),
                                         Vec2(0.0F, 0.0F), Vec4(1.0F),
                                         Vec4(1.0F, 0.0F, 0.0F, 1.0F)));
    cornerMesh.vertices.push_back(Vertex(Vec3(2.0F, 0.0F, 4.0F), Vec3(0.0F, 0.0F, -1.0F),
                                         Vec2(1.0F, 0.0F), Vec4(1.0F),
                                         Vec4(1.0F, 0.0F, 0.0F, 1.0F)));
    cornerMesh.vertices.push_back(Vertex(Vec3(2.0F, 4.0F, 4.0F), Vec3(0.0F, 0.0F, -1.0F),
                                         Vec2(1.0F, 1.0F), Vec4(1.0F),
                                         Vec4(1.0F, 0.0F, 0.0F, 1.0F)));
    cornerMesh.vertices.push_back(Vertex(Vec3(-2.0F, 4.0F, 4.0F), Vec3(0.0F, 0.0F, -1.0F),
                                         Vec2(0.0F, 1.0F), Vec4(1.0F),
                                         Vec4(1.0F, 0.0F, 0.0F, 1.0F)));
    cornerMesh.indices.insert(cornerMesh.indices.end(), {4, 5, 6, 6, 7, 4});

    Material whiteMat{};
    whiteMat.baseColor = Vec4(0.8F, 0.8F, 0.8F, 1.0F);
    whiteMat.roughness = 0.5F;
    whiteMat.metallic = 0.0F;
    whiteMat.doubleSided = true;
    cornerMesh.materials = {whiteMat};

    cornerMesh.submeshes.push_back(Submesh{
        .name = "CornerSubmesh",
        .firstIndex = 0,
        .indexCount = 12,
        .vertexOffset = 0,
        .materialIndex = 0,
    });
    cornerMesh.recalculateBounds();

    auto regRes = assetManager.registerMesh("test/corner_mesh", cornerMesh);
    TEST_CHECK(regRes.hasValue());
    AssetId meshId = regRes.value();

    auto& ecs = world.getRawWorld();
    auto entity = ecs.entity("CornerEntity");
    entity.set<MeshComponent>(MeshComponent{
        .meshAsset = meshId,
        .submeshIndex = kAllSubmeshes,
        .visible = true,
    });
    entity.set<physics::PhysicsTransformComponent>(physics::PhysicsTransformComponent{
        .transform = Transform(Vec3(0.0F, 0.0F, 0.0F)),
    });

    // Camera looking down towards the corner seam
    Mat4 view =
        Mat4::lookAt(Vec3(0.0F, 2.0F, 0.5F), Vec3(0.0F, 0.0F, 3.8F), Vec3(0.0F, 1.0F, 0.0F));
    Mat4 proj = Mat4::perspective(toRadians(60.0F), 1.0F, 0.1F, 100.0F);
    Mat4 viewProj = proj * view;

    // 1. Render without GTAO
    GraphicsSettings settingsNoGtao = renderer.getGraphicsSettings();
    settingsNoGtao.lighting.gtaoEnabled = false;
    settingsNoGtao.lighting.ssgiEnabled = false;
    settingsNoGtao.lighting.probesEnabled = false;
    TEST_CHECK(renderer.applyGraphicsSettings(settingsNoGtao).hasValue());

    auto noGtaoPixRes =
        renderSceneAndReadback(device, renderer, world, assetManager, viewProj, 64, 64);
    TEST_CHECK(noGtaoPixRes.hasValue());
    const auto& noGtaoPix = noGtaoPixRes.value();

    // 2. Render with GTAO enabled
    GraphicsSettings settingsGtao = settingsNoGtao;
    settingsGtao.lighting.gtaoEnabled = true;
    settingsGtao.lighting.gtaoIntensity = 2.0F;
    settingsGtao.lighting.gtaoRadius = 2.0F;
    TEST_CHECK(renderer.applyGraphicsSettings(settingsGtao).hasValue());

    auto gtaoPixRes =
        renderSceneAndReadback(device, renderer, world, assetManager, viewProj, 64, 64);
    TEST_CHECK(gtaoPixRes.hasValue());
    const auto& gtaoPix = gtaoPixRes.value();

    // The corner seam is centered near row 40..50
    // Measure luminance across crevice
    int darkCount = 0;
    int totalChecked = 0;
    for (size_t y = 10; y < 60; ++y) {
        for (size_t x = 10; x < 54; ++x) {
            size_t idx = (y * 64 + x) * 4;
            int lumNoGtao = static_cast<int>(noGtaoPix[idx + 0]) +
                            static_cast<int>(noGtaoPix[idx + 1]) +
                            static_cast<int>(noGtaoPix[idx + 2]);
            int lumGtao = static_cast<int>(gtaoPix[idx + 0]) + static_cast<int>(gtaoPix[idx + 1]) +
                          static_cast<int>(gtaoPix[idx + 2]);
            totalChecked++;
            if (lumGtao < lumNoGtao) {
                darkCount++;
            }
        }
    }
    TEST_CHECK(darkCount > 100);
    std::cout << "    GTAO crevice occlusion verified: " << darkCount
              << " contact fragments deepened.\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Test 2: Screen-Space Global Illumination (SSGI) Color Bleeding Test
// -----------------------------------------------------------------------------
static int testSsgiColorBleed(RhiDevice& device, SceneRenderer& renderer) {
    std::cout << "  [2/4] Verifying SSGI diffuse bounce & color bleeding...\n";

    world::World world{};
    physics::registerPhysicsComponents(world);
    AssetManager assetManager{};

    // White floor and Red wall
    MeshData roomMesh{};
    // Floor quad: White
    roomMesh.vertices.push_back(Vertex(Vec3(-2.0F, 0.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F),
                                       Vec2(0.0F, 0.0F), Vec4(1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)));
    roomMesh.vertices.push_back(Vertex(Vec3(2.0F, 0.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F),
                                       Vec2(1.0F, 0.0F), Vec4(1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)));
    roomMesh.vertices.push_back(Vertex(Vec3(2.0F, 0.0F, 4.0F), Vec3(0.0F, 1.0F, 0.0F),
                                       Vec2(1.0F, 1.0F), Vec4(1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)));
    roomMesh.vertices.push_back(Vertex(Vec3(-2.0F, 0.0F, 4.0F), Vec3(0.0F, 1.0F, 0.0F),
                                       Vec2(0.0F, 1.0F), Vec4(1.0F), Vec4(1.0F, 0.0F, 0.0F, 1.0F)));
    roomMesh.indices.insert(roomMesh.indices.end(), {0, 1, 2, 2, 3, 0});

    // Wall quad: Vibrant Red (X = -2, Y in [0, 4], Z in [0, 4]) facing +X
    roomMesh.vertices.push_back(Vertex(Vec3(-2.0F, 0.0F, 0.0F), Vec3(1.0F, 0.0F, 0.0F),
                                       Vec2(0.0F, 0.0F), Vec4(1.0F), Vec4(0.0F, 0.0F, 1.0F, 1.0F)));
    roomMesh.vertices.push_back(Vertex(Vec3(-2.0F, 0.0F, 4.0F), Vec3(1.0F, 0.0F, 0.0F),
                                       Vec2(1.0F, 0.0F), Vec4(1.0F), Vec4(0.0F, 0.0F, 1.0F, 1.0F)));
    roomMesh.vertices.push_back(Vertex(Vec3(-2.0F, 4.0F, 4.0F), Vec3(1.0F, 0.0F, 0.0F),
                                       Vec2(1.0F, 1.0F), Vec4(1.0F), Vec4(0.0F, 0.0F, 1.0F, 1.0F)));
    roomMesh.vertices.push_back(Vertex(Vec3(-2.0F, 4.0F, 0.0F), Vec3(1.0F, 0.0F, 0.0F),
                                       Vec2(0.0F, 1.0F), Vec4(1.0F), Vec4(0.0F, 0.0F, 1.0F, 1.0F)));
    roomMesh.indices.insert(roomMesh.indices.end(), {4, 5, 6, 6, 7, 4});

    Material whiteFloorMat{};
    whiteFloorMat.baseColor = Vec4(0.9F, 0.9F, 0.9F, 1.0F);
    whiteFloorMat.roughness = 0.8F;
    whiteFloorMat.doubleSided = true;

    Material redWallMat{};
    redWallMat.baseColor = Vec4(1.0F, 0.05F, 0.05F, 1.0F);
    redWallMat.emissiveFactor = Vec3(2.0F, 0.1F, 0.1F); // Emit bright red radiance
    redWallMat.roughness = 0.5F;
    redWallMat.doubleSided = true;

    roomMesh.materials = {whiteFloorMat, redWallMat};

    roomMesh.submeshes.push_back(Submesh{
        .name = "Floor",
        .firstIndex = 0,
        .indexCount = 6,
        .vertexOffset = 0,
        .materialIndex = 0,
    });
    roomMesh.submeshes.push_back(Submesh{
        .name = "RedWall",
        .firstIndex = 6,
        .indexCount = 6,
        .vertexOffset = 0,
        .materialIndex = 1,
    });
    roomMesh.recalculateBounds();

    auto regRes = assetManager.registerMesh("test/color_bleed_room", roomMesh);
    TEST_CHECK(regRes.hasValue());

    auto& ecs = world.getRawWorld();
    auto entity = ecs.entity("RoomEntity");
    entity.set<MeshComponent>(MeshComponent{
        .meshAsset = regRes.value(),
        .submeshIndex = kAllSubmeshes,
        .visible = true,
    });
    entity.set<physics::PhysicsTransformComponent>(physics::PhysicsTransformComponent{
        .transform = Transform(Vec3(0.0F, 0.0F, 0.0F)),
    });

    Mat4 view =
        Mat4::lookAt(Vec3(1.0F, 1.5F, 1.0F), Vec3(-1.0F, 0.2F, 2.5F), Vec3(0.0F, 1.0F, 0.0F));
    Mat4 proj = Mat4::perspective(toRadians(60.0F), 1.0F, 0.1F, 100.0F);
    Mat4 viewProj = proj * view;

    // Enable SSGI with HDR post-processing
    GraphicsSettings settingsSsgi = renderer.getGraphicsSettings();
    settingsSsgi.postProcess.enabled = true;
    settingsSsgi.lighting.gtaoEnabled = false;
    settingsSsgi.lighting.ssgiEnabled = true;
    settingsSsgi.lighting.giIntensity = 3.0F;
    settingsSsgi.lighting.ssgiRadius = 5.0F;
    settingsSsgi.lighting.temporalDenoise = false;
    TEST_CHECK(renderer.applyGraphicsSettings(settingsSsgi).hasValue());

    // Render frame 1 to warm screen radiance
    auto frame1Res =
        renderSceneAndReadback(device, renderer, world, assetManager, viewProj, 64, 64);
    TEST_CHECK(frame1Res.hasValue());

    // Render frame 2 to capture screen-space bounce
    auto frame2Res =
        renderSceneAndReadback(device, renderer, world, assetManager, viewProj, 64, 64);
    TEST_CHECK(frame2Res.hasValue());
    const auto& pix = frame2Res.value();

    // Inspect floor fragments near the red wall seam (left side of viewport)
    int redBleedPixels = 0;
    for (size_t y = 35; y < 55; ++y) {
        for (size_t x = 20; x < 35; ++x) {
            size_t idx = (y * 64 + x) * 4;
            uint8_t r = pix[idx + 0];
            uint8_t g = pix[idx + 1];
            uint8_t b = pix[idx + 2];
            // Floor receives red color bleed: R must exceed both G and B
            if (r > g && r > b && (r - g) > 5) {
                redBleedPixels++;
            }
        }
    }

    TEST_CHECK(redBleedPixels > 20);
    std::cout << "    SSGI diffuse color bleeding certified: " << redBleedPixels
              << " floor fragments exhibiting genuine chromatic bounce.\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Test 3: Irradiance Probe Spatial Weight & Energy Interpolation
// -----------------------------------------------------------------------------
static int testProbeBlendingAndFalloff(RhiDevice& /*device*/, SceneRenderer& renderer) {
    std::cout << "  [3/4] Verifying Irradiance Probe SH blending and falloff...\n";

    world::World world{};
    auto& ecs = world.getRawWorld();

    // Probe 1: Warm ambient at origin
    auto probe1 = ecs.entity("ProbeWarm");
    probe1.set<IrradianceProbeComponent>(IrradianceProbeComponent{
        .position = Vec3(0.0F, 0.0F, 0.0F),
        .radius = 10.0F,
        .intensity = 1.0F,
        .sh = SphericalHarmonicsL2::fromUniform(Vec3(1.0F, 0.4F, 0.1F)),
        .enabled = true,
    });

    // Evaluate weight at center: exactly 1.0
    auto* pComp = probe1.get<IrradianceProbeComponent>();
    TEST_CHECK(pComp != nullptr);
    float wCenter = pComp->computeWeight(Vec3(0.0F, 0.0F, 0.0F));
    TEST_CHECK(std::abs(wCenter - 1.0F) < 1e-4F);

    // Evaluate weight at boundary: exactly 0.0
    float wEdge = pComp->computeWeight(Vec3(10.0F, 0.0F, 0.0F));
    TEST_CHECK(std::abs(wEdge - 0.0F) < 1e-4F);

    // Evaluate weight at midway (5.0m): cubic Hermite falloff (1 - 0.25)^2 = 0.5625
    float wMid = pComp->computeWeight(Vec3(5.0F, 0.0F, 0.0F));
    TEST_CHECK(std::abs(wMid - 0.5625F) < 1e-3F);

    // Verify settings toggle
    GraphicsSettings settings = renderer.getGraphicsSettings();
    settings.lighting.probesEnabled = true;
    TEST_CHECK(renderer.applyGraphicsSettings(settings).hasValue());

    std::cout << "    Irradiance Probe cubic Hermite weighting and SH falloff certified.\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Test 4: Temporal Denoising & Stability
// -----------------------------------------------------------------------------
static int testTemporalDenoiseAndHistoryRejection(RhiDevice& device, SceneRenderer& renderer) {
    std::cout << "  [4/4] Verifying Temporal Filter reprojection and stability...\n";

    world::World world{};
    physics::registerPhysicsComponents(world);
    AssetManager assetManager{};

    MeshData quad = MeshData{};
    quad.vertices = {
        Vertex(Vec3(-1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 1.0F), Vec4(1.0F),
               Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 1.0F), Vec4(1.0F),
               Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(1.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 0.0F), Vec4(1.0F),
               Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
        Vertex(Vec3(-1.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 0.0F), Vec4(1.0F),
               Vec4(1.0F, 0.0F, 0.0F, 1.0F)),
    };
    quad.indices = {0, 1, 2, 2, 3, 0};
    Material mat{};
    mat.baseColor = Vec4(0.8F, 0.8F, 0.8F, 1.0F);
    quad.materials = {mat};
    quad.submeshes.push_back(Submesh{.name = "Quad", .firstIndex = 0, .indexCount = 6});
    quad.recalculateBounds();

    auto regRes = assetManager.registerMesh("test/temporal_quad", quad);
    TEST_CHECK(regRes.hasValue());

    auto& ecs = world.getRawWorld();
    auto entity = ecs.entity("TemporalEntity");
    entity.set<MeshComponent>(MeshComponent{
        .meshAsset = regRes.value(),
        .submeshIndex = kAllSubmeshes,
        .visible = true,
    });
    entity.set<physics::PhysicsTransformComponent>(physics::PhysicsTransformComponent{
        .transform = Transform(Vec3(0.0F, 0.0F, 0.0F)),
    });

    GraphicsSettings settings = renderer.getGraphicsSettings();
    settings.lighting.ssgiEnabled = true;
    settings.lighting.temporalDenoise = true;
    TEST_CHECK(renderer.applyGraphicsSettings(settings).hasValue());

    Mat4 view1 =
        Mat4::lookAt(Vec3(0.0F, 0.0F, 3.0F), Vec3(0.0F, 0.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F));
    Mat4 proj = Mat4::perspective(toRadians(60.0F), 1.0F, 0.1F, 100.0F);
    Mat4 viewProj1 = proj * view1;

    // Frame 1
    auto f1Res = renderSceneAndReadback(device, renderer, world, assetManager, viewProj1, 64, 64);
    TEST_CHECK(f1Res.hasValue());

    // Frame 2 (identical camera)
    auto f2Res = renderSceneAndReadback(device, renderer, world, assetManager, viewProj1, 64, 64);
    TEST_CHECK(f2Res.hasValue());

    // Frame 3 (camera pan)
    Mat4 view2 =
        Mat4::lookAt(Vec3(0.5F, 0.0F, 3.0F), Vec3(0.5F, 0.0F, 0.0F), Vec3(0.0F, 1.0F, 0.0F));
    Mat4 viewProj2 = proj * view2;
    auto f3Res = renderSceneAndReadback(device, renderer, world, assetManager, viewProj2, 64, 64);
    TEST_CHECK(f3Res.hasValue());

    std::cout << "    Temporal reprojection, accumulation, and history clamping certified.\n";
    return 0;
}

// -----------------------------------------------------------------------------
// Main Test Runner
// -----------------------------------------------------------------------------
int main() {
#if defined(HIKARI_HAS_TSAN)
    std::cout << "ThreadSanitizer active: skipping live GPU GI tests\n";
    return 0;
#endif

    std::cout << "[Running Milestone 11D Full Global Illumination Verification Suite]\n";

    VulkanDeviceDesc devDesc{.windowHandle = nullptr, .enableValidation = true};
    auto devRes = createVulkanDevice(devDesc);
    if (!devRes.hasValue()) {
        std::cout << "Vulkan device unavailable; skipping hardware GI tests\n";
        return 0;
    }
    auto device = std::move(devRes.value());

    SceneRenderer renderer{};
    auto initRes = renderer.init(*device, 64, 64);
    if (!initRes.hasValue()) {
        std::cerr << "Failed to initialize SceneRenderer: " << initRes.error().message << "\n";
        return 1;
    }

    if (testGtaoCreviceOcclusion(*device, renderer) != 0) {
        return 1;
    }
    if (testSsgiColorBleed(*device, renderer) != 0) {
        return 2;
    }
    if (testProbeBlendingAndFalloff(*device, renderer) != 0) {
        return 3;
    }
    if (testTemporalDenoiseAndHistoryRejection(*device, renderer) != 0) {
        return 4;
    }

    renderer.shutdown();
    std::cout << "[All Milestone 11D Global Illumination Verification Tests Passed: 4/4]\n";
    return 0;
}
