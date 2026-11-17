/// @file engine_launcher.h
/// @brief Engine/Editor executable discovery, launch argument construction, and process spawning.
///
/// Purpose:
/// Discovers the ANTERAY Engine/Editor executable associated with the current build or repository
/// without hardcoding machine-specific paths, constructs verified CLI arguments, and launches
/// detached Editor processes.

#pragma once

#include "core/result.h"
#include "hub/project_metadata.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace eng::hub {

/// @struct EngineInstallation
/// @brief Discovered ANTERAY Engine/Editor executable and repository context.
struct EngineInstallation {
    std::filesystem::path executablePath{};
    std::filesystem::path repositoryRoot{};
    std::string engineVersion{std::string(kCurrentEngineVersion)};
    std::string buildFlavor{};
    bool available{false};
};

/// @class EngineLauncher
/// @brief Discovers ANTERAY executables and launches projects/scenes in the Editor.
class EngineLauncher {
public:
    /// Discovers the ANTERAY executable and repository root.
    /// Search order:
    /// 1. `overrideExecutable` (if non-empty and valid executable)
    /// 2. Sibling `anteray` next to `hubExecutablePath`
    /// 3. Repository preset build directories (`build/editor/bin`, `build/release/bin`,
    ///    `build/debug/bin`, `build/profile/bin`) relative to `searchStartDir` /
    ///    `hubExecutablePath`
    /// 4. `PATH` lookup for `anteray`
    [[nodiscard]] static EngineInstallation
    discover(const std::filesystem::path& hubExecutablePath = {},
             const std::filesystem::path& overrideExecutable = {},
             const std::filesystem::path& searchStartDir = {});

    /// Finds the Hikari repository root by walking upward from `startDir` looking for
    /// `CMakeLists.txt` + `engine/` + `assets/`.
    [[nodiscard]] static std::filesystem::path
    findRepositoryRoot(const std::filesystem::path& startDir);

    /// Constructs verified CLI arguments for launching the Hikari Editor on a project.
    /// If `metadata.startupScene` is non-empty, passes `--scene <resolvedScenePath>`.
    [[nodiscard]] static std::vector<std::string>
    buildProjectLaunchArgs(const std::filesystem::path& projectDir,
                           const ProjectMetadata& metadata);

    /// Constructs verified CLI arguments for launching the Hikari Editor on a standalone scene.
    [[nodiscard]] static std::vector<std::string>
    buildSceneLaunchArgs(const std::filesystem::path& scenePath);

    /// Launches `executablePath` detached with `args` and `workingDir`.
    /// Returns the child process ID on success.
    [[nodiscard]] static Result<uint64_t>
    launchDetached(const std::filesystem::path& executablePath,
                   const std::vector<std::string>& args,
                   const std::filesystem::path& workingDir = {});
};

} // namespace eng::hub
