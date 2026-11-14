/// @file benchmark_lighting_conditions.cpp
/// @brief Multi-scene, multi-condition lighting benchmark certification tool (Milestone 11C).
///
/// Evaluates Hikari Engine's lighting fidelity across:
/// 1. Beautiful Natural Daylight
/// 2. Majestic Cinematic Sunset
/// 3. Deep Atmospheric Night
///
/// Across Khronos Sponza Atrium, Damaged Helmet, and Flight Helmet at native 1080p ($1920 \times
/// 1080$). Measures GPU pass timings, CPU overhead, and saves visual readback captures.

#include "core/log.h"
#include "framework/scene_serializer.h"
#include "render/asset_manager.h"
#include "render/camera_component.h"
#include "render/environment_map.h"
#include "render/graphics_settings.h"
#include "render/light_components.h"
#include "render/mesh_component.h"
#include "render/post_process_types.h"
#include "render/scene_renderer.h"
#include "rhi/command_list.h"
#include "rhi/rhi_device.h"
#include "world/hierarchy_systems.h"
#include "world/world.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
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
    std::cout << "[LIGHTING-SAVED] " << path << " (" << width << "x" << height << ")\n";
}

struct LightingBenchmarkRecord {
    std::string scene;
    std::string condition;
    float cpuTimeMs;
    float gpuTotalMs;
    float gpuShadowMs;
    float gpuMainMs;
    float gpuPostMs;
    uint32_t totalDraws;
    uint32_t totalTriangles;
};

int main(int argc, char** argv) {
    std::string outDir = "test-assets/lighting_conditions";
    if (argc > 1) {
        outDir = argv[1];
    }
    std::filesystem::create_directories(outDir);

    std::cout << "======================================================================\n";
    std::cout << "  Hikari Engine | Milestone 11C: Premium Lighting Quality Benchmark    \n";
    std::cout << "  Daylight · Majestic Sunset · Atmospheric Night (Native 1080p)       \n";
    std::cout << "======================================================================\n\n";

    rhi::VulkanDeviceDesc devDesc{
        .windowHandle = nullptr,
        .enableValidation = false,
    };
    auto devRes = rhi::createVulkanDevice(devDesc);
    if (!devRes.hasValue()) {
        std::cerr << "CRITICAL: Vulkan device creation failed: " << devRes.error().message << '\n';
        return 1;
    }
    auto device = std::move(devRes.value());

    constexpr uint32_t kWidth = 1920;
    constexpr uint32_t kHeight = 1080;

    render::SceneRenderer renderer{};
    auto initRes = renderer.init(*device, kWidth, kHeight);
    if (!initRes.hasValue()) {
        std::cerr << "CRITICAL: SceneRenderer initialization failed: " << initRes.error().message
                  << '\n';
        return 1;
    }

    // High quality preset with IBL enabled
    render::GraphicsSettings baseSettings =
        render::GraphicsSettings::createPreset(render::GraphicsQualityPreset::High);
    baseSettings.lighting.iblEnabled = true;
    baseSettings.shadows.resolution = 2048;
    baseSettings.shadows.pcfQuality = render::ShadowPcfQuality::High;
    baseSettings.postProcess.enabled = true;
    (void)renderer.applyGraphicsSettings(baseSettings);

    const uint64_t byteSize = kWidth * kHeight * 4;
    rhi::BufferDesc rbDesc{
        .sizeInBytes = byteSize,
        .usage = rhi::BufferUsageFlags::TransferDst,
        .memoryUsage = rhi::MemoryUsage::GpuToCpu,
        .debugName = "LightingReadbackBuffer",
    };
    auto rbRes = device->createBuffer(rbDesc);
    if (!rbRes.hasValue()) {
        std::cerr << "Failed to create readback buffer\n";
        return 1;
    }
    auto readbackBuffer = rbRes.value();

    struct SceneConfig {
        std::string name;
        std::string fileTag;
        std::string path;
        Vec3 camPos;
        Vec3 camTarget;
        float fov;
        float farPlane{100.0F};
    };

    std::vector<SceneConfig> scenes = {
        {"Sponza Atrium", "Sponza", "assets/benchmark-scenes/Sponza_Benchmark.hikari",
         Vec3(-8.0F, 2.5F, 0.0F), Vec3(0.0F, 2.5F, 0.0F), 65.0F, 200.0F},
        {"Damaged Helmet", "DamagedHelmet",
         "assets/benchmark-scenes/DamagedHelmet_Benchmark.hikari", Vec3(0.0F, 0.0F, 2.4F),
         Vec3(0.0F, 0.0F, 0.0F), 45.0F, 50.0F},
        {"Flight Helmet", "FlightHelmet", "assets/benchmark-scenes/FlightHelmet_Benchmark.hikari",
         Vec3(0.0F, 0.3F, 1.2F), Vec3(0.0F, 0.2F, 0.0F), 45.0F, 50.0F},
        {"Amazon Bistro", "Bistro", "assets/benchmark-scenes/Bistro_Benchmark.hikari",
         Vec3(-26.43F, 3.16F, 11.17F), Vec3(-16.85F, 2.84F, 14.03F), 65.0F, 500.0F},
    };

    struct ConditionConfig {
        std::string name;
        render::AtmosphereParameters params;
    };

    std::vector<ConditionConfig> conditions = {
        {"Daylight", render::AtmosphereParameters::createDaylight()},
        {"Sunset", render::AtmosphereParameters::createSunset()},
        {"Night", render::AtmosphereParameters::createNight()},
    };

    std::vector<LightingBenchmarkRecord> records;

    for (const auto& scene : scenes) {
        if (!std::filesystem::exists(scene.path)) {
            std::cerr << "[SKIP] Scene file not found: " << scene.path << '\n';
            continue;
        }

        std::cout << "\n>>> Benchmarking Scene: " << scene.name << " (" << scene.path << ")\n";

        world::World world;
        render::AssetManager assetManager;

        auto loadRes = framework::SceneSerializer::deserializeFromFile(world, scene.path);
        if (!loadRes.hasValue()) {
            std::cerr << "Failed loading scene: " << loadRes.error().message << '\n';
            continue;
        }
        world::updateHierarchyTransforms(world);

        Mat4 view = Mat4::lookAt(scene.camPos, scene.camTarget, Vec3::unitY());
        Mat4 proj = Mat4::perspective(toRadians(scene.fov),
                                      static_cast<float>(kWidth) / static_cast<float>(kHeight),
                                      0.1F, scene.farPlane);
        Mat4 vp = proj * view;

        for (const auto& cond : conditions) {
            std::cout << "  * Evaluating Condition: " << std::setw(10) << cond.name << " ... "
                      << std::flush;

            // Apply atmospheric condition to renderer and synchronize world directional light
            (void)renderer.applyAtmosphere(world, cond.params);

            // If Night in Sponza, add two subtle warm lantern point lights to accentuate contrast
            if (cond.name == "Night" && scene.fileTag == "Sponza") {
                auto l1 = world.getRawWorld().entity("Lantern_1");
                l1.set(render::PointLightComponent{
                    .position = Vec3(-2.0F, 1.8F, -1.5F),
                    .color = Vec3(1.0F, 0.65F, 0.25F), // Warm amber flame
                    .intensity = 18.0F,
                    .range = 8.0F,
                    .enabled = true,
                });
                auto l2 = world.getRawWorld().entity("Lantern_2");
                l2.set(render::PointLightComponent{
                    .position = Vec3(2.0F, 1.8F, 1.5F),
                    .color = Vec3(1.0F, 0.65F, 0.25F),
                    .intensity = 18.0F,
                    .range = 8.0F,
                    .enabled = true,
                });
            }

            // Warmup frames (5 frames)
            for (int f = 0; f < 5; ++f) {
                auto cmdRes = device->createCommandList();
                auto cmd = std::move(cmdRes.value());
                cmd->begin();
                (void)renderer.renderScene(world, assetManager, *device, *cmd, vp);
                renderer.renderPostProcess(*cmd);
                cmd->end();
                (void)device->submit(cmd.get());
                device->waitIdle();
                renderer.updateGpuTimings(*device);
            }

            // Measurement frames (30 steady-state frames)
            constexpr int kMeasureFrames = 30;
            double totalCpuMs = 0.0;
            render::RenderStats lastStats{};

            for (int f = 0; f < kMeasureFrames; ++f) {
                auto cmdRes = device->createCommandList();
                auto cmd = std::move(cmdRes.value());
                cmd->begin();

                auto tStart = std::chrono::high_resolution_clock::now();
                lastStats = renderer.renderScene(world, assetManager, *device, *cmd, vp);
                renderer.renderPostProcess(*cmd);
                auto tEnd = std::chrono::high_resolution_clock::now();
                totalCpuMs += std::chrono::duration<double, std::milli>(tEnd - tStart).count();

                cmd->end();
                (void)device->submit(cmd.get());
                device->waitIdle();
                renderer.updateGpuTimings(*device);
            }

            const auto& gpuTimings = renderer.getGpuPassTimings();
            double avgCpuMs = totalCpuMs / kMeasureFrames;
            float postMs = gpuTimings.bloomDownsampleMs + gpuTimings.bloomUpsampleMs +
                           gpuTimings.toneMappingMs;

            uint32_t totalDraws = lastStats.drawCalls + lastStats.shadowDrawCalls;
            uint32_t totalTriangles = lastStats.triangles + lastStats.shadowTriangles;

            std::cout << "Done! (GPU: " << std::fixed << std::setprecision(2)
                      << gpuTimings.totalSceneMs << " ms, CPU: " << avgCpuMs
                      << " ms, Draws: " << totalDraws << ", Tris: " << totalTriangles << ")\n";

            LightingBenchmarkRecord rec{
                .scene = scene.name,
                .condition = cond.name,
                .cpuTimeMs = static_cast<float>(avgCpuMs),
                .gpuTotalMs = gpuTimings.totalSceneMs,
                .gpuShadowMs = gpuTimings.shadowPassMs,
                .gpuMainMs = gpuTimings.mainPbrPassMs,
                .gpuPostMs = postMs,
                .totalDraws = totalDraws,
                .totalTriangles = totalTriangles,
            };
            records.push_back(rec);

            // Readback final frame
            {
                auto readCmdRes = device->createCommandList();
                auto readCmd = std::move(readCmdRes.value());
                readCmd->begin();

                (void)renderer.renderScene(world, assetManager, *device, *readCmd, vp);
                renderer.renderPostProcess(*readCmd);
                readCmd->copyTextureToBuffer(renderer.getColorTarget(), readbackBuffer);

                readCmd->end();
                (void)device->submit(readCmd.get());
                device->waitIdle();

                std::vector<uint8_t> pxBuffer(byteSize, 0);
                auto copyRes = device->readBuffer(readbackBuffer, 0, pxBuffer.data(), byteSize);
                if (copyRes.hasValue()) {
                    std::string outPpm = outDir + "/" + scene.fileTag + "_" + cond.name + ".ppm";
                    savePpm(outPpm, pxBuffer.data(), kWidth, kHeight);
                }
            }
        }
    }

    std::cout << "\n==============================================================================="
                 "=========\n";
    std::cout << "              PREMIUM LIGHTING QUALITY CERTIFICATION REPORT (1080P)              "
                 "      \n";
    std::cout << "================================================================================="
                 "=======\n\n";

    std::cout << "| Scene | Condition | GPU Total (ms) | Shadow (ms) | PBR (ms) | Post (ms) | CPU "
                 "(ms) | Draw Calls | Triangles |\n";
    std::cout << "| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |\n";

    for (const auto& r : records) {
        std::cout << "| " << r.scene << " | **" << r.condition << "** | " << std::fixed
                  << std::setprecision(2) << r.gpuTotalMs << " | " << r.gpuShadowMs << " | "
                  << r.gpuMainMs << " | " << r.gpuPostMs << " | " << r.cpuTimeMs << " | "
                  << r.totalDraws << " | " << r.totalTriangles << " |\n";
    }

    std::cout << "\n==============================================================================="
                 "=========\n";
    std::cout << "Lighting conditions certification complete!\n";
    return 0;
}
