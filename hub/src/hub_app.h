/// @file hub_app.h
/// @brief Standalone Dear ImGui application controller for Hikari Machine Hub.

#pragma once

#include "hub/engine_launcher.h"
#include "hub/project_validator.h"
#include "hub/recent_projects.h"
#include "hub/sample_catalog.h"
#include "hub/template_registry.h"
#include "hub_imgui_backend.h"
#include "platform/input_snapshot.h"
#include "render/gpu_texture.h"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ImGuiContext;

namespace eng::rhi {
class RhiDevice;
class RhiCommandList;
} // namespace eng::rhi

namespace eng::hub {

/// @enum HubTab
/// @brief Primary navigation sections of Hikari Machine Hub.
enum class HubTab : uint8_t {
    Home = 0,
    Projects = 1,
    Templates = 2,
    Samples = 3,
    Settings = 4,
};

/// @struct HubConfig
/// @brief Startup configuration for the Hub UI.
struct HubConfig {
    std::filesystem::path hubExecutablePath{};
    std::filesystem::path recentProjectsPath{};
    std::filesystem::path customEngineExecutable{};
};

/// @class HubApp
/// @brief Manages Hub UI state, project workflows, and ImGui rendering over RHI.
class HubApp {
public:
    HubApp() = default;
    ~HubApp();

    HubApp(const HubApp&) = delete;
    HubApp& operator=(const HubApp&) = delete;

    [[nodiscard]] bool initialize(rhi::RhiDevice& device, const HubConfig& config);
    void shutdown();

    void processInput(const platform::InputSnapshot& input);
    void beginFrame(float deltaSeconds, uint32_t width, uint32_t height);
    void drawUi();
    void endFrame(rhi::RhiCommandList& cmdList);

private:
    void applyTheme();
    void rescanEnvironment();
    void drawSidebar();
    void drawStatusBanner();
    void drawHomeTab();
    void drawProjectsTab();
    void drawTemplatesTab();
    void drawSamplesTab();
    void drawSettingsTab();
    void drawValidationReport(const ProjectValidationReport& report);

    void openProjectDirectory(const std::filesystem::path& projectDir, bool launchInEditor);
    void launchProjectInEditor(const std::filesystem::path& projectDir,
                               const ProjectMetadata& metadata);
    void launchSampleInEditor(const SampleEntry& sample);
    void setStatus(bool isError, std::string message);

    rhi::RhiDevice* device_{nullptr};
    ImGuiContext* imguiContext_{nullptr};
    HubImGuiBackend imguiBackend_{};
    std::unique_ptr<render::GpuTexture> logoTexture_{};
    uint64_t logoTextureId_{0};

    HubConfig config_{};
    std::filesystem::path recentStoragePath_{};
    RecentProjectsStore recents_{};
    EngineInstallation engineInfo_{};
    std::vector<SampleEntry> samples_{};

    HubTab activeTab_{HubTab::Home};
    std::optional<ProjectValidationReport> activeReport_{};

    // Status notification banner
    bool statusIsError_{false};
    std::string statusMessage_{};

    // Open Existing Project input
    std::array<char, 512> openProjectPathBuf_{};

    // Create Project form state
    size_t selectedTemplateIndex_{1}; // Default to Basic 3D Scene
    std::array<char, 128> createProjectNameBuf_{};
    std::array<char, 512> createParentDirBuf_{};

    // Settings form state
    std::array<char, 512> engineOverrideBuf_{};

    bool initialized_{false};
};

} // namespace eng::hub
