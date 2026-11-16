/// @file main.cpp
/// @brief Standalone executable entrypoint for Hikari Machine Hub.

#include "core/log.h"
#include "hub_app.h"
#include "platform/platform.h"
#include "platform/window.h"
#include "render/image.h"
#include "rhi/command_list.h"
#include "rhi/rhi_device.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
    bool headless = false;
    uint32_t targetFrames = 0; // 0 = run until window close
    uint32_t windowWidth = 1120;
    uint32_t windowHeight = 720;
    std::filesystem::path recentPath{};
    std::filesystem::path customEngine{};

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--headless") {
            headless = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            targetFrames = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--width" && i + 1 < argc) {
            windowWidth = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--height" && i + 1 < argc) {
            windowHeight = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--recents" && i + 1 < argc) {
            recentPath = argv[++i];
        } else if (arg == "--engine" && i + 1 < argc) {
            customEngine = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Hikari Machine Hub 0.1.0\n"
                      << "Usage: hikari_hub [options]\n"
                      << "  --headless         Run without OS window (Null RHI)\n"
                      << "  --frames <N>       Exit cleanly after N frames\n"
                      << "  --width <W>        Window width (default 1120)\n"
                      << "  --height <H>       Window height (default 720)\n"
                      << "  --recents <path>   Override recent-projects JSON path\n"
                      << "  --engine <path>    Override Hikari executable path\n";
            return 0;
        }
    }

    eng::platform::PlatformConfig platConfig{.headless = headless};
    auto platRes = eng::platform::createPlatform(platConfig);
    if (!platRes.hasValue()) {
        ENG_LOG_CRITICAL("Hub", "Failed to initialize platform subsystem");
        return 1;
    }
    auto platform = std::move(platRes.value());

    std::unique_ptr<eng::platform::Window> window;
    std::unique_ptr<eng::rhi::RhiDevice> rhiDevice;

    if (!headless) {
        eng::platform::WindowDesc winDesc{
            .title = "Hikari Machine Hub",
            .width = windowWidth,
            .height = windowHeight,
            .vulkan = true,
        };
        auto winRes = platform->createWindow(winDesc);
        if (winRes.hasValue()) {
            window = std::move(winRes.value());

            const std::filesystem::path repoRoot =
                eng::hub::EngineLauncher::findRepositoryRoot(std::filesystem::current_path());
            if (!repoRoot.empty()) {
                const std::filesystem::path logoPath =
                    repoRoot / "assets/textures/branding/hikari_logo.jpg";
                if (std::filesystem::exists(logoPath)) {
                    auto imgRes =
                        eng::render::ImageDecoder::decodeFromFile(logoPath.string(), 4, true);
                    if (imgRes.hasValue() && imgRes.value().width > 0 &&
                        imgRes.value().height > 0) {
                        const auto& img = imgRes.value();
                        const uint32_t side = std::max(img.width, img.height);
                        std::vector<uint8_t> squarePixels(
                            static_cast<size_t>(side) * static_cast<size_t>(side) * 4ULL, 0U);
                        const uint8_t bgR = img.pixels[0];
                        const uint8_t bgG = img.pixels[1];
                        const uint8_t bgB = img.pixels[2];
                        for (size_t idx = 0; idx < squarePixels.size(); idx += 4) {
                            squarePixels[idx] = bgR;
                            squarePixels[idx + 1] = bgG;
                            squarePixels[idx + 2] = bgB;
                            squarePixels[idx + 3] = 255U;
                        }
                        const uint32_t offsetX = (side - img.width) / 2U;
                        const uint32_t offsetY = (side - img.height) / 2U;
                        for (uint32_t y = 0; y < img.height; ++y) {
                            const uint8_t* srcRow =
                                img.pixels.data() + (static_cast<size_t>(y) * img.width * 4ULL);
                            uint8_t* dstRow =
                                squarePixels.data() +
                                ((static_cast<size_t>(y + offsetY) * side + offsetX) * 4ULL);
                            std::memcpy(dstRow, srcRow, static_cast<size_t>(img.width) * 4ULL);
                        }
                        window->setIcon(squarePixels.data(), side, side);
                    }
                }
            }

            eng::rhi::VulkanDeviceDesc vkDesc{
                .windowHandle = window->getNativeHandle(),
                .enableValidation = false,
            };
            auto devRes = eng::rhi::createVulkanDevice(vkDesc);
            if (devRes.hasValue()) {
                rhiDevice = std::move(devRes.value());
            }
        }
    }

    if (!rhiDevice) {
        auto nullRes = eng::rhi::createNullDevice();
        if (!nullRes.hasValue()) {
            return 1;
        }
        rhiDevice = std::move(nullRes.value());
    }

    auto cmdRes = rhiDevice->createCommandList();
    if (!cmdRes.hasValue()) {
        return 1;
    }
    auto mainCmd = std::move(cmdRes.value());

    eng::hub::HubApp app;
    const eng::hub::HubConfig cfg{
        .hubExecutablePath = (argc > 0 && argv[0] != nullptr) ? std::filesystem::path(argv[0])
                                                              : std::filesystem::path{},
        .recentProjectsPath = recentPath,
        .customEngineExecutable = customEngine,
    };
    if (!app.initialize(*rhiDevice, cfg)) {
        return 1;
    }

    uint32_t frameCount = 0;
    uint32_t lastWidth = window ? window->getWidth() : windowWidth;
    uint32_t lastHeight = window ? window->getHeight() : windowHeight;
    auto lastTime = std::chrono::steady_clock::now();

    while (true) {
        if (targetFrames > 0 && frameCount >= targetFrames) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        platform->pollEvents();
        const auto& input = platform->getCurrentInput();

        if (window && window->shouldClose()) {
            break;
        }

        if (window) {
            windowWidth = window->getWidth();
            windowHeight = window->getHeight();
            if (!window->isMinimized() &&
                (windowWidth != lastWidth || windowHeight != lastHeight)) {
                (void)rhiDevice->resize(windowWidth, windowHeight);
                lastWidth = windowWidth;
                lastHeight = windowHeight;
            }
        }

        app.processInput(input);
        app.beginFrame(dt, windowWidth, windowHeight);
        app.drawUi();

        mainCmd->begin();
        eng::rhi::RenderPassBeginInfo passInfo{
            .clearColor = {0.09F, 0.10F, 0.13F, 1.0F},
            .clearTarget = true,
        };
        mainCmd->beginRenderPass(passInfo);
        app.endFrame(*mainCmd);
        mainCmd->endRenderPass();
        mainCmd->end();

        (void)rhiDevice->submit(mainCmd.get());
        if (window) {
            (void)rhiDevice->present();
        }

        ++frameCount;
    }

    app.shutdown();
    return 0;
}
