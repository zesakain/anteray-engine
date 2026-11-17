/// @file benchmark_gi_1080p.cpp
/// @brief Official 1080p Benchmark & Certification Harness for Milestone 11D Global Illumination.

#include "core/log.h"
#include "framework/scene_serializer.h"
#include "render/asset_manager.h"
#include "render/camera_component.h"
#include "render/graphics_settings.h"
#include "render/irradiance_probe.h"
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
    std::cout << "[SAVED] " << path << " (" << width << "x" << height << ")\n";
}

struct GiTelemetry {
    std::string scene;
    std::string config;
    double avgFrameTimeMs{0.0};
    double p95FrameTimeMs{0.0};
    double p99FrameTimeMs{0.0};
    double avgFps{0.0};
    double avgGtaoMs{0.0};
    double avgSsgiMs{0.0};
    double avgTemporalDenoiseMs{0.0};
    double avgTotalGiMs{0.0};
    double avgCpuRecordMs{0.0};
    uint32_t drawCalls{0};
    uint32_t triangles{0};
};

int main(int argc, char** argv) {
    std::string outDir = "test-assets";
    if (argc > 1) {
        outDir = argv[1];
    }
    std::filesystem::create_directories(outDir);

    std::cout << "========================================================================\n";
    std::cout << "  ANTERAY ENGINE | Milestone 11D Global Illumination 1080p Benchmark    \n";
    std::cout << "  NVIDIA Pascal Tier 0 Baseline Evaluation: Bistro & Sponza             \n";
    std::cout << "========================================================================\n\n";

    rhi::VulkanDeviceDesc devDesc{
        .windowHandle = nullptr,
        .enableValidation = false,
    };
    auto devRes = rhi::createVulkanDevice(devDesc);
    if (!devRes.hasValue()) {
        std::cerr << "Vulkan device unavailable: " << devRes.error().message << "\n";
        return 1;
    }
    auto device = std::move(devRes.value());

    constexpr uint32_t width = 1920;
    constexpr uint32_t height = 1080;

    render::SceneRenderer renderer;
    auto initRes = renderer.init(*device, width, height);
    if (!initRes.hasValue()) {
        std::cerr << "Failed to init SceneRenderer: " << initRes.error().message << "\n";
        return 1;
    }

    const uint64_t byteSize = width * height * 4;
    rhi::BufferDesc rbDesc{
        .sizeInBytes = byteSize,
        .usage = rhi::BufferUsageFlags::TransferDst,
        .memoryUsage = rhi::MemoryUsage::GpuToCpu,
        .debugName = "ReadbackBuffer",
    };
    auto rbRes = device->createBuffer(rbDesc);
    if (!rbRes.hasValue()) {
        return 1;
    }
    auto rbHandle = rbRes.value();

    std::vector<GiTelemetry> results;

    auto benchmarkScene = [&](const std::string& sceneName, const std::string& scenePath,
                              const Mat4& viewProj, bool enableGi, const std::string& captureName) {
        world::World world;
        render::AssetManager assetMgr;

        auto desRes = framework::SceneSerializer::deserializeFromFile(world, scenePath);
        if (!desRes.hasValue()) {
            std::cerr << "Failed to load scene: " << scenePath << "\n";
            return;
        }

        render::GraphicsSettings settings = renderer.getGraphicsSettings();
        settings.postProcess.enabled = true;
        settings.lighting.directLighting = true;
        settings.lighting.iblEnabled = true;

        if (enableGi) {
            settings.lighting.gtaoEnabled = true;
            settings.lighting.gtaoIntensity = 1.2F;
            settings.lighting.gtaoRadius = 1.8F;
            settings.lighting.ssgiEnabled = true;
            settings.lighting.giIntensity = 1.0F;
            settings.lighting.ssgiRadius = 4.0F;
            settings.lighting.temporalDenoise = true;
            settings.lighting.probesEnabled = true;
        } else {
            settings.lighting.gtaoEnabled = false;
            settings.lighting.ssgiEnabled = false;
            settings.lighting.temporalDenoise = false;
            settings.lighting.probesEnabled = false;
        }

        if (renderer.applyGraphicsSettings(settings).isError()) {
            std::cerr << "Failed to apply graphics settings\n";
            return;
        }

        if (desRes.value().postProcessSettings.has_value()) {
            renderer.setPostProcessSettings(*desRes.value().postProcessSettings);
        }

        // Warm up 15 frames
        for (int i = 0; i < 15; ++i) {
            auto cmdRes = device->createCommandList();
            if (!cmdRes.hasValue())
                break;
            auto cmd = std::move(cmdRes.value());
            cmd->begin();
            renderer.renderScene(world, assetMgr, *device, *cmd, viewProj);
            if (renderer.isPostProcessingEnabled()) {
                renderer.renderPostProcess(*cmd);
            }
            cmd->end();
            std::ignore = device->submit(cmd.get());
            device->waitIdle();
            renderer.updateGpuTimings(*device);
        }

        // Benchmark 50 frames
        std::vector<double> frameTimes;
        std::vector<double> gtaoTimes;
        std::vector<double> ssgiTimes;
        std::vector<double> temporalTimes;
        std::vector<double> totalGiTimes;
        std::vector<double> cpuRecordTimes;

        for (int i = 0; i < 50; ++i) {
            auto startCpu = std::chrono::high_resolution_clock::now();
            auto cmdRes = device->createCommandList();
            if (!cmdRes.hasValue())
                break;
            auto cmd = std::move(cmdRes.value());
            cmd->begin();
            renderer.renderScene(world, assetMgr, *device, *cmd, viewProj);
            if (renderer.isPostProcessingEnabled()) {
                renderer.renderPostProcess(*cmd);
            }
            cmd->end();
            auto endCpu = std::chrono::high_resolution_clock::now();
            double cpuMs = std::chrono::duration<double, std::milli>(endCpu - startCpu).count();
            cpuRecordTimes.push_back(cpuMs);

            auto startTotal = std::chrono::high_resolution_clock::now();
            std::ignore = device->submit(cmd.get());
            device->waitIdle();
            auto endTotal = std::chrono::high_resolution_clock::now();
            double totalMs =
                std::chrono::duration<double, std::milli>(endTotal - startTotal).count();
            frameTimes.push_back(totalMs);

            renderer.updateGpuTimings(*device);
            const auto& timings = renderer.getGpuPassTimings();
            if (timings.valid) {
                gtaoTimes.push_back(timings.gtaoPassMs);
                ssgiTimes.push_back(timings.ssgiPassMs);
                temporalTimes.push_back(timings.temporalDenoiseMs);
                totalGiTimes.push_back(timings.totalGiMs);
            }
        }

        if (!captureName.empty()) {
            auto cmdRes = device->createCommandList();
            if (cmdRes.hasValue()) {
                auto cmd = std::move(cmdRes.value());
                cmd->begin();
                cmd->copyTextureToBuffer(renderer.getColorTarget(), rbHandle);
                cmd->end();
                std::ignore = device->submit(cmd.get());
                device->waitIdle();
                std::vector<uint8_t> pxBuffer(byteSize, 0);
                if (device->readBuffer(rbHandle, 0, pxBuffer.data(), byteSize).hasValue()) {
                    savePpm(outDir + "/" + captureName, pxBuffer.data(), width, height);
                }
            }
        }

        std::sort(frameTimes.begin(), frameTimes.end());
        double avgFt = std::accumulate(frameTimes.begin(), frameTimes.end(), 0.0) /
                       static_cast<double>(frameTimes.size());
        double p95Ft =
            frameTimes[static_cast<size_t>(static_cast<double>(frameTimes.size()) * 0.95)];
        double p99Ft =
            frameTimes[static_cast<size_t>(static_cast<double>(frameTimes.size()) * 0.99)];
        double avgGtao = gtaoTimes.empty()
                             ? 0.0
                             : std::accumulate(gtaoTimes.begin(), gtaoTimes.end(), 0.0) /
                                   static_cast<double>(gtaoTimes.size());
        double avgSsgi = ssgiTimes.empty()
                             ? 0.0
                             : std::accumulate(ssgiTimes.begin(), ssgiTimes.end(), 0.0) /
                                   static_cast<double>(ssgiTimes.size());
        double avgTemp = temporalTimes.empty()
                             ? 0.0
                             : std::accumulate(temporalTimes.begin(), temporalTimes.end(), 0.0) /
                                   static_cast<double>(temporalTimes.size());
        double avgGi = totalGiTimes.empty()
                           ? 0.0
                           : std::accumulate(totalGiTimes.begin(), totalGiTimes.end(), 0.0) /
                                 static_cast<double>(totalGiTimes.size());
        double avgCpu = std::accumulate(cpuRecordTimes.begin(), cpuRecordTimes.end(), 0.0) /
                        static_cast<double>(cpuRecordTimes.size());

        GiTelemetry telem{
            .scene = sceneName,
            .config = enableGi ? "Milestone 11D Full GI" : "Milestone 11C Baseline",
            .avgFrameTimeMs = avgFt,
            .p95FrameTimeMs = p95Ft,
            .p99FrameTimeMs = p99Ft,
            .avgFps = 1000.0 / avgFt,
            .avgGtaoMs = avgGtao,
            .avgSsgiMs = avgSsgi,
            .avgTemporalDenoiseMs = avgTemp,
            .avgTotalGiMs = avgGi,
            .avgCpuRecordMs = avgCpu,
            .drawCalls = renderer.getLastRenderStats().drawCalls,
            .triangles = renderer.getLastRenderStats().triangles,
        };
        results.push_back(telem);

        std::cout << ">>> [" << sceneName << " | " << (enableGi ? "11D Full GI" : "11C Baseline")
                  << "]\n";
        std::cout << "    Frame Time: " << std::fixed << std::setprecision(2) << avgFt
                  << " ms (P95: " << p95Ft << " ms, P99: " << p99Ft << " ms) -> "
                  << (1000.0 / avgFt) << " FPS\n";
        if (enableGi) {
            std::cout << "    GI GPU Breakdown: GTAO=" << avgGtao << " ms, SSGI=" << avgSsgi
                      << " ms, Temporal=" << avgTemp << " ms -> Total GI=" << avgGi << " ms\n";
        }
        std::cout << "    CPU Recording: " << avgCpu << " ms | Draws: " << telem.drawCalls
                  << " | Triangles: " << telem.triangles << "\n\n";
    };

    // 1. Bistro Street Cameras
    Mat4 bistroStreetView =
        Mat4::lookAt(Vec3(-26.43F, 3.16F, 11.17F), Vec3(-16.85F, 2.84F, 14.03F), Vec3::unitY());
    Mat4 proj1080p = Mat4::perspective(
        toRadians(65.0F), static_cast<float>(width) / static_cast<float>(height), 0.1F, 2000.0F);

    benchmarkScene("Amazon Bistro", "assets/benchmark-scenes/Bistro_Benchmark.hikari",
                   proj1080p * bistroStreetView, false, "Bistro_1080p_Baseline_11C.ppm");
    benchmarkScene("Amazon Bistro", "assets/benchmark-scenes/Bistro_Benchmark.hikari",
                   proj1080p * bistroStreetView, true, "Bistro_1080p_Full_GI_11D.ppm");

    // 2. Bistro Courtyard Camera
    Mat4 bistroCourtyardView =
        Mat4::lookAt(Vec3(-10.0F, 2.5F, 12.0F), Vec3(0.0F, 2.0F, 10.0F), Vec3::unitY());
    benchmarkScene("Amazon Bistro (Courtyard)", "assets/benchmark-scenes/Bistro_Benchmark.hikari",
                   proj1080p * bistroCourtyardView, true, "Bistro_Courtyard_Full_GI_11D.ppm");

    // 3. Sponza Atrium Camera
    Mat4 sponzaView = Mat4::lookAt(Vec3(0.0F, 2.0F, 0.0F), Vec3(0.0F, 2.0F, -6.0F), Vec3::unitY());
    Mat4 sponzaProj = Mat4::perspective(
        toRadians(65.0F), static_cast<float>(width) / static_cast<float>(height), 0.1F, 100.0F);

    benchmarkScene("Khronos Sponza", "assets/benchmark-scenes/Sponza_Benchmark.hikari",
                   sponzaProj * sponzaView, false, "Sponza_1080p_Baseline_11C.ppm");
    benchmarkScene("Khronos Sponza", "assets/benchmark-scenes/Sponza_Benchmark.hikari",
                   sponzaProj * sponzaView, true, "Sponza_1080p_Full_GI_11D.ppm");

    device->destroyBuffer(rbHandle);
    renderer.shutdown();

    std::cout << "========================================================================\n";
    std::cout << "  MILESTONE 11D BENCHMARK SUMMARY (NATIVE 1080P)\n";
    std::cout << "========================================================================\n";
    std::cout << std::left << std::setw(28) << "Scene" << std::setw(24) << "Configuration"
              << std::setw(12) << "FPS" << std::setw(14) << "Avg Frame" << std::setw(14)
              << "P95 Frame" << std::setw(14) << "Total GI"
              << "\n";
    std::cout << "---------------------------------------------------------------------------------"
                 "------------------------\n";
    for (const auto& r : results) {
        std::cout << std::left << std::setw(28) << r.scene << std::setw(24) << r.config
                  << std::setw(12) << std::fixed << std::setprecision(1) << r.avgFps
                  << std::setw(14) << std::fixed << std::setprecision(2)
                  << (std::to_string(r.avgFrameTimeMs).substr(0, 5) + " ms") << std::setw(14)
                  << std::fixed << std::setprecision(2)
                  << (std::to_string(r.p95FrameTimeMs).substr(0, 5) + " ms") << std::setw(14)
                  << std::fixed << std::setprecision(2)
                  << (r.avgTotalGiMs > 0.0 ? (std::to_string(r.avgTotalGiMs).substr(0, 4) + " ms")
                                           : "-")
                  << "\n";
    }
    std::cout << "========================================================================\n";
    return 0;
}
