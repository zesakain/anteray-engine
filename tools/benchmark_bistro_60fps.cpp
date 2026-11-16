/// @file benchmark_bistro_60fps.cpp
/// @brief Reproducible 1,000-frame Bistro Benchmark across Edit Mode and Play Mode (Idle,
/// Translation, Rotation, Combined) with full CPU/GPU per-pass profiling.

#include "core/job_system.h"
#include "core/log.h"
#include "core/time.h"
#include "editor/editor_engine.h"
#include "editor/workspace_manager.h"
#include "framework/async_scene_loader.h"
#include "framework/runtime_session.h"
#include "physics/physics_components.h"
#include "render/graphics_settings.h"
#include "render/scene_renderer.h"
#include "rhi/command_list.h"
#include "rhi/rhi_device.h"
#include "world/world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace eng;

namespace {

struct StatSummary {
    double avg{0.0};
    double p50{0.0};
    double p95{0.0};
    double p99{0.0};
    double max{0.0};
    double onePercentLowFps{0.0};

    static StatSummary compute(std::vector<double> samples) {
        StatSummary s{};
        if (samples.empty()) {
            return s;
        }
        std::sort(samples.begin(), samples.end());
        const size_t n = samples.size();
        s.avg = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(n);
        s.p50 = samples[n / 2];
        s.p95 = samples[std::min(n - 1, static_cast<size_t>(static_cast<double>(n) * 0.95))];
        s.p99 = samples[std::min(n - 1, static_cast<size_t>(static_cast<double>(n) * 0.99))];
        s.max = samples.back();
        // 1% low FPS = 1000 / average of slowest 1% of frames
        const size_t slowCount = std::max<size_t>(1U, n / 100U);
        double slowSum = 0.0;
        for (size_t i = n - slowCount; i < n; ++i) {
            slowSum += samples[i];
        }
        const double slowAvgMs = slowSum / static_cast<double>(slowCount);
        s.onePercentLowFps = slowAvgMs > 0.0 ? (1000.0 / slowAvgMs) : 0.0;
        return s;
    }
};

enum class CameraMode : uint8_t { Idle = 0, Translate = 1, Rotate = 2, Combined = 3 };

struct ScenarioMetrics {
    std::string name;
    uint32_t frames{0};
    std::vector<double> frameMs;
    std::vector<double> beginFrameMs;
    std::vector<double> drawPanelsMs;
    std::vector<double> runtimeUpdateMs;
    std::vector<double> renderSceneCpuMs;
    std::vector<double> endFrameMs;
    std::vector<double> submitWaitMs;

    std::vector<double> gpuTotalMs;
    std::vector<double> gpuShadowMs;
    std::vector<double> gpuDepthPrepassMs;
    std::vector<double> gpuMainPbrMs;
    std::vector<double> gpuVelocityMs;
    std::vector<double> gpuTaaMs;
    std::vector<double> gpuGtaoMs;
    std::vector<double> gpuSsgiMs;
    std::vector<double> gpuSsrMs;
    std::vector<double> gpuBloomMs;
    std::vector<double> gpuDofMs;
    std::vector<double> gpuMotionBlurMs;
    std::vector<double> gpuToneMapMs;

    uint64_t totalMainDraws{0};
    uint64_t totalShadowDraws{0};
    uint64_t totalMainTris{0};
    uint64_t totalShadowTris{0};
    uint64_t totalCulled{0};

    void print() const {
        const auto f = StatSummary::compute(frameMs);
        const double avgFps = f.avg > 0.0 ? (1000.0 / f.avg) : 0.0;
        auto avgOf = [](const std::vector<double>& v) {
            return v.empty()
                       ? 0.0
                       : std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
        };
        const auto g = StatSummary::compute(gpuTotalMs);

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "\n--- " << name << " (" << frames << " frames) ---\n";
        std::cout << "  FPS: avg=" << avgFps << " | 1%low=" << f.onePercentLowFps
                  << " | Frame (ms): avg=" << f.avg << " p50=" << f.p50 << " p95=" << f.p95
                  << " p99=" << f.p99 << " max=" << f.max << "\n";
        std::cout << "  Geometry (avg/frame): MainDraws=" << (totalMainDraws / frames)
                  << " ShadowDraws=" << (totalShadowDraws / frames)
                  << " MainTris=" << (totalMainTris / frames)
                  << " ShadowTris=" << (totalShadowTris / frames)
                  << " Culled=" << (totalCulled / frames) << "\n";
        std::cout << "  CPU (avg ms): beginFrame=" << avgOf(beginFrameMs)
                  << " drawPanels=" << avgOf(drawPanelsMs)
                  << " runtimeUpdate=" << avgOf(runtimeUpdateMs)
                  << " renderScene=" << avgOf(renderSceneCpuMs) << " endFrame=" << avgOf(endFrameMs)
                  << " submit+wait=" << avgOf(submitWaitMs) << "\n";
        if (!gpuTotalMs.empty()) {
            std::cout << "  GPU (avg ms): total=" << g.avg << " (p95=" << g.p95 << " max=" << g.max
                      << ") | shadow=" << avgOf(gpuShadowMs)
                      << " depthPre=" << avgOf(gpuDepthPrepassMs)
                      << " mainPBR=" << avgOf(gpuMainPbrMs) << " vel=" << avgOf(gpuVelocityMs)
                      << " TAA=" << avgOf(gpuTaaMs) << " GTAO=" << avgOf(gpuGtaoMs)
                      << " SSGI=" << avgOf(gpuSsgiMs) << " SSR=" << avgOf(gpuSsrMs)
                      << " Bloom=" << avgOf(gpuBloomMs) << " DoF=" << avgOf(gpuDofMs)
                      << " MotBlur=" << avgOf(gpuMotionBlurMs) << " ToneMap=" << avgOf(gpuToneMapMs)
                      << "\n";
        }
    }
};

} // namespace

int main(int argc, char* argv[]) {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t warmUpFrames = 60;
    uint32_t measureFrames = 1000;
    std::string presetOverride{};

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--width" && i + 1 < argc) {
            width = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--height" && i + 1 < argc) {
            height = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--frames" && i + 1 < argc) {
            measureFrames = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--warmup" && i + 1 < argc) {
            warmUpFrames = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--preset" && i + 1 < argc) {
            presetOverride = argv[++i];
        }
    }

    rhi::VulkanDeviceDesc vkDesc{.windowHandle = nullptr, .enableValidation = false};
    auto devRes = rhi::createVulkanDevice(vkDesc);
    if (!devRes.hasValue()) {
        std::cerr << "Failed to create Vulkan device: " << devRes.error().message << "\n";
        return 1;
    }
    auto device = std::move(devRes.value());
    const auto& caps = device->getCaps();
    std::cout << "=====================================================================\n";
    std::cout << " Hikari Bistro 60+ FPS Benchmark (" << width << "x" << height << ", "
              << measureFrames << " frames/scenario)\n";
    std::cout << " GPU: " << caps.deviceName
              << " | TimestampQueries: " << (caps.features.timestampQueries ? "YES" : "NO") << "\n";
    std::cout << "=====================================================================\n";

    rhi::TextureDesc rtDesc{
        .type = rhi::TextureType::Texture2D,
        .format = rhi::TextureFormat::BGRA8_UNORM,
        .width = width,
        .height = height,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = rhi::TextureUsageFlags::ColorAttachment | rhi::TextureUsageFlags::TransferSrc,
        .debugName = "BistroBenchColorTarget",
    };
    auto rtRes = device->createTexture(rtDesc);
    if (!rtRes.hasValue()) {
        std::cerr << "Failed to create color target\n";
        return 1;
    }
    auto rtHandle = rtRes.value();

    editor::EditorEngine editor;
    if (!editor.initialize(*device, {.showDemoWindow = false})) {
        std::cerr << "Failed to initialize EditorEngine\n";
        return 1;
    }

    world::World editWorld;
    physics::registerPhysicsComponents(editWorld);
    editor.bindWorld(&editWorld);

    auto& runtimeSession = editor.getRuntimeSession();
    runtimeSession.setEditWorld(&editWorld);

    // Load Bistro via AsyncSceneLoader so all 338 textures & GPU assets are resident.
    const std::string bistroPath = "assets/benchmark-scenes/Bistro_Benchmark.hikari";
    if (!std::filesystem::exists(bistroPath)) {
        std::cerr << "Missing scene: " << bistroPath << "\n";
        return 1;
    }

    JobSystemConfig jobConfig{.workerCount = 4, .maxJobs = 256};
    std::vector<std::byte> jobStorage(JobSystem::requiredBytes(jobConfig));
    JobSystem jobSystem(jobStorage, jobConfig);

    framework::AsyncSceneLoader asyncLoader;
    asyncLoader.bindJobSystem(&jobSystem);
    asyncLoader.bindAssetManager(&editor.getAssetManager());
    auto loadStartRes = asyncLoader.startLoad(bistroPath);
    if (!loadStartRes.hasValue()) {
        std::cerr << "Failed to begin Bistro load: " << loadStartRes.error().message << "\n";
        return 1;
    }
    while (asyncLoader.isLoading() && !asyncLoader.isReadyToActivate()) {
        asyncLoader.update(*device, 8.0F);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!asyncLoader.isReadyToActivate()) {
        std::cerr << "Bistro async load failed: " << asyncLoader.getErrorMessage() << "\n";
        return 1;
    }
    (void)asyncLoader.activate(editWorld);
    std::cout << "Bistro loaded: entities=" << editWorld.getAllEntities().size()
              << " | PeakVRAM=" << (asyncLoader.getProgress().peakVramBytes / (1024ULL * 1024ULL))
              << " MiB\n";

    if (!presetOverride.empty()) {
        render::GraphicsQualityPreset p = render::GraphicsQualityPreset::High;
        if (presetOverride == "low") {
            p = render::GraphicsQualityPreset::Low;
        } else if (presetOverride == "medium") {
            p = render::GraphicsQualityPreset::Medium;
        } else if (presetOverride == "high") {
            p = render::GraphicsQualityPreset::High;
        } else if (presetOverride == "ultra") {
            p = render::GraphicsQualityPreset::Ultra;
        }
        auto gs = render::GraphicsSettings::createPreset(p);
        (void)editor.getSceneRenderer().applyGraphicsSettings(gs);
    }

    const auto& activeGs = editor.getSceneRenderer().getGraphicsSettings();
    std::cout << "GraphicsSettings: shadowRes=" << activeGs.shadows.resolution
              << " cascades=" << activeGs.shadows.cascadeCount
              << " pcf=" << static_cast<int>(activeGs.shadows.pcfQuality)
              << " shadowCaching=" << (activeGs.shadows.shadowCaching ? "ON" : "OFF")
              << " AA=" << static_cast<int>(activeGs.quality.antiAliasing)
              << " GTAO=" << (activeGs.lighting.gtaoEnabled ? "ON" : "OFF")
              << " SSR=" << (activeGs.lighting.ssrEnabled ? "ON" : "OFF")
              << " SSGI=" << (activeGs.lighting.ssgiEnabled ? "ON" : "OFF")
              << " Bloom=" << (activeGs.postProcess.bloomEnabled ? "ON" : "OFF") << "\n";

    (void)editor.resizeViewport(width, height);

    auto runScenario = [&](const std::string& name, CameraMode camMode, bool playMode) {
        ScenarioMetrics m{};
        m.name = name;

        if (playMode) {
            auto startRes = runtimeSession.play();
            if (!startRes.hasValue()) {
                std::cerr << "Failed to start RuntimeSession: " << startRes.error().message << "\n";
            }
        }

        const Vec3 baseEye(-26.7302F, 3.30268F, 8.89158F);
        const Vec3 baseTarget(-17.9055F, 2.60661F, 8.75494F);
        const Vec3 baseForward = (baseTarget - baseEye).normalized();
        const Duration fixedStep = Duration::fromSeconds(1.0 / 60.0);
        Timestamp prevTime = Timestamp::now();
        Duration lastCpuDur = Duration::fromMilliseconds(4);

        const uint32_t totalFrames = warmUpFrames + measureFrames;
        for (uint32_t i = 0; i < totalFrames; ++i) {
            const Timestamp tFrameStart = Timestamp::now();
            const Duration frameDur = tFrameStart - prevTime;
            prevTime = tFrameStart;

            Vec3 eye = baseEye;
            Vec3 target = baseTarget;
            const float t = static_cast<float>(i) * 0.03F;
            if (camMode == CameraMode::Translate) {
                const Vec3 offset(std::sin(t) * 2.5F, std::cos(t * 0.5F) * 0.4F,
                                  std::cos(t) * 1.8F);
                eye = baseEye + offset;
                target = baseTarget + offset;
            } else if (camMode == CameraMode::Rotate) {
                const float yaw = std::sin(t) * 0.45F;
                const float c = std::cos(yaw);
                const float s = std::sin(yaw);
                const Vec3 dir(baseForward.x * c - baseForward.z * s, baseForward.y,
                               baseForward.x * s + baseForward.z * c);
                target = eye + dir * 8.85F;
            } else if (camMode == CameraMode::Combined) {
                const Vec3 offset(std::sin(t) * 2.5F, std::cos(t * 0.5F) * 0.4F,
                                  std::cos(t) * 1.8F);
                eye = baseEye + offset;
                const float yaw = std::sin(t * 0.8F) * 0.45F;
                const float c = std::cos(yaw);
                const float s = std::sin(yaw);
                const Vec3 dir(baseForward.x * c - baseForward.z * s, baseForward.y,
                               baseForward.x * s + baseForward.z * c);
                target = eye + dir * 8.85F;
            }

            const Mat4 view = Mat4::lookAt(eye, target, Vec3(0.0F, 1.0F, 0.0F));
            const Mat4 proj = Mat4::perspective(
                1.04719755F, static_cast<float>(width) / static_cast<float>(height), 0.1F, 250.0F);
            const Mat4 vp = proj * view;

            const Timestamp tBfStart = Timestamp::now();
            editor.recordFrameTelemetry(frameDur, lastCpuDur);
            editor.beginFrame(frameDur, width, height);
            const Timestamp tBfEnd = Timestamp::now();

            const Timestamp tDpStart = Timestamp::now();
            editor.drawPanels();
            const Timestamp tDpEnd = Timestamp::now();

            const Timestamp tRuStart = Timestamp::now();
            if (playMode) {
                runtimeSession.update(fixedStep);
            }
            const Timestamp tRuEnd = Timestamp::now();

            auto cmdRes = device->createCommandList();
            auto cmd = std::move(cmdRes.value());

            const Timestamp tRsStart = Timestamp::now();
            cmd->begin();
            auto* activeWorld = playMode && runtimeSession.getActiveWorld() != nullptr
                                    ? runtimeSession.getActiveWorld()
                                    : &editWorld;
            const auto stats = editor.getSceneRenderer().renderScene(
                *activeWorld, editor.getAssetManager(), *device, *cmd, vp);
            if (editor.getSceneRenderer().isPostProcessingEnabled()) {
                editor.getSceneRenderer().renderPostProcess(*cmd);
            }
            editor.getSceneRenderer().updateGpuTimings(*device);
            const Timestamp tRsEnd = Timestamp::now();

            const Timestamp tEfStart = Timestamp::now();
            rhi::RenderPassBeginInfo passInfo{
                .colorTarget = rtHandle,
                .clearColor = {0.08F, 0.12F, 0.20F, 1.0F},
                .clearTarget = true,
            };
            cmd->beginRenderPass(passInfo);
            editor.endFrame(*cmd);
            cmd->endRenderPass();
            cmd->end();
            const Timestamp tEfEnd = Timestamp::now();

            const Timestamp tSubStart = Timestamp::now();
            (void)device->submit(cmd.get());
            device->waitIdle();
            const Timestamp tSubEnd = Timestamp::now();

            const Timestamp tFrameEnd = Timestamp::now();
            lastCpuDur = (tEfEnd - tBfStart);

            if (i >= warmUpFrames) {
                ++m.frames;
                const auto toMs = [](Duration d) {
                    return static_cast<double>(d.asMicroseconds()) * 0.001;
                };
                m.frameMs.push_back(toMs(tFrameEnd - tFrameStart));
                m.beginFrameMs.push_back(toMs(tBfEnd - tBfStart));
                m.drawPanelsMs.push_back(toMs(tDpEnd - tDpStart));
                m.runtimeUpdateMs.push_back(toMs(tRuEnd - tRuStart));
                m.renderSceneCpuMs.push_back(toMs(tRsEnd - tRsStart));
                m.endFrameMs.push_back(toMs(tEfEnd - tEfStart));
                m.submitWaitMs.push_back(toMs(tSubEnd - tSubStart));

                m.totalMainDraws += stats.drawCalls;
                m.totalShadowDraws += stats.shadowDrawCalls;
                m.totalMainTris += stats.triangles;
                m.totalShadowTris += stats.shadowTriangles;
                m.totalCulled += stats.culledPrimitives;

                const auto& gpu = editor.getSceneRenderer().getGpuPassTimings();
                if (gpu.valid) {
                    m.gpuTotalMs.push_back(gpu.totalSceneMs);
                    m.gpuShadowMs.push_back(gpu.shadowPassMs);
                    m.gpuDepthPrepassMs.push_back(gpu.depthPrepassMs);
                    m.gpuMainPbrMs.push_back(gpu.mainPbrPassMs);
                    m.gpuVelocityMs.push_back(gpu.velocityPassMs);
                    m.gpuTaaMs.push_back(gpu.taaPassMs);
                    m.gpuGtaoMs.push_back(gpu.gtaoPassMs);
                    m.gpuSsgiMs.push_back(gpu.ssgiPassMs);
                    m.gpuSsrMs.push_back(gpu.ssrPassMs);
                    m.gpuBloomMs.push_back(gpu.bloomDownsampleMs + gpu.bloomUpsampleMs);
                    m.gpuDofMs.push_back(gpu.dofPassMs);
                    m.gpuMotionBlurMs.push_back(gpu.motionBlurPassMs);
                    m.gpuToneMapMs.push_back(gpu.toneMappingMs);
                }
            }
        }

        if (playMode) {
            (void)runtimeSession.stop();
        }
        m.print();
    };

    runScenario("Edit Idle", CameraMode::Idle, false);
    runScenario("Edit Translation", CameraMode::Translate, false);
    runScenario("Edit Rotation", CameraMode::Rotate, false);
    runScenario("Edit Combined", CameraMode::Combined, false);
    runScenario("Play Idle", CameraMode::Idle, true);
    runScenario("Play Translation", CameraMode::Translate, true);
    runScenario("Play Rotation", CameraMode::Rotate, true);
    runScenario("Play Combined", CameraMode::Combined, true);

    device->destroyTexture(rtHandle);
    editor.shutdown();
    return 0;
}
