/// @file hub_app.cpp
/// @brief Standalone Dear ImGui application controller for Hikari Machine Hub.

#include "hub_app.h"

#include "core/log.h"
#include "render/image.h"
#include "rhi/command_list.h"
#include "rhi/rhi_device.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <system_error>

namespace eng::hub {

namespace {

void copyStringToBuf(std::string_view src, char* dst, size_t dstSize) {
    if (dst == nullptr || dstSize == 0) {
        return;
    }
    const size_t n = std::min(src.size(), dstSize - 1U);
    if (n > 0) {
        std::memcpy(dst, src.data(), n);
    }
    dst[n] = '\0';
}

int64_t nowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

ImGuiKey mapKeyCode(platform::KeyCode code) {
    using K = platform::KeyCode;
    switch (code) {
    case K::Tab:
        return ImGuiKey_Tab;
    case K::Left:
        return ImGuiKey_LeftArrow;
    case K::Right:
        return ImGuiKey_RightArrow;
    case K::Up:
        return ImGuiKey_UpArrow;
    case K::Down:
        return ImGuiKey_DownArrow;
    case K::PageUp:
        return ImGuiKey_PageUp;
    case K::PageDown:
        return ImGuiKey_PageDown;
    case K::Home:
        return ImGuiKey_Home;
    case K::End:
        return ImGuiKey_End;
    case K::Delete:
        return ImGuiKey_Delete;
    case K::Backspace:
        return ImGuiKey_Backspace;
    case K::Return:
        return ImGuiKey_Enter;
    case K::Escape:
        return ImGuiKey_Escape;
    case K::Space:
        return ImGuiKey_Space;
    case K::A:
        return ImGuiKey_A;
    case K::C:
        return ImGuiKey_C;
    case K::V:
        return ImGuiKey_V;
    case K::X:
        return ImGuiKey_X;
    case K::Z:
        return ImGuiKey_Z;
    default:
        return ImGuiKey_None;
    }
}

} // namespace

HubApp::~HubApp() {
    shutdown();
}

bool HubApp::initialize(rhi::RhiDevice& device, const HubConfig& config) {
    if (initialized_) {
        return true;
    }

    device_ = &device;
    config_ = config;
    recentStoragePath_ = config.recentProjectsPath.empty()
                             ? RecentProjectsStore::defaultStoragePath()
                             : config.recentProjectsPath;

    imguiContext_ = ImGui::CreateContext();
    if (imguiContext_ == nullptr) {
        return false;
    }
    ImGui::SetCurrentContext(imguiContext_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Try system desktop UI font if available, fall back to built-in ProggyClean.
    constexpr const char* kSystemFont = "/usr/share/fonts/noto/NotoSans-Regular.ttf";
    std::error_code ec;
    if (std::filesystem::exists(kSystemFont, ec)) {
        io.Fonts->AddFontFromFileTTF(kSystemFont, 16.0F);
    }

    applyTheme();

    if (!imguiBackend_.initialize(device)) {
        ImGui::DestroyContext(imguiContext_);
        imguiContext_ = nullptr;
        return false;
    }

    copyStringToBuf(config.customEngineExecutable.string(), engineOverrideBuf_.data(),
                    engineOverrideBuf_.size());
    rescanEnvironment();

    // Load Hikari branding logo if available
    if (!engineInfo_.repositoryRoot.empty()) {
        const std::filesystem::path logoPath =
            engineInfo_.repositoryRoot / "assets/textures/branding/hikari_logo.jpg";
        if (std::filesystem::exists(logoPath, ec)) {
            auto imgRes = render::ImageDecoder::decodeFromFile(logoPath.string(), 4, true);
            if (imgRes.hasValue()) {
                auto texRes = render::GpuTexture::create(device, imgRes.value(), false);
                if (texRes.hasValue()) {
                    logoTexture_ = std::move(texRes.value());
                    logoTextureId_ = imguiBackend_.registerTexture(logoTexture_->getTexture(),
                                                                   logoTexture_->getSampler());
                }
            }
        }
    }

    auto loadRes = recents_.load(recentStoragePath_);
    if (!loadRes.hasValue()) {
        setStatus(true, "Failed to load recent projects: " + std::string(loadRes.error().message));
    }

    copyStringToBuf("MyAnterayProject", createProjectNameBuf_.data(), createProjectNameBuf_.size());
    const std::filesystem::path defaultProjectsRoot =
        std::filesystem::current_path(ec) / "projects";
    copyStringToBuf(defaultProjectsRoot.string(), createParentDirBuf_.data(),
                    createParentDirBuf_.size());

    initialized_ = true;
    return true;
}

void HubApp::shutdown() {
    if (!initialized_) {
        return;
    }

    if (device_ != nullptr) {
        device_->waitIdle();
    }

    if (logoTextureId_ != 0) {
        imguiBackend_.unregisterTexture(logoTextureId_);
        logoTextureId_ = 0;
    }
    logoTexture_.reset();
    imguiBackend_.shutdown();

    if (imguiContext_ != nullptr) {
        ImGui::DestroyContext(imguiContext_);
        imguiContext_ = nullptr;
    }

    device_ = nullptr;
    initialized_ = false;
}

void HubApp::applyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0F;
    style.ChildRounding = 6.0F;
    style.FrameRounding = 4.0F;
    style.GrabRounding = 4.0F;
    style.WindowPadding = ImVec2(16.0F, 16.0F);
    style.FramePadding = ImVec2(10.0F, 6.0F);
    style.ItemSpacing = ImVec2(10.0F, 8.0F);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.09F, 0.10F, 0.13F, 1.00F);
    c[ImGuiCol_ChildBg] = ImVec4(0.12F, 0.14F, 0.18F, 1.00F);
    c[ImGuiCol_Border] = ImVec4(0.22F, 0.25F, 0.31F, 1.00F);
    c[ImGuiCol_FrameBg] = ImVec4(0.16F, 0.18F, 0.23F, 1.00F);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21F, 0.24F, 0.30F, 1.00F);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.24F, 0.28F, 0.35F, 1.00F);
    c[ImGuiCol_Button] = ImVec4(0.16F, 0.42F, 0.68F, 1.00F);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22F, 0.52F, 0.82F, 1.00F);
    c[ImGuiCol_ButtonActive] = ImVec4(0.14F, 0.36F, 0.60F, 1.00F);
    c[ImGuiCol_Header] = ImVec4(0.18F, 0.36F, 0.56F, 0.85F);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22F, 0.44F, 0.68F, 0.90F);
    c[ImGuiCol_HeaderActive] = ImVec4(0.18F, 0.40F, 0.64F, 1.00F);
    c[ImGuiCol_Text] = ImVec4(0.92F, 0.94F, 0.97F, 1.00F);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55F, 0.59F, 0.66F, 1.00F);
}

void HubApp::rescanEnvironment() {
    const std::filesystem::path overridePath(engineOverrideBuf_.data());
    engineInfo_ = EngineLauncher::discover(config_.hubExecutablePath, overridePath);
    if (!engineInfo_.repositoryRoot.empty()) {
        samples_ = SampleCatalog::discoverSamples(engineInfo_.repositoryRoot);
    } else {
        samples_.clear();
    }
    recents_.refreshAvailability();
}

void HubApp::processInput(const platform::InputSnapshot& input) {
    if (!initialized_) {
        return;
    }

    ImGui::SetCurrentContext(imguiContext_);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(input.mouseX, input.mouseY);
    io.AddMouseButtonEvent(
        0, input.mouseButtons.test(static_cast<size_t>(platform::MouseButton::Left)));
    io.AddMouseButtonEvent(
        1, input.mouseButtons.test(static_cast<size_t>(platform::MouseButton::Right)));
    io.AddMouseButtonEvent(
        2, input.mouseButtons.test(static_cast<size_t>(platform::MouseButton::Middle)));
    io.AddMouseWheelEvent(input.wheelDeltaX, input.wheelDeltaY);

    if (input.textInputLength > 0) {
        io.AddInputCharactersUTF8(input.textInput.data());
    }

    const bool ctrl = input.keys.test(static_cast<size_t>(platform::KeyCode::LeftCtrl)) ||
                      input.keys.test(static_cast<size_t>(platform::KeyCode::RightCtrl));
    const bool shift = input.keys.test(static_cast<size_t>(platform::KeyCode::LeftShift)) ||
                       input.keys.test(static_cast<size_t>(platform::KeyCode::RightShift));
    const bool alt = input.keys.test(static_cast<size_t>(platform::KeyCode::LeftAlt)) ||
                     input.keys.test(static_cast<size_t>(platform::KeyCode::RightAlt));
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    io.AddKeyEvent(ImGuiMod_Alt, alt);

    for (uint32_t i = 0; i < input.keyEventCount; ++i) {
        const auto& ev = input.keyEvents[i];
        const ImGuiKey ik = mapKeyCode(ev.code);
        if (ik != ImGuiKey_None) {
            io.AddKeyEvent(ik, ev.down);
        }
    }
}

void HubApp::beginFrame(float deltaSeconds, uint32_t width, uint32_t height) {
    if (!initialized_) {
        return;
    }

    ImGui::SetCurrentContext(imguiContext_);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize =
        ImVec2(static_cast<float>(std::max(1U, width)), static_cast<float>(std::max(1U, height)));
    io.DisplayFramebufferScale = ImVec2(1.0F, 1.0F);
    io.DeltaTime = std::max(0.0001F, deltaSeconds);
    ImGui::NewFrame();
}

void HubApp::drawUi() {
    if (!initialized_) {
        return;
    }

    ImGui::SetCurrentContext(imguiContext_);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);

    constexpr ImGuiWindowFlags kRootFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("HikariMachineHubRoot", nullptr, kRootFlags);

    ImGui::BeginChild("HubSidebar", ImVec2(220.0F, 0.0F), true);
    drawSidebar();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("HubContent", ImVec2(0.0F, 0.0F), false);
    drawStatusBanner();

    switch (activeTab_) {
    case HubTab::Home:
        drawHomeTab();
        break;
    case HubTab::Projects:
        drawProjectsTab();
        break;
    case HubTab::Templates:
        drawTemplatesTab();
        break;
    case HubTab::Samples:
        drawSamplesTab();
        break;
    case HubTab::Settings:
        drawSettingsTab();
        break;
    }
    ImGui::EndChild();

    ImGui::End();
}

void HubApp::endFrame(rhi::RhiCommandList& cmdList) {
    if (!initialized_) {
        return;
    }

    ImGui::SetCurrentContext(imguiContext_);
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData != nullptr && imguiBackend_.isInitialized() && cmdList.isRecording()) {
        imguiBackend_.renderDrawData(drawData, cmdList);
    }
}

void HubApp::drawSidebar() {
    if (logoTextureId_ != 0) {
        const float logoSize = 72.0F;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - logoSize) * 0.5F +
                             ImGui::GetCursorPosX());
        ImGui::Image(static_cast<ImTextureID>(logoTextureId_), ImVec2(logoSize, logoSize));
    }

    ImGui::TextColored(ImVec4(0.32F, 0.78F, 0.98F, 1.0F), "ANTERAY HUB");
    ImGui::TextDisabled("Project Management MVP v%s", kCurrentEngineVersion.data());
    ImGui::Separator();
    ImGui::Spacing();

    auto navButton = [&](const char* label, HubTab tab) {
        const bool selected = (activeTab_ == tab);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20F, 0.48F, 0.78F, 1.0F));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14F, 0.16F, 0.21F, 1.0F));
        }
        if (ImGui::Button(label, ImVec2(-1.0F, 36.0F))) {
            activeTab_ = tab;
        }
        ImGui::PopStyleColor();
    };

    navButton("Home", HubTab::Home);
    navButton("Projects", HubTab::Projects);
    navButton("Templates", HubTab::Templates);
    navButton("Samples", HubTab::Samples);
    navButton("Settings", HubTab::Settings);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Engine Status");
    if (engineInfo_.available) {
        ImGui::TextColored(ImVec4(0.30F, 0.85F, 0.48F, 1.0F), "Editor Ready (%s)",
                           engineInfo_.buildFlavor.c_str());
        ImGui::TextWrapped("%s", engineInfo_.executablePath.filename().string().c_str());
    } else {
        ImGui::TextColored(ImVec4(0.95F, 0.65F, 0.25F, 1.0F), "Editor Unavailable");
        ImGui::TextDisabled("Check Settings tab");
    }
}

void HubApp::drawStatusBanner() {
    if (statusMessage_.empty()) {
        return;
    }
    const ImVec4 bg =
        statusIsError_ ? ImVec4(0.45F, 0.14F, 0.14F, 1.0F) : ImVec4(0.12F, 0.34F, 0.24F, 1.0F);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::BeginChild("StatusBanner", ImVec2(0.0F, 44.0F), true);
    ImGui::TextWrapped("%s", statusMessage_.c_str());
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0F);
    if (ImGui::SmallButton("Dismiss")) {
        statusMessage_.clear();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void HubApp::drawHomeTab() {
    ImGui::Text("Welcome to ANTERAY Hub");
    ImGui::TextDisabled("Create, validate, and launch ANTERAY ENGINE projects.");
    ImGui::Spacing();

    if (ImGui::Button("Create New Project", ImVec2(190.0F, 38.0F))) {
        activeTab_ = HubTab::Templates;
    }
    ImGui::SameLine();
    if (ImGui::Button("Open Existing Project", ImVec2(190.0F, 38.0F))) {
        activeTab_ = HubTab::Projects;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse Samples", ImVec2(170.0F, 38.0F))) {
        activeTab_ = HubTab::Samples;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Recent Projects (%zu)", recents_.entries().size());
    if (recents_.entries().empty()) {
        ImGui::BeginChild("EmptyRecents", ImVec2(0.0F, 120.0F), true);
        ImGui::TextDisabled("No recent projects yet.");
        ImGui::TextWrapped("Create a new project from the Templates tab or open an existing "
                           "project directory in the Projects tab.");
        ImGui::EndChild();
        return;
    }

    const size_t maxHome = std::min<size_t>(recents_.entries().size(), 5U);
    for (size_t i = 0; i < maxHome; ++i) {
        const auto& entry = recents_.entries()[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginChild("HomeRecentCard", ImVec2(0.0F, 82.0F), true);
        ImGui::Text("%s", entry.projectName.empty() ? "(Unnamed)" : entry.projectName.c_str());
        ImGui::SameLine();
        if (!entry.existsOnDisk) {
            ImGui::TextColored(ImVec4(0.95F, 0.35F, 0.35F, 1.0F), "[Missing]");
        } else {
            ImGui::TextColored(ImVec4(0.35F, 0.85F, 0.55F, 1.0F), "[Available]");
        }
        ImGui::TextDisabled("%s | Engine %s", entry.projectDirectory.string().c_str(),
                            entry.engineVersion.empty() ? "unspecified"
                                                        : entry.engineVersion.c_str());

        if (ImGui::SmallButton("Validate")) {
            activeReport_ = validateProjectDirectory(entry.projectDirectory);
            activeTab_ = HubTab::Projects;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!entry.existsOnDisk || !engineInfo_.available);
        if (ImGui::SmallButton("Open in Editor")) {
            openProjectDirectory(entry.projectDirectory, true);
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::PopID();
    }
}

void HubApp::drawProjectsTab() {
    ImGui::Text("Projects");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Open Existing Project Directory");
    ImGui::SetNextItemWidth(-220.0F);
    ImGui::InputText("##OpenProjectDir", openProjectPathBuf_.data(), openProjectPathBuf_.size());
    ImGui::SameLine();
    if (ImGui::Button("Validate & Add", ImVec2(120.0F, 0.0F))) {
        openProjectDirectory(openProjectPathBuf_.data(), false);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!engineInfo_.available);
    if (ImGui::Button("Open Editor", ImVec2(90.0F, 0.0F))) {
        openProjectDirectory(openProjectPathBuf_.data(), true);
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Recent Projects");
    std::filesystem::path toRemove{};
    if (recents_.entries().empty()) {
        ImGui::TextDisabled("No recent projects recorded.");
    } else {
        for (size_t i = 0; i < recents_.entries().size(); ++i) {
            const auto& entry = recents_.entries()[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginChild("ProjectRow", ImVec2(0.0F, 88.0F), true);
            ImGui::Text("%s", entry.projectName.c_str());
            ImGui::SameLine();
            if (!entry.existsOnDisk) {
                ImGui::TextColored(ImVec4(0.95F, 0.35F, 0.35F, 1.0F), "[Missing on disk]");
            } else {
                ImGui::TextColored(ImVec4(0.35F, 0.85F, 0.55F, 1.0F), "[Found]");
            }
            ImGui::TextDisabled("Location: %s", entry.projectDirectory.string().c_str());
            ImGui::TextDisabled("Engine: %s | Last Opened: %lld",
                                entry.engineVersion.empty() ? "unspecified"
                                                            : entry.engineVersion.c_str(),
                                static_cast<long long>(entry.lastOpenedTimestamp));

            if (ImGui::SmallButton("Validate")) {
                activeReport_ = validateProjectDirectory(entry.projectDirectory);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!entry.existsOnDisk || !engineInfo_.available);
            if (ImGui::SmallButton("Open in Editor")) {
                openProjectDirectory(entry.projectDirectory, true);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove from Recents")) {
                toRemove = entry.projectDirectory;
            }
            ImGui::EndChild();
            ImGui::PopID();
        }
    }

    if (!toRemove.empty()) {
        if (recents_.remove(toRemove)) {
            (void)recents_.saveAtomic(recentStoragePath_);
            setStatus(false, "Removed project from recent list (files on disk were not deleted).");
        }
    }

    if (activeReport_.has_value()) {
        ImGui::Spacing();
        drawValidationReport(*activeReport_);
    }
}

void HubApp::drawValidationReport(const ProjectValidationReport& report) {
    ImGui::BeginChild("ValidationReport", ImVec2(0.0F, 190.0F), true);
    ImGui::Text("Validation Report: %s", report.projectDirectory.string().c_str());
    ImGui::SameLine();
    if (report.status == ProjectValidationStatus::Valid) {
        ImGui::TextColored(ImVec4(0.30F, 0.85F, 0.48F, 1.0F), "[VALID]");
    } else if (report.status == ProjectValidationStatus::ValidWithWarnings) {
        ImGui::TextColored(ImVec4(0.95F, 0.72F, 0.25F, 1.0F), "[VALID WITH WARNINGS]");
    } else {
        ImGui::TextColored(ImVec4(0.95F, 0.35F, 0.35F, 1.0F), "[INVALID]");
    }
    ImGui::Separator();

    if (report.issues.empty()) {
        ImGui::TextColored(ImVec4(0.30F, 0.85F, 0.48F, 1.0F),
                           "All manifest, directory, and startup scene checks passed.");
    } else {
        for (const auto& issue : report.issues) {
            const ImVec4 col = (issue.severity == ValidationIssueSeverity::Error)
                                   ? ImVec4(0.95F, 0.35F, 0.35F, 1.0F)
                                   : ImVec4(0.95F, 0.75F, 0.25F, 1.0F);
            ImGui::TextColored(col, "[%s] %s", issue.code.c_str(), issue.message.c_str());
        }
    }
    ImGui::EndChild();
}

void HubApp::drawTemplatesTab() {
    ImGui::Text("Create New Project from Template");
    ImGui::Separator();
    ImGui::Spacing();

    const auto& templates = TemplateRegistry::builtinTemplates();
    if (selectedTemplateIndex_ >= templates.size()) {
        selectedTemplateIndex_ = 0;
    }

    ImGui::BeginChild("TemplateList", ImVec2(280.0F, 280.0F), true);
    for (size_t i = 0; i < templates.size(); ++i) {
        const auto& tmpl = templates[i];
        const bool selected = (i == selectedTemplateIndex_);
        if (ImGui::Selectable(tmpl.name.c_str(), selected)) {
            selectedTemplateIndex_ = i;
        }
        ImGui::TextDisabled("  %s", tmpl.category.c_str());
    }
    ImGui::EndChild();

    ImGui::SameLine();

    const auto& activeTmpl = templates[selectedTemplateIndex_];
    ImGui::BeginChild("TemplateDetails", ImVec2(0.0F, 280.0F), true);
    ImGui::TextColored(ImVec4(0.32F, 0.78F, 0.98F, 1.0F), "%s", activeTmpl.name.c_str());
    ImGui::TextWrapped("%s", activeTmpl.description.c_str());
    ImGui::Spacing();
    ImGui::Text("Startup Scene: %s", activeTmpl.defaultStartupScene.empty()
                                         ? "(None — opens empty scene)"
                                         : activeTmpl.defaultStartupScene.c_str());
    ImGui::Spacing();
    ImGui::Text("Verified Capabilities:");
    for (const auto& cap : activeTmpl.capabilities) {
        ImGui::BulletText("%s", cap.c_str());
    }
    ImGui::Text("Requirements:");
    for (const auto& req : activeTmpl.requirements) {
        ImGui::BulletText("%s", req.c_str());
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::BeginChild("CreateProjectForm", ImVec2(0.0F, 0.0F), true);
    ImGui::Text("Project Configuration");
    ImGui::InputText("Project Name", createProjectNameBuf_.data(), createProjectNameBuf_.size());
    ImGui::InputText("Parent Directory", createParentDirBuf_.data(), createParentDirBuf_.size());

    const auto nameCheck = validateProjectName(createProjectNameBuf_.data());
    const std::filesystem::path previewTarget =
        std::filesystem::path(createParentDirBuf_.data()) / createProjectNameBuf_.data();
    std::error_code ec;
    const bool targetExists = std::filesystem::exists(previewTarget, ec);

    ImGui::TextDisabled("Target Location: %s", previewTarget.string().c_str());
    if (!nameCheck.hasValue()) {
        ImGui::TextColored(ImVec4(0.95F, 0.35F, 0.35F, 1.0F), "Invalid name: %s",
                           std::string(nameCheck.error().message).c_str());
    } else if (targetExists) {
        ImGui::TextColored(ImVec4(0.95F, 0.35F, 0.35F, 1.0F),
                           "Target directory already exists (will not overwrite).");
    }

    const bool canCreate = nameCheck.hasValue() && !targetExists;
    ImGui::Spacing();
    ImGui::BeginDisabled(!canCreate);
    if (ImGui::Button("Create Project", ImVec2(160.0F, 36.0F))) {
        std::filesystem::create_directories(createParentDirBuf_.data(), ec);
        ProjectCreateRequest req{
            .projectName = createProjectNameBuf_.data(),
            .parentDirectory = createParentDirBuf_.data(),
            .templateId = activeTmpl.id,
            .repositoryRoot = engineInfo_.repositoryRoot,
        };
        auto res = TemplateRegistry::createProjectFromBuiltin(req);
        if (res.hasValue()) {
            openProjectDirectory(res.value().projectDirectory, false);
            setStatus(false, "Created project '" + res.value().metadata.projectName + "' at " +
                                 res.value().projectDirectory.string());
        } else {
            setStatus(true, "Project creation failed: " + std::string(res.error().message));
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!engineInfo_.available);
    if (ImGui::Button("Create & Open in Editor", ImVec2(210.0F, 36.0F))) {
        std::filesystem::create_directories(createParentDirBuf_.data(), ec);
        ProjectCreateRequest req{
            .projectName = createProjectNameBuf_.data(),
            .parentDirectory = createParentDirBuf_.data(),
            .templateId = activeTmpl.id,
            .repositoryRoot = engineInfo_.repositoryRoot,
        };
        auto res = TemplateRegistry::createProjectFromBuiltin(req);
        if (res.hasValue()) {
            openProjectDirectory(res.value().projectDirectory, true);
        } else {
            setStatus(true, "Project creation failed: " + std::string(res.error().message));
        }
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::EndChild();
}

void HubApp::drawSamplesTab() {
    ImGui::Text("Benchmark & Sample Library");
    ImGui::TextDisabled(
        "Cataloged from repository assets without duplicating large datasets into projects.");
    ImGui::Separator();
    ImGui::Spacing();

    if (samples_.empty()) {
        ImGui::TextDisabled("No repository root detected. Configure Engine/Repo path in Settings.");
        return;
    }

    for (size_t i = 0; i < samples_.size(); ++i) {
        const auto& sample = samples_[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginChild("SampleCard", ImVec2(0.0F, 130.0F), true);
        ImGui::TextColored(ImVec4(0.32F, 0.78F, 0.98F, 1.0F), "%s", sample.name.c_str());
        ImGui::SameLine();
        if (sample.canOpenInEditor) {
            ImGui::TextColored(ImVec4(0.30F, 0.85F, 0.48F, 1.0F), "[Ready | %s]",
                               SampleCatalog::formatByteSize(sample.measuredDiskBytes).c_str());
        } else {
            ImGui::TextColored(ImVec4(0.95F, 0.35F, 0.35F, 1.0F), "[Missing Assets]");
        }
        ImGui::TextWrapped("%s", sample.purpose.c_str());
        ImGui::TextDisabled("Scene: %s", sample.sceneRelativePath.c_str());
        ImGui::TextDisabled("Note: %s", sample.knownLimitations.c_str());

        ImGui::BeginDisabled(!sample.canOpenInEditor || !engineInfo_.available);
        if (ImGui::SmallButton("Open Sample in Editor")) {
            launchSampleInEditor(sample);
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::PopID();
    }
}

void HubApp::drawSettingsTab() {
    ImGui::Text("Hub Settings & Engine Discovery");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginChild("SettingsPanel", ImVec2(0.0F, 0.0F), true);
    ImGui::Text("Detected Engine Executable:");
    ImGui::TextDisabled("%s", engineInfo_.available ? engineInfo_.executablePath.string().c_str()
                                                    : "(Not found)");
    ImGui::Text("Build Flavor: %s | Version: %s",
                engineInfo_.buildFlavor.empty() ? "unknown" : engineInfo_.buildFlavor.c_str(),
                engineInfo_.engineVersion.c_str());
    ImGui::Text("Repository Root: %s", engineInfo_.repositoryRoot.empty()
                                           ? "(Not detected)"
                                           : engineInfo_.repositoryRoot.string().c_str());
    ImGui::Text("Recent Projects Store: %s", recentStoragePath_.string().c_str());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::InputText("Custom Engine Executable Override", engineOverrideBuf_.data(),
                     engineOverrideBuf_.size());
    if (ImGui::Button("Rescan Engine & Samples", ImVec2(220.0F, 34.0F))) {
        rescanEnvironment();
        setStatus(!engineInfo_.available,
                  engineInfo_.available
                      ? "Discovered ANTERAY executable: " + engineInfo_.executablePath.string()
                      : "Could not locate ANTERAY executable.");
    }
    ImGui::EndChild();
}

void HubApp::openProjectDirectory(const std::filesystem::path& projectDir, bool launchInEditor) {
    ProjectValidationReport report = validateProjectDirectory(projectDir);
    activeReport_ = report;

    if (!report.isValid() || !report.metadata.has_value()) {
        setStatus(true, "Project validation failed for: " + projectDir.string());
        activeTab_ = HubTab::Projects;
        return;
    }

    RecentProjectEntry entry{};
    entry.projectName = report.metadata->projectName;
    entry.projectDirectory = projectDir;
    entry.manifestPath = report.manifestPath;
    entry.lastOpenedTimestamp = nowUnixSeconds();
    entry.engineVersion = report.metadata->engineVersion;
    entry.existsOnDisk = true;
    recents_.upsert(std::move(entry));
    (void)recents_.saveAtomic(recentStoragePath_);

    if (launchInEditor) {
        launchProjectInEditor(projectDir, *report.metadata);
    } else {
        setStatus(false, "Validated and added project '" + report.metadata->projectName + "'.");
    }
}

void HubApp::launchProjectInEditor(const std::filesystem::path& projectDir,
                                   const ProjectMetadata& metadata) {
    if (!engineInfo_.available) {
        setStatus(true, "Cannot launch Editor: ANTERAY executable not found.");
        return;
    }
    const auto args = EngineLauncher::buildProjectLaunchArgs(projectDir, metadata);
    const std::filesystem::path workDir =
        engineInfo_.repositoryRoot.empty() ? projectDir : engineInfo_.repositoryRoot;
    auto launchRes = EngineLauncher::launchDetached(engineInfo_.executablePath, args, workDir);
    if (launchRes.hasValue()) {
        setStatus(false, "Launched ANTERAY Editor (PID " + std::to_string(launchRes.value()) +
                             ") for '" + metadata.projectName + "'.");
    } else {
        setStatus(true, "Failed to launch Editor: " + std::string(launchRes.error().message));
    }
}

void HubApp::launchSampleInEditor(const SampleEntry& sample) {
    if (!engineInfo_.available || engineInfo_.repositoryRoot.empty()) {
        setStatus(true, "Cannot launch sample: ANTERAY executable or repository root unavailable.");
        return;
    }
    const std::filesystem::path sceneAbs = engineInfo_.repositoryRoot / sample.sceneRelativePath;
    const auto args = EngineLauncher::buildSceneLaunchArgs(sceneAbs);
    auto launchRes = EngineLauncher::launchDetached(engineInfo_.executablePath, args,
                                                    engineInfo_.repositoryRoot);
    if (launchRes.hasValue()) {
        setStatus(false, "Launched sample '" + sample.name + "' in ANTERAY Editor (PID " +
                             std::to_string(launchRes.value()) + ").");
    } else {
        setStatus(true, "Failed to launch sample: " + std::string(launchRes.error().message));
    }
}

void HubApp::setStatus(bool isError, std::string message) {
    statusIsError_ = isError;
    statusMessage_ = std::move(message);
}

} // namespace eng::hub
