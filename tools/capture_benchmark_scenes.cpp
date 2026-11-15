/// @file capture_benchmark_scenes.cpp
/// @brief Offscreen 1080p GPU capture & benchmarking tool for persistent .hikari scenes.
///
/// Purpose:
/// Loads persistent .hikari benchmark scenes from assets/benchmark-scenes/ (Sponza,
/// DamagedHelmet, Bistro, MaterialLab), executes Vulkan GPU rendering at 1080p,
/// profiles frame times, CPU recording, GPU pass timings, and texture residency,
/// and reads back and saves 1080p PPM visual certification images.

#include "core/log.h"
#include "framework/scene_serializer.h"
#include "render/asset_manager.h"
#include "render/camera_component.h"
#include "render/graphics_settings.h"
#include "render/light_components.h"
#include "render/mesh_component.h"
#include "render/post_process_types.h"
#include "render/scene_renderer.h"
#include "rhi/command_list.h"
#include "rhi/rhi_device.h"
#include "world/hierarchy_systems.h"
#include "world/world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
extern "C" const char* __lsan_default_suppressions() {
    return "leak:libdbus-1.so.3\n"
           "leak:libvulkan.so.1\n"
           "leak:libnvidia-glcore.so\n";
}
#endif

using namespace eng;

static constexpr float toRadians(float deg) noexcept {
    return deg * (3.14159265358979323846F / 180.0F);
}

static void savePpm(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "Failed to open PPM output: " << path << '\n';
        return;
    }
    f << "P6\n" << width << " " << height << "\n255\n";
    std::vector<uint8_t> rgb(width * height * 3);
    for (size_t i = 0; i < width * height; ++i) {
        rgb[(i * 3) + 0] = rgba[(i * 4) + 0];
        rgb[(i * 3) + 1] = rgba[(i * 4) + 1];
        rgb[(i * 3) + 2] = rgba[(i * 4) + 2];
    }
    f.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    std::cout << "  [SAVED CAPTURE] " << path << " (" << width << "x" << height << ")\n";
}

struct SceneBenchmarkTelemetry {
    std::string sceneName;
    uint32_t entities{0};
    double avgFrameMs{0.0};
    double medianFrameMs{0.0};
    double p95FrameMs{0.0};
    double p99FrameMs{0.0};
    double maxFrameMs{0.0};
    double avgFps{0.0};
    double avgCpuRecordMs{0.0};
    double avgGpuTotalMs{0.0};
    double avgPbrPassMs{0.0};
    double avgShadowPassMs{0.0};
    double avgDepthPrepassMs{0.0};
    double avgGtaoPassMs{0.0};
    double avgSsrPassMs{0.0};
    double avgTemporalMs{0.0};
    double avgToneMappingMs{0.0};
    double avgBloomMs{0.0};
    size_t residentGpuBytes{0};
    size_t totalGpuBytes{0};
    uint32_t residentTextures{0};
    uint32_t evictionCount{0};
};

int main(int argc, char** argv) {
    std::string outDir = "test-assets";
    if (argc > 1) {
        outDir = argv[1];
    }
    std::filesystem::create_directories(outDir);

    std::cout << "========================================================================\n";
    std::cout << "  Hikari Machine | Milestone 11F Phase 6 Visual & Benchmark Harness     \n";
    std::cout << "  NVIDIA Pascal Tier 0 Baseline Evaluation (1920x1080)                  \n";
    std::cout << "========================================================================\n\n";

    rhi::VulkanDeviceDesc vkDesc{
        .windowHandle = nullptr,
        .enableValidation = false,
    };
    auto devRes = rhi::createVulkanDevice(vkDesc);
    if (!devRes.hasValue()) {
        std::cerr << "Failed to create Vulkan device: " << devRes.error().message << '\n';
        return 1;
    }
    auto device = std::move(devRes.value());

    constexpr uint32_t width = 1920;
    constexpr uint32_t height = 1080;
    constexpr float aspect = static_cast<float>(width) / static_cast<float>(height);

    render::SceneRenderer renderer;
    auto initRes = renderer.init(*device, width, height);
    if (!initRes.hasValue()) {
        std::cerr << "Failed to init SceneRenderer: " << initRes.error().message << '\n';
        return 1;
    }

    const uint64_t byteSize = width * height * 4;
    rhi::BufferDesc rbDesc{
        .sizeInBytes = byteSize,
        .usage = rhi::BufferUsageFlags::TransferDst,
        .memoryUsage = rhi::MemoryUsage::GpuToCpu,
        .debugName = "CaptureReadbackBuffer",
    };
    auto rbRes = device->createBuffer(rbDesc);
    if (!rbRes.hasValue()) {
        std::cerr << "Failed to create readback buffer\n";
        return 1;
    }
    auto rbHandle = rbRes.value();

    std::vector<SceneBenchmarkTelemetry> benchmarkSummary;

    auto benchmarkAndCapture = [&](const std::string& sceneName, const std::string& scenePath,
                                   const Mat4& view, float fovDeg, float nearP, float farP,
                                   const std::string& primaryCaptureFilename,
                                   const std::vector<std::pair<Mat4, std::string>>& extraViews = {},
                                   bool enableSsr = false) {
        std::cout << "\n>>> Benchmarking & Capturing Scene: " << sceneName << " (" << scenePath
                  << ")" << (enableSsr ? " [SSR ON]" : " [SSR OFF]") << "\n";

        world::World world;
        render::AssetManager assetMgr;

        auto desRes = framework::SceneSerializer::deserializeFromFile(world, scenePath);
        if (!desRes.hasValue()) {
            std::cerr << "Failed to deserialize scene: " << desRes.error().message << '\n';
            return;
        }

        uint32_t entityCount = static_cast<uint32_t>(desRes.value().entitiesLoaded);
        std::cout << "  Loaded " << entityCount << " entities from scene graph.\n";

        // Enable standard high-quality PBR settings
        render::GraphicsSettings settings = renderer.getGraphicsSettings();
        settings.postProcess.enabled = true;
        settings.lighting.directLighting = true;
        settings.lighting.iblEnabled = true;
        settings.lighting.probesEnabled = true;
        settings.lighting.reflectionProbeBlending = true;
        settings.lighting.ssrEnabled = enableSsr;
        settings.shadows.enabled = true;
        (void)renderer.applyGraphicsSettings(settings);

        if (desRes.value().postProcessSettings.has_value()) {
            renderer.setPostProcessSettings(*desRes.value().postProcessSettings);
        }

        Mat4 proj = Mat4::perspective(toRadians(fovDeg), aspect, nearP, farP);
        Mat4 primaryViewProj = proj * view;

        // Warm up 15 frames
        for (int i = 0; i < 15; ++i) {
            auto cmdRes = device->createCommandList();
            if (!cmdRes.hasValue())
                break;
            auto cmd = std::move(cmdRes.value());
            cmd->begin();
            renderer.renderScene(world, assetMgr, *device, *cmd, primaryViewProj);
            if (renderer.isPostProcessingEnabled()) {
                renderer.renderPostProcess(*cmd);
            }
            cmd->end();
            (void)device->submit(cmd.get());
            device->waitIdle();
            renderer.updateGpuTimings(*device);
        }

        // Benchmark 50 frames
        constexpr int kBenchFrames = 50;
        std::vector<double> frameTimes;
        std::vector<double> cpuRecordTimes;
        std::vector<double> gpuTotalTimes;
        std::vector<double> pbrPassTimes;
        std::vector<double> shadowPassTimes;
        std::vector<double> depthPrepassTimes;
        std::vector<double> gtaoPassTimes;
        std::vector<double> ssrPassTimes;
        std::vector<double> temporalTimes;
        std::vector<double> toneMappingTimes;
        std::vector<double> bloomTimes;

        frameTimes.reserve(kBenchFrames);
        cpuRecordTimes.reserve(kBenchFrames);
        gpuTotalTimes.reserve(kBenchFrames);

        for (int i = 0; i < kBenchFrames; ++i) {
            auto startFrame = std::chrono::high_resolution_clock::now();

            auto startCpu = std::chrono::high_resolution_clock::now();
            auto cmdRes = device->createCommandList();
            if (!cmdRes.hasValue())
                break;
            auto cmd = std::move(cmdRes.value());

            cmd->begin();
            renderer.renderScene(world, assetMgr, *device, *cmd, primaryViewProj);
            if (renderer.isPostProcessingEnabled()) {
                renderer.renderPostProcess(*cmd);
            }
            cmd->end();
            auto endCpu = std::chrono::high_resolution_clock::now();
            double cpuMs = std::chrono::duration<double, std::milli>(endCpu - startCpu).count();
            cpuRecordTimes.push_back(cpuMs);

            (void)device->submit(cmd.get());
            device->waitIdle();

            auto endFrame = std::chrono::high_resolution_clock::now();
            double frameMs =
                std::chrono::duration<double, std::milli>(endFrame - startFrame).count();
            frameTimes.push_back(frameMs);

            renderer.updateGpuTimings(*device);
            const auto& timings = renderer.getGpuPassTimings();
            if (timings.valid) {
                gpuTotalTimes.push_back(timings.totalSceneMs);
                pbrPassTimes.push_back(timings.mainPbrPassMs);
                shadowPassTimes.push_back(timings.shadowPassMs);
                depthPrepassTimes.push_back(timings.depthPrepassMs);
                gtaoPassTimes.push_back(timings.gtaoPassMs);
                ssrPassTimes.push_back(timings.ssrPassMs);
                temporalTimes.push_back(timings.temporalDenoiseMs);
                toneMappingTimes.push_back(timings.toneMappingMs);
                bloomTimes.push_back(timings.bloomDownsampleMs + timings.bloomUpsampleMs);
            }
        }

        // Capture primary view to readback buffer and write PPM
        {
            auto cmdRes = device->createCommandList();
            if (cmdRes.hasValue()) {
                auto cmd = std::move(cmdRes.value());
                cmd->begin();
                renderer.renderScene(world, assetMgr, *device, *cmd, primaryViewProj);
                if (renderer.isPostProcessingEnabled()) {
                    renderer.renderPostProcess(*cmd);
                }
                rhi::TextureHandle colorTarget = renderer.getColorTarget();
                cmd->copyTextureToBuffer(colorTarget, rbHandle);
                cmd->end();
                (void)device->submit(cmd.get());
                device->waitIdle();

                std::vector<uint8_t> pxBuffer(byteSize, 0);
                auto copyRes = device->readBuffer(rbHandle, 0, pxBuffer.data(), byteSize);
                if (copyRes.hasValue()) {
                    savePpm(outDir + "/" + primaryCaptureFilename, pxBuffer.data(), width, height);
                }
            }
        }

        // Capture extra views if requested
        for (const auto& [extraView, extraFilename] : extraViews) {
            Mat4 extraViewProj = proj * extraView;
            auto cmdRes = device->createCommandList();
            if (cmdRes.hasValue()) {
                auto cmd = std::move(cmdRes.value());
                cmd->begin();
                renderer.renderScene(world, assetMgr, *device, *cmd, extraViewProj);
                if (renderer.isPostProcessingEnabled()) {
                    renderer.renderPostProcess(*cmd);
                }
                rhi::TextureHandle colorTarget = renderer.getColorTarget();
                cmd->copyTextureToBuffer(colorTarget, rbHandle);
                cmd->end();
                (void)device->submit(cmd.get());
                device->waitIdle();

                std::vector<uint8_t> pxBuffer(byteSize, 0);
                auto copyRes = device->readBuffer(rbHandle, 0, pxBuffer.data(), byteSize);
                if (copyRes.hasValue()) {
                    savePpm(outDir + "/" + extraFilename, pxBuffer.data(), width, height);
                }
            }
        }

        // Calculate statistics
        std::sort(frameTimes.begin(), frameTimes.end());
        double avgFrame = std::accumulate(frameTimes.begin(), frameTimes.end(), 0.0) /
                          static_cast<double>(frameTimes.size());
        double medianFrame = frameTimes[frameTimes.size() / 2];
        size_t p95Idx = static_cast<size_t>(static_cast<double>(frameTimes.size()) * 0.95);
        double p95Frame = frameTimes[std::min(p95Idx, frameTimes.size() - 1)];
        size_t p99Idx = static_cast<size_t>(static_cast<double>(frameTimes.size()) * 0.99);
        double p99Frame = frameTimes[std::min(p99Idx, frameTimes.size() - 1)];
        double maxFrame = frameTimes.back();

        double avgCpu = std::accumulate(cpuRecordTimes.begin(), cpuRecordTimes.end(), 0.0) /
                        static_cast<double>(cpuRecordTimes.size());
        double avgGpu = gpuTotalTimes.empty()
                            ? 0.0
                            : (std::accumulate(gpuTotalTimes.begin(), gpuTotalTimes.end(), 0.0) /
                               static_cast<double>(gpuTotalTimes.size()));
        double avgPbr = pbrPassTimes.empty()
                            ? 0.0
                            : (std::accumulate(pbrPassTimes.begin(), pbrPassTimes.end(), 0.0) /
                               static_cast<double>(pbrPassTimes.size()));
        double avgShadow =
            shadowPassTimes.empty()
                ? 0.0
                : (std::accumulate(shadowPassTimes.begin(), shadowPassTimes.end(), 0.0) /
                   static_cast<double>(shadowPassTimes.size()));
        double avgDepth =
            depthPrepassTimes.empty()
                ? 0.0
                : (std::accumulate(depthPrepassTimes.begin(), depthPrepassTimes.end(), 0.0) /
                   static_cast<double>(depthPrepassTimes.size()));
        double avgGtao = gtaoPassTimes.empty()
                             ? 0.0
                             : (std::accumulate(gtaoPassTimes.begin(), gtaoPassTimes.end(), 0.0) /
                                static_cast<double>(gtaoPassTimes.size()));
        double avgSsr = ssrPassTimes.empty()
                            ? 0.0
                            : (std::accumulate(ssrPassTimes.begin(), ssrPassTimes.end(), 0.0) /
                               static_cast<double>(ssrPassTimes.size()));
        double avgTemporal =
            temporalTimes.empty()
                ? 0.0
                : (std::accumulate(temporalTimes.begin(), temporalTimes.end(), 0.0) /
                   static_cast<double>(temporalTimes.size()));
        double avgTone =
            toneMappingTimes.empty()
                ? 0.0
                : (std::accumulate(toneMappingTimes.begin(), toneMappingTimes.end(), 0.0) /
                   static_cast<double>(toneMappingTimes.size()));
        double avgBloom = bloomTimes.empty()
                              ? 0.0
                              : (std::accumulate(bloomTimes.begin(), bloomTimes.end(), 0.0) /
                                 static_cast<double>(bloomTimes.size()));

        const auto& memMetrics = assetMgr.getTextureMemoryMetrics();

        SceneBenchmarkTelemetry t{
            .sceneName = sceneName,
            .entities = entityCount,
            .avgFrameMs = avgFrame,
            .medianFrameMs = medianFrame,
            .p95FrameMs = p95Frame,
            .p99FrameMs = p99Frame,
            .maxFrameMs = maxFrame,
            .avgFps = (avgFrame > 0.0) ? (1000.0 / avgFrame) : 0.0,
            .avgCpuRecordMs = avgCpu,
            .avgGpuTotalMs = avgGpu,
            .avgPbrPassMs = avgPbr,
            .avgShadowPassMs = avgShadow,
            .avgDepthPrepassMs = avgDepth,
            .avgGtaoPassMs = avgGtao,
            .avgSsrPassMs = avgSsr,
            .avgTemporalMs = avgTemporal,
            .avgToneMappingMs = avgTone,
            .avgBloomMs = avgBloom,
            .residentGpuBytes = memMetrics.residentGpuMemoryBytes,
            .totalGpuBytes = memMetrics.estimatedTotalGpuMemoryBytes,
            .residentTextures = memMetrics.residentTextureCount,
            .evictionCount = memMetrics.evictionCountLifetime,
        };

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  [PERF] Avg: " << t.avgFrameMs << " ms (" << t.avgFps
                  << " FPS) | Median: " << t.medianFrameMs << " ms | P95: " << t.p95FrameMs
                  << " ms | P99: " << t.p99FrameMs << " ms | Max: " << t.maxFrameMs << " ms\n";
        std::cout << "  [PASSES] CPU Record: " << t.avgCpuRecordMs
                  << " ms | GPU Total: " << t.avgGpuTotalMs << " ms | PBR: " << t.avgPbrPassMs
                  << " ms | Shadow: " << t.avgShadowPassMs << " ms | Depth: " << t.avgDepthPrepassMs
                  << " ms | GTAO: " << t.avgGtaoPassMs << " ms | SSR: " << t.avgSsrPassMs
                  << " ms | Temporal: " << t.avgTemporalMs
                  << " ms | ToneMap: " << t.avgToneMappingMs << " ms | Bloom: " << t.avgBloomMs
                  << " ms\n";
        std::cout << "  [TEXTURE RESIDENCY] Resident: "
                  << (static_cast<double>(t.residentGpuBytes) / (1024.0 * 1024.0)) << " MB ("
                  << t.residentTextures << " textures) | Total Allocated: "
                  << (static_cast<double>(t.totalGpuBytes) / (1024.0 * 1024.0))
                  << " MB | Lifetime Evictions: " << t.evictionCount << "\n";

        benchmarkSummary.push_back(t);
    };

    // 1. Sponza
    {
        Mat4 viewOverview =
            Mat4::lookAt(Vec3(0.0F, 2.0F, 0.0F), Vec3(0.0F, 2.0F, -6.0F), Vec3::unitY());
        Mat4 viewDetail =
            Mat4::lookAt(Vec3(2.5F, 1.8F, -1.0F), Vec3(3.8F, 1.8F, -1.0F), Vec3::unitY());
        Mat4 viewShadows =
            Mat4::lookAt(Vec3(-4.0F, 5.0F, 2.0F), Vec3(0.0F, 1.0F, -2.0F), Vec3::unitY());

        benchmarkAndCapture("Sponza", "assets/benchmark-scenes/Sponza_Benchmark.hikari",
                            viewOverview, 65.0F, 0.1F, 100.0F, "sponza_persistent_overview.ppm",
                            {{viewDetail, "sponza_persistent_detail_arch.ppm"},
                             {viewShadows, "sponza_persistent_shadows.ppm"}});
    }

    // 2. DamagedHelmet
    {
        Mat4 view = Mat4::lookAt(Vec3(0.0F, 0.0F, 2.5F), Vec3(0.0F, 0.0F, 0.0F), Vec3::unitY());
        benchmarkAndCapture("DamagedHelmet",
                            "assets/benchmark-scenes/DamagedHelmet_Benchmark.hikari", view, 60.0F,
                            0.05F, 50.0F, "damaged_helmet_persistent_scene.ppm");
    }

    // 3. Bistro
    {
        Mat4 viewHero =
            Mat4::lookAt(Vec3(-22.8F, 2.1F, 12.4F), Vec3(-14.2F, 2.2F, 14.5F), Vec3::unitY());
        Mat4 viewStreet =
            Mat4::lookAt(Vec3(-26.43F, 3.16F, 11.17F), Vec3(-16.85F, 2.84F, 14.03F), Vec3::unitY());
        Mat4 viewCourtyard =
            Mat4::lookAt(Vec3(-10.0F, 2.5F, 12.0F), Vec3(0.0F, 2.0F, 10.0F), Vec3::unitY());
        Mat4 viewCafeWindows =
            Mat4::lookAt(Vec3(-18.5F, 2.2F, 13.5F), Vec3(-14.0F, 2.2F, 14.8F), Vec3::unitY());
        Mat4 viewFacadeWindows =
            Mat4::lookAt(Vec3(-20.0F, 2.5F, 10.0F), Vec3(-16.0F, 7.5F, 14.0F), Vec3::unitY());

        benchmarkAndCapture("Bistro", "assets/benchmark-scenes/Bistro_Benchmark.hikari", viewHero,
                            52.0F, 0.1F, 2000.0F, "bistro_hero_corner_match.ppm",
                            {{viewStreet, "bistro_persistent_street.ppm"},
                             {viewCourtyard, "bistro_persistent_courtyard.ppm"},
                             {viewCafeWindows, "bistro_cafe_windows_closeup.ppm"},
                             {viewFacadeWindows, "bistro_building_facade_windows.ppm"}});
    }

    // 4. MaterialLab
    {
        Mat4 viewGrid =
            Mat4::lookAt(Vec3(0.0F, 3.8F, 7.2F), Vec3(0.0F, 0.45F, 0.0F), Vec3::unitY());
        Mat4 viewDielectrics =
            Mat4::lookAt(Vec3(0.0F, 1.8F, 1.5F), Vec3(0.0F, 0.45F, -2.1F), Vec3::unitY());
        Mat4 viewMetals =
            Mat4::lookAt(Vec3(0.0F, 1.8F, 2.5F), Vec3(0.0F, 0.45F, -0.7F), Vec3::unitY());
        Mat4 viewClearcoatSheen =
            Mat4::lookAt(Vec3(0.0F, 1.8F, 3.8F), Vec3(0.0F, 0.45F, 0.7F), Vec3::unitY());
        Mat4 viewAnisoTrans =
            Mat4::lookAt(Vec3(0.0F, 1.8F, 5.0F), Vec3(0.0F, 0.45F, 2.1F), Vec3::unitY());

        benchmarkAndCapture("MaterialLab", "assets/benchmark-scenes/MaterialLab_Benchmark.hikari",
                            viewGrid, 50.0F, 0.1F, 500.0F, "material_lab_1080p_grid.ppm",
                            {{viewDielectrics, "material_lab_dielectrics.ppm"},
                             {viewMetals, "material_lab_metals.ppm"},
                             {viewClearcoatSheen, "material_lab_clearcoat_sheen.ppm"},
                             {viewAnisoTrans, "material_lab_anisotropy_transmission.ppm"}});
    }

    // 5. MaterialLab with Screen-Space Reflections (SSR Comparison)
    {
        Mat4 viewGrid =
            Mat4::lookAt(Vec3(0.0F, 3.8F, 7.2F), Vec3(0.0F, 0.45F, 0.0F), Vec3::unitY());

        benchmarkAndCapture("MaterialLab_SSR",
                            "assets/benchmark-scenes/MaterialLab_Benchmark.hikari", viewGrid, 50.0F,
                            0.1F, 500.0F, "material_lab_1080p_ssr.ppm", {}, true);
    }

    // Final Consolidated Report
    std::cout << "\n==============================================================================="
                 "====================\n";
    std::cout << "  CONSOLIDATED 1080p BENCHMARK TELEMETRY (Pascal Tier 0 Hardware Baseline)\n";
    std::cout << "================================================================================="
                 "==================\n";
    std::cout << std::left << std::setw(15) << "Scene" << std::right << std::setw(10) << "Entities"
              << std::setw(10) << "Avg(ms)" << std::setw(10) << "Med(ms)" << std::setw(10)
              << "P95(ms)" << std::setw(10) << "P99(ms)" << std::setw(10) << "Max(ms)"
              << std::setw(10) << "FPS" << std::setw(12) << "GpuTex(MB)"
              << "\n";
    std::cout << "---------------------------------------------------------------------------------"
                 "------------------\n";
    for (const auto& b : benchmarkSummary) {
        std::cout << std::left << std::setw(15) << b.sceneName << std::right << std::setw(10)
                  << b.entities << std::fixed << std::setprecision(2) << std::setw(10)
                  << b.avgFrameMs << std::setw(10) << b.medianFrameMs << std::setw(10)
                  << b.p95FrameMs << std::setw(10) << b.p99FrameMs << std::setw(10) << b.maxFrameMs
                  << std::setw(10) << b.avgFps << std::setw(12)
                  << (static_cast<double>(b.residentGpuBytes) / (1024.0 * 1024.0)) << "\n";
    }
    std::cout << "================================================================================="
                 "==================\n\n";

    device->destroyBuffer(rbHandle);
    renderer.shutdown();
    std::cout << "Visual certification captures and 1080p benchmarks completed successfully.\n";
    return 0;
}
