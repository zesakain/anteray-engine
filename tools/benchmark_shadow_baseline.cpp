/// @file benchmark_shadow_baseline.cpp
/// @brief Shadow Pipeline Profiling & Baseline Measurement Tool for Milestone 11E.

#include "core/log.h"
#include "framework/scene_serializer.h"
#include "render/asset_manager.h"
#include "render/camera_component.h"
#include "render/graphics_settings.h"
#include "render/post_process_types.h"
#include "render/scene_renderer.h"
#include "rhi/command_list.h"
#include "rhi/rhi_device.h"
#include "world/hierarchy_systems.h"
#include "world/world.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>

using namespace eng;

int main() {
    std::cout << "========================================================================\n";
    std::cout << "  ANTERAY ENGINE | Milestone 11E Shadow Architecture Baseline Audit    \n";
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

    auto runAudit = [&](const std::string& sceneName, const std::string& scenePath,
                       const Mat4& viewProj) {
        world::World world;
        render::AssetManager assetMgr;

        auto desRes = framework::SceneSerializer::deserializeFromFile(world, scenePath);
        if (!desRes.hasValue()) {
            std::cerr << "Failed to load scene: " << scenePath << "\n";
            return;
        }

        render::GraphicsSettings settings = renderer.getGraphicsSettings();
        settings.shadows.enabled = true;
        settings.shadows.resolution = 2048;
        settings.shadows.pcfQuality = render::ShadowPcfQuality::Medium;
        settings.shadows.distance = 50.0F;
        std::ignore = renderer.applyGraphicsSettings(settings);

        if (desRes.value().postProcessSettings.has_value()) {
            renderer.setPostProcessSettings(*desRes.value().postProcessSettings);
        }

        // Warm up 15 frames
        for (int i = 0; i < 15; ++i) {
            auto cmdRes = device->createCommandList();
            if (!cmdRes.hasValue()) break;
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

        // Measure 30 frames of dirty/rendering shadow pass, and 30 frames of cached shadow pass
        std::vector<double> dirtyGpuTimes;
        std::vector<double> dirtyCpuTimes;
        render::RenderStats dirtyStats{};

        // Force shadowDirty every frame for dirty measurement
        for (int i = 0; i < 20; ++i) {
            renderer.invalidateShadowMap(); // invalidate to measure active shadow rendering
            auto startCpu = std::chrono::high_resolution_clock::now();
            auto cmdRes = device->createCommandList();
            if (!cmdRes.hasValue()) break;
            auto cmd = std::move(cmdRes.value());
            cmd->begin();
            renderer.renderScene(world, assetMgr, *device, *cmd, viewProj);
            if (renderer.isPostProcessingEnabled()) {
                renderer.renderPostProcess(*cmd);
            }
            cmd->end();
            auto endCpu = std::chrono::high_resolution_clock::now();
            double cpuMs = std::chrono::duration<double, std::milli>(endCpu - startCpu).count();
            dirtyCpuTimes.push_back(cpuMs);

            auto startTotal = std::chrono::high_resolution_clock::now();
            std::ignore = device->submit(cmd.get());
            device->waitIdle();
            auto endTotal = std::chrono::high_resolution_clock::now();
            double totalMs = std::chrono::duration<double, std::milli>(endTotal - startTotal).count();
            (void)totalMs;

            renderer.updateGpuTimings(*device);
            const auto& timings = renderer.getGpuPassTimings();
            if (timings.valid) {
                dirtyGpuTimes.push_back(timings.shadowPassMs);
            }
            dirtyStats = renderer.getLastRenderStats();
        }

        // Measure 30 frames of cached shadow pass
        std::vector<double> cachedGpuTimes;
        std::vector<double> cachedCpuTimes;
        std::vector<double> cachedFrameTimes;
        render::RenderStats cachedStats{};

        for (int i = 0; i < 30; ++i) {
            auto startCpu = std::chrono::high_resolution_clock::now();
            auto cmdRes = device->createCommandList();
            if (!cmdRes.hasValue()) break;
            auto cmd = std::move(cmdRes.value());
            cmd->begin();
            renderer.renderScene(world, assetMgr, *device, *cmd, viewProj);
            if (renderer.isPostProcessingEnabled()) {
                renderer.renderPostProcess(*cmd);
            }
            cmd->end();
            auto endCpu = std::chrono::high_resolution_clock::now();
            double cpuMs = std::chrono::duration<double, std::milli>(endCpu - startCpu).count();
            cachedCpuTimes.push_back(cpuMs);

            auto startTotal = std::chrono::high_resolution_clock::now();
            std::ignore = device->submit(cmd.get());
            device->waitIdle();
            auto endTotal = std::chrono::high_resolution_clock::now();
            double totalMs = std::chrono::duration<double, std::milli>(endTotal - startTotal).count();
            cachedFrameTimes.push_back(totalMs);

            renderer.updateGpuTimings(*device);
            const auto& timings = renderer.getGpuPassTimings();
            if (timings.valid) {
                cachedGpuTimes.push_back(timings.shadowPassMs);
            }
            cachedStats = renderer.getLastRenderStats();
        }

        std::sort(cachedFrameTimes.begin(), cachedFrameTimes.end());
        double avgCachedFrame = std::accumulate(cachedFrameTimes.begin(), cachedFrameTimes.end(), 0.0) /
                                static_cast<double>(cachedFrameTimes.size());
        double avgDirtyGpu = dirtyGpuTimes.empty() ? 0.0 :
            (std::accumulate(dirtyGpuTimes.begin(), dirtyGpuTimes.end(), 0.0) /
             static_cast<double>(dirtyGpuTimes.size()));
        double avgDirtyCpu = std::accumulate(dirtyCpuTimes.begin(), dirtyCpuTimes.end(), 0.0) /
                             static_cast<double>(dirtyCpuTimes.size());
        double avgCachedCpu = std::accumulate(cachedCpuTimes.begin(), cachedCpuTimes.end(), 0.0) /
                              static_cast<double>(cachedCpuTimes.size());

        std::cout << ">>> SCENE: " << sceneName << " <<<\n";
        std::cout << "  Resolution:                 " << width << "x" << height << "\n";
        std::cout << "  Shadow Map:                 " << settings.shadows.resolution << "x" << settings.shadows.resolution << " (D32_FLOAT, 16.0 MB)\n";
        std::cout << "  Shadow Coverage:            Single Orthographic Box (" << settings.shadows.distance << "m range, orthoSize 20.0m)\n";
        std::cout << "  Filtering:                  8-tap Vogel Disc PCF (Medium)\n";
        std::cout << "  -- ACTIVE RENDERING (DIRTY CACHE) --\n";
        std::cout << "  Shadow Draw Calls:          " << dirtyStats.shadowDrawCalls << "\n";
        std::cout << "  Shadow Triangles:           " << dirtyStats.shadowTriangles << "\n";
        std::cout << "  Meshes Evaluated:           " << dirtyStats.meshesRendered << "\n";
        std::cout << "  Culled Primitives:          " << dirtyStats.culledPrimitives << "\n";
        std::cout << "  CPU Recording Time (Dirty): " << std::fixed << std::setprecision(2) << avgDirtyCpu << " ms\n";
        std::cout << "  GPU Shadow Pass Time:       " << avgDirtyGpu << " ms\n";
        std::cout << "  -- CACHED RENDERING (WARM CACHE) --\n";
        std::cout << "  Shadow Draw Calls:          " << cachedStats.shadowDrawCalls << " (bypassed)\n";
        std::cout << "  CPU Recording Time (Warm):  " << avgCachedCpu << " ms\n";
        std::cout << "  Total Frame Time (Warm):    " << avgCachedFrame << " ms (" << (1000.0 / avgCachedFrame) << " FPS)\n\n";
    };

    // 1. Bistro Benchmark Hero Corner Viewpoint
    const Mat4 bistroViewProj =
        Mat4::perspective(52.0F * 3.14159265F / 180.0F, 1920.0F / 1080.0F, 0.1F, 250.0F) *
        Mat4::lookAt(Vec3(-22.8F, 2.1F, 12.4F), Vec3(-14.2F, 2.2F, 14.5F), Vec3::unitY());
    runAudit("Amazon Lumberyard Bistro", "assets/benchmark-scenes/Bistro_Benchmark.hikari", bistroViewProj);

    // 2. Sponza Benchmark Viewpoint
    const Mat4 sponzaViewProj =
        Mat4::perspective(60.0F * 3.14159265F / 180.0F, 1920.0F / 1080.0F, 0.1F, 100.0F) *
        Mat4::lookAt(Vec3(-8.0F, 3.5F, 0.0F), Vec3(0.0F, 2.5F, 0.0F), Vec3::unitY());
    runAudit("Crytek Sponza", "assets/benchmark-scenes/Sponza_Benchmark.hikari", sponzaViewProj);

    return 0;
}
