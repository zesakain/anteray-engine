/// @file benchmark_recovery_r2.cpp
/// @brief Comprehensive CPU/GPU profiling benchmark for Hikari Performance Recovery R2.
///
/// Measures Editor Idle, Editor Camera Translation, Editor Camera Rotation,
/// Play Mode (Simulation Only vs Simulation + Rendering), Repeated Scene Switching,
/// and Graphics Feature Isolation across Small, Medium, and Bistro scenes.

#include "core/log.h"
#include "core/time.h"
#include "editor/editor_engine.h"
#include "editor/workspace_manager.h"
#include "framework/runtime_session.h"
#include "framework/scene_serializer.h"
#include "physics/physics_components.h"
#include "render/asset_manager.h"
#include "render/graphics_settings.h"
#include "render/light_components.h"
#include "render/scene_renderer.h"
#include "rhi/command_list.h"
#include "rhi/rhi_device.h"
#include "world/hierarchy_systems.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
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

namespace {

struct StatSummary {
    double avg{0.0};
    double p50{0.0};
    double p95{0.0};
    double p99{0.0};
    double max{0.0};

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
        return s;
    }
};

struct ScenarioMetrics {
    std::string name;
    uint32_t frames{0};
    std::vector<double> frameMs;
    std::vector<double> beginFrameMs;
    std::vector<double> drawPanelsMs;
    std::vector<double> runtimeUpdateMs;
    std::vector<double> renderSceneCpuMs;
    std::vector<double> shadowChecksumCpuMs;
    std::vector<double> shadowCascadeCpuMs;
    std::vector<double> shadowRecordCpuMs;
    std::vector<double> depthPrepassCpuMs;
    std::vector<double> mainPbrCpuMs;
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

    uint64_t sumMainDraws{0};
    uint64_t sumShadowDraws{0};
    uint64_t sumMainTris{0};
    uint64_t sumShadowTris{0};
    uint64_t sumCulled{0};

    void print() const {
        const auto f = StatSummary::compute(frameMs);
        const auto bf = StatSummary::compute(beginFrameMs);
        const auto dp = StatSummary::compute(drawPanelsMs);
        const auto ru = StatSummary::compute(runtimeUpdateMs);
        const auto rsc = StatSummary::compute(renderSceneCpuMs);
        const auto scq = StatSummary::compute(shadowChecksumCpuMs);
        const auto scc = StatSummary::compute(shadowCascadeCpuMs);
        const auto sr = StatSummary::compute(shadowRecordCpuMs);
        const auto dpr = StatSummary::compute(depthPrepassCpuMs);
        const auto mpr = StatSummary::compute(mainPbrCpuMs);
        const auto ef = StatSummary::compute(endFrameMs);
        const auto sw = StatSummary::compute(submitWaitMs);

        const auto gt = StatSummary::compute(gpuTotalMs);
        const auto gs = StatSummary::compute(gpuShadowMs);
        const auto gd = StatSummary::compute(gpuDepthPrepassMs);
        const auto gm = StatSummary::compute(gpuMainPbrMs);
        const auto gv = StatSummary::compute(gpuVelocityMs);
        const auto gtaa = StatSummary::compute(gpuTaaMs);
        const auto ggtao = StatSummary::compute(gpuGtaoMs);
        const auto gssgi = StatSummary::compute(gpuSsgiMs);
        const auto gssr = StatSummary::compute(gpuSsrMs);
        const auto gbl = StatSummary::compute(gpuBloomMs);
        const auto gdof = StatSummary::compute(gpuDofMs);
        const auto gmb = StatSummary::compute(gpuMotionBlurMs);
        const auto gtm = StatSummary::compute(gpuToneMapMs);

        const double fps = (f.avg > 1e-4) ? (1000.0 / f.avg) : 0.0;
        const double dFrames = std::max(1.0, static_cast<double>(frames));

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "\n--- " << name << " (" << frames << " frames) ---\n";
        std::cout << "  Effective FPS: " << fps << " FPS | Frame (ms): avg=" << f.avg
                  << " p50=" << f.p50 << " p95=" << f.p95 << " p99=" << f.p99 << " max=" << f.max
                  << "\n";
        std::cout << "  Geometry (avg/frame): MainDraws="
                  << static_cast<uint64_t>(static_cast<double>(sumMainDraws) / dFrames)
                  << " ShadowDraws="
                  << static_cast<uint64_t>(static_cast<double>(sumShadowDraws) / dFrames)
                  << " MainTris="
                  << static_cast<uint64_t>(static_cast<double>(sumMainTris) / dFrames)
                  << " ShadowTris="
                  << static_cast<uint64_t>(static_cast<double>(sumShadowTris) / dFrames)
                  << " Culled=" << static_cast<uint64_t>(static_cast<double>(sumCulled) / dFrames)
                  << "\n";
        std::cout << "  CPU (avg ms): beginFrame=" << bf.avg << " drawPanels=" << dp.avg
                  << " runtimeUpdate=" << ru.avg << " renderScene=" << rsc.avg
                  << " endFrame=" << ef.avg << " submit+wait=" << sw.avg << "\n";
        std::cout << "  SceneRenderer CPU (avg ms): checksumQuery=" << scq.avg
                  << " cascadeCalc=" << scc.avg << " shadowRec=" << sr.avg
                  << " depthPrepassRec=" << dpr.avg << " mainPbrRec=" << mpr.avg << "\n";
        std::cout << "  GPU (avg ms): total=" << gt.avg << " (p95=" << gt.p95 << " max=" << gt.max
                  << ") | shadow=" << gs.avg << " depthPre=" << gd.avg << " mainPBR=" << gm.avg
                  << " vel=" << gv.avg << " TAA=" << gtaa.avg << "\n";
        std::cout << "                GTAO=" << ggtao.avg << " SSGI=" << gssgi.avg
                  << " SSR=" << gssr.avg << " Bloom=" << gbl.avg << " DoF=" << gdof.avg
                  << " MotBlur=" << gmb.avg << " ToneMap=" << gtm.avg << "\n";
    }
};

enum class CameraMode { Idle, Translate, Rotate };

} // namespace

int main(int argc, char** argv) {
    uint32_t warmUpFrames = 15;
    uint32_t measureFrames = 60;
    if (argc > 1) {
        measureFrames = static_cast<uint32_t>(std::max(10, std::atoi(argv[1])));
    }

#if defined(NDEBUG)
    const char* buildType = "Release";
#else
    const char* buildType = "Debug";
#endif

    std::cout << "=====================================================================\n";
    std::cout << " Hikari Engine Performance Recovery R2 Benchmark (" << buildType << ")\n";
    std::cout << "=====================================================================\n";

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
    const auto& caps = device->getCaps();
    std::cout << "GPU: " << caps.deviceName
              << " | TimestampQueries: " << (caps.features.timestampQueries ? "YES" : "NO") << "\n";

    constexpr uint32_t width = 1280;
    constexpr uint32_t height = 720;

    rhi::TextureDesc rtDesc{
        .type = rhi::TextureType::Texture2D,
        .format = rhi::TextureFormat::BGRA8_UNORM,
        .width = width,
        .height = height,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = rhi::TextureUsageFlags::ColorAttachment | rhi::TextureUsageFlags::TransferSrc,
        .debugName = "RecoveryR2ColorTarget",
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

    auto runBenchmark = [&](const std::string& name, CameraMode camMode, bool playMode,
                            bool renderEnabled) -> ScenarioMetrics {
        ScenarioMetrics m;
        m.name = name;

        if (playMode) {
            auto startRes = runtimeSession.play();
            if (!startRes.hasValue()) {
                std::cerr << "Failed to start RuntimeSession: " << startRes.error().message << "\n";
            }
        }

        const Vec3 baseEye(0.0F, 2.5F, 6.0F);
        const Duration fixedStep = Duration::fromSeconds(1.0 / 60.0);
        Timestamp prevTime = Timestamp::now();
        Duration lastCpuDur = Duration::fromMilliseconds(2);

        const uint32_t totalFrames = warmUpFrames + measureFrames;
        for (uint32_t i = 0; i < totalFrames; ++i) {
            const Timestamp tFrameStart = Timestamp::now();
            const Duration frameDur = tFrameStart - prevTime;
            prevTime = tFrameStart;

            Vec3 eye = baseEye;
            Vec3 target(0.0F, 1.5F, 0.0F);
            if (camMode == CameraMode::Translate) {
                const float t = static_cast<float>(i) * 0.05F;
                eye = baseEye +
                      Vec3(std::sin(t) * 3.0F, std::cos(t * 0.5F) * 0.5F, std::cos(t) * 2.0F);
                target = Vec3(std::sin(t) * 1.5F, 1.5F, 0.0F);
            } else if (camMode == CameraMode::Rotate) {
                const float angle = static_cast<float>(i) * 0.04F;
                target = eye + Vec3(std::sin(angle) * 5.0F, -0.3F, -std::cos(angle) * 5.0F);
            }

            const Mat4 view = Mat4::lookAt(eye, target, Vec3(0.0F, 1.0F, 0.0F));
            const Mat4 proj = Mat4::perspective(1.04719755F, 16.0F / 9.0F, 0.1F, 250.0F);
            const Mat4 vp = proj * view;

            const Timestamp tBfStart = Timestamp::now();
            if (renderEnabled) {
                editor.recordFrameTelemetry(frameDur, lastCpuDur);
                editor.beginFrame(frameDur, width, height);
            }
            const Timestamp tBfEnd = Timestamp::now();

            const Timestamp tDpStart = Timestamp::now();
            if (renderEnabled) {
                editor.drawPanels();
            }
            const Timestamp tDpEnd = Timestamp::now();

            const Timestamp tRuStart = Timestamp::now();
            if (playMode) {
                runtimeSession.update(fixedStep);
            }
            const Timestamp tRuEnd = Timestamp::now();

            double renderSceneMs = 0.0;
            double endFrameMs = 0.0;
            double submitWaitMs = 0.0;

            if (renderEnabled) {
                auto cmdRes = device->createCommandList();
                if (cmdRes.hasValue()) {
                    auto cmd = std::move(cmdRes.value());
                    cmd->begin();

                    const Timestamp tRsStart = Timestamp::now();
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
                    renderSceneMs =
                        static_cast<double>((tRsEnd - tRsStart).asMicroseconds()) * 0.001;

                    rhi::RenderPassBeginInfo passInfo{
                        .colorTarget = rtHandle,
                        .clearColor = {0.10F, 0.12F, 0.16F, 1.0F},
                        .clearTarget = true,
                    };
                    cmd->beginRenderPass(passInfo);
                    const Timestamp tEfStart = Timestamp::now();
                    editor.endFrame(*cmd);
                    const Timestamp tEfEnd = Timestamp::now();
                    endFrameMs = static_cast<double>((tEfEnd - tEfStart).asMicroseconds()) * 0.001;
                    cmd->endRenderPass();
                    cmd->end();

                    const Timestamp tSubStart = Timestamp::now();
                    (void)device->submit(cmd.get());
                    device->waitIdle();
                    const Timestamp tSubEnd = Timestamp::now();
                    submitWaitMs =
                        static_cast<double>((tSubEnd - tSubStart).asMicroseconds()) * 0.001;

                    if (i >= warmUpFrames) {
                        m.sumMainDraws += stats.drawCalls;
                        m.sumShadowDraws += stats.shadowDrawCalls;
                        m.sumMainTris += stats.triangles;
                        m.sumShadowTris += stats.shadowTriangles;
                        m.sumCulled += stats.culledPrimitives;
                    }
                }
            }

            const Timestamp tFrameEnd = Timestamp::now();
            const double totalFrameMs =
                static_cast<double>((tFrameEnd - tFrameStart).asMicroseconds()) * 0.001;
            lastCpuDur = tFrameEnd - tFrameStart;

            if (i >= warmUpFrames) {
                m.frames++;
                m.frameMs.push_back(totalFrameMs);
                m.beginFrameMs.push_back(static_cast<double>((tBfEnd - tBfStart).asMicroseconds()) *
                                         0.001);
                m.drawPanelsMs.push_back(static_cast<double>((tDpEnd - tDpStart).asMicroseconds()) *
                                         0.001);
                m.runtimeUpdateMs.push_back(
                    static_cast<double>((tRuEnd - tRuStart).asMicroseconds()) * 0.001);
                m.renderSceneCpuMs.push_back(renderSceneMs);
                m.endFrameMs.push_back(endFrameMs);
                m.submitWaitMs.push_back(submitWaitMs);

                const auto& cpu = editor.getSceneRenderer().getCpuPassTimings();
                m.shadowChecksumCpuMs.push_back(cpu.shadowChecksumQueryMs);
                m.shadowCascadeCpuMs.push_back(cpu.shadowCascadeCalcMs);
                m.shadowRecordCpuMs.push_back(cpu.shadowRecordMs);
                m.depthPrepassCpuMs.push_back(cpu.depthPrepassRecordMs);
                m.mainPbrCpuMs.push_back(cpu.mainPbrRecordMs);

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
        return m;
    };

    auto runSceneSuite = [&](const std::string& label, const std::string& scenePath) {
        if (!std::filesystem::exists(scenePath)) {
            std::cout << "\n[SKIP] Scene not found: " << scenePath << "\n";
            return;
        }
        std::cout << "\n=====================================================================\n";
        std::cout << " SCENE: " << label << " (" << scenePath << ")\n";
        std::cout << "=====================================================================\n";

        if (!editor.getWorkspaceManager().openScene(editWorld, scenePath.c_str(),
                                                    &editor.getSelectionModel(), nullptr,
                                                    &editor.getSceneRenderer())) {
            std::cerr << "Failed to open scene: " << scenePath << "\n";
            return;
        }
        // Apply controlled High quality preset with TAA + Shadows + Bloom + GTAO + SSR
        auto gs = render::GraphicsSettings::createPreset(render::GraphicsQualityPreset::High);
        gs.quality.antiAliasing = render::AntiAliasingMode::TAA;
        gs.quality.taa.enabled = true;
        gs.shadows.enabled = true;
        gs.lighting.gtaoEnabled = true;
        gs.lighting.ssrEnabled = true;
        gs.lighting.ssgiEnabled = false;
        gs.postProcess.bloomEnabled = true;
        (void)editor.getSceneRenderer().applyGraphicsSettings(gs);

        runBenchmark(label + " | 1. Editor Idle", CameraMode::Idle, false, true).print();
        runBenchmark(label + " | 2. Editor Camera Translate (Shadows ON)", CameraMode::Translate,
                     false, true)
            .print();
        runBenchmark(label + " | 3. Editor Camera Rotate (Shadows ON)", CameraMode::Rotate, false,
                     true)
            .print();
        runBenchmark(label + " | 4. Play Mode (Rendering Disabled)", CameraMode::Idle, true, false)
            .print();
        runBenchmark(label + " | 5. Play Mode (Rendering Enabled)", CameraMode::Idle, true, true)
            .print();

        // Disable shadows for comparison
        auto gsNoShadow = gs;
        gsNoShadow.shadows.enabled = false;
        (void)editor.getSceneRenderer().applyGraphicsSettings(gsNoShadow);
        runBenchmark(label + " | 6. Editor Camera Translate (Shadows OFF)", CameraMode::Translate,
                     false, true)
            .print();
    };

    runSceneSuite("Small (DamagedHelmet)",
                  "assets/benchmark-scenes/DamagedHelmet_Benchmark.hikari");
    runSceneSuite("Medium (Sponza)", "assets/benchmark-scenes/Sponza_Benchmark.hikari");
    runSceneSuite("Large (Bistro)", "assets/benchmark-scenes/Bistro_Benchmark.hikari");

    // Repeated scene switching benchmark
    {
        const std::string p1 = "assets/benchmark-scenes/DamagedHelmet_Benchmark.hikari";
        const std::string p2 = "assets/benchmark-scenes/Sponza_Benchmark.hikari";
        if (std::filesystem::exists(p1) && std::filesystem::exists(p2)) {
            std::cout
                << "\n=====================================================================\n";
            std::cout << " REPEATED SCENE SWITCHING (6 cycles)\n";
            std::cout << "=====================================================================\n";
            std::vector<double> switchMs;
            for (int c = 0; c < 6; ++c) {
                const std::string& target = (c % 2 == 0) ? p1 : p2;
                const Timestamp t0 = Timestamp::now();
                (void)editor.getWorkspaceManager().openScene(editWorld, target.c_str(),
                                                             &editor.getSelectionModel(), nullptr,
                                                             &editor.getSceneRenderer());
                const Timestamp t1 = Timestamp::now();
                switchMs.push_back(static_cast<double>((t1 - t0).asMicroseconds()) * 0.001);
            }
            const auto sw = StatSummary::compute(switchMs);
            std::cout << std::fixed << std::setprecision(2)
                      << "  Scene switch time (ms): avg=" << sw.avg << " p50=" << sw.p50
                      << " max=" << sw.max << "\n";
        }
    }

    // Feature Isolation on Sponza
    {
        const std::string sponza = "assets/benchmark-scenes/Sponza_Benchmark.hikari";
        if (std::filesystem::exists(sponza)) {
            std::cout
                << "\n=====================================================================\n";
            std::cout << " FEATURE ISOLATION (Sponza, Camera Translate)\n";
            std::cout << "=====================================================================\n";
            (void)editor.getWorkspaceManager().openScene(editWorld, sponza.c_str(),
                                                         &editor.getSelectionModel(), nullptr,
                                                         &editor.getSceneRenderer());

            auto base = render::GraphicsSettings::createPreset(render::GraphicsQualityPreset::Low);
            base.quality.antiAliasing = render::AntiAliasingMode::None;
            base.quality.taa.enabled = false;
            base.shadows.enabled = false;
            base.lighting.gtaoEnabled = false;
            base.lighting.ssgiEnabled = false;
            base.lighting.ssrEnabled = false;
            base.postProcess.enabled = true;
            base.postProcess.bloomEnabled = false;
            base.postProcess.dofEnabled = false;
            base.postProcess.motionBlurEnabled = false;

            auto testFeature = [&](const std::string& featName,
                                   const render::GraphicsSettings& cfg) {
                (void)editor.getSceneRenderer().applyGraphicsSettings(cfg);
                runBenchmark("Feature: " + featName, CameraMode::Translate, false, true).print();
            };

            testFeature("Baseline (All Off)", base);

            {
                auto s = base;
                s.shadows.enabled = true;
                s.shadows.cascadeCount = 4;
                testFeature("+ Directional Shadows (4 cascades)", s);
            }
            {
                auto s = base;
                s.lighting.gtaoEnabled = true;
                testFeature("+ GTAO", s);
            }
            {
                auto s = base;
                s.lighting.ssgiEnabled = true;
                testFeature("+ SSGI", s);
            }
            {
                auto s = base;
                s.lighting.ssrEnabled = true;
                testFeature("+ SSR", s);
            }
            {
                auto s = base;
                s.quality.antiAliasing = render::AntiAliasingMode::TAA;
                s.quality.taa.enabled = true;
                testFeature("+ TAA", s);
            }
            {
                auto s = base;
                s.quality.antiAliasing = render::AntiAliasingMode::MSAA4x;
                testFeature("+ MSAA 4x", s);
            }
            {
                auto s = base;
                s.postProcess.bloomEnabled = true;
                testFeature("+ Bloom", s);
            }
            {
                auto s = base;
                s.postProcess.dofEnabled = true;
                testFeature("+ DoF", s);
            }
            {
                auto s = base;
                s.postProcess.motionBlurEnabled = true;
                testFeature("+ Motion Blur", s);
            }
        }
    }

    device->destroyTexture(rtHandle);
    editor.shutdown();
    return 0;
}
