/// @file project_validator.cpp
/// @brief Project structure and manifest validation implementation.

#include "hub/project_validator.h"

#include <array>
#include <system_error>

namespace eng::hub {

namespace {

void addIssue(ProjectValidationReport& report, ValidationIssueSeverity severity, std::string code,
              std::string message, std::string path = {}) {
    report.issues.push_back(ValidationIssue{
        .severity = severity,
        .code = std::move(code),
        .message = std::move(message),
        .path = std::move(path),
    });
}

} // namespace

ProjectValidationReport validateProjectDirectory(const std::filesystem::path& projectDir,
                                                 std::string_view currentEngineVersion) {
    ProjectValidationReport report{};
    report.projectDirectory = projectDir;

    std::error_code ec;
    if (projectDir.empty() || !std::filesystem::exists(projectDir, ec) ||
        !std::filesystem::is_directory(projectDir, ec)) {
        addIssue(report, ValidationIssueSeverity::Error, "PROJECT_DIR_MISSING",
                 "Project directory does not exist or is not accessible.", projectDir.string());
        report.status = ProjectValidationStatus::Invalid;
        return report;
    }

    auto manifestRes = findProjectManifestInDirectory(projectDir);
    if (!manifestRes.hasValue()) {
        addIssue(report, ValidationIssueSeverity::Error, "MANIFEST_MISSING",
                 std::string(manifestRes.error().message), projectDir.string());
        report.status = ProjectValidationStatus::Invalid;
        return report;
    }

    report.manifestPath = manifestRes.value();

    auto metaRes = loadProjectMetadataFile(report.manifestPath);
    if (!metaRes.hasValue()) {
        addIssue(report, ValidationIssueSeverity::Error, "MANIFEST_INVALID",
                 std::string(metaRes.error().message), report.manifestPath.string());
        report.status = ProjectValidationStatus::Invalid;
        return report;
    }

    report.metadata = metaRes.value();
    const ProjectMetadata& meta = *report.metadata;

    // Engine compatibility check
    if (meta.engineVersion.empty()) {
        addIssue(report, ValidationIssueSeverity::Warning, "ENGINE_VERSION_UNSPECIFIED",
                 "Manifest does not specify an engineVersion compatibility target.");
    } else if (!currentEngineVersion.empty() && meta.engineVersion != currentEngineVersion) {
        addIssue(report, ValidationIssueSeverity::Warning, "ENGINE_VERSION_MISMATCH",
                 "Project targets engineVersion '" + meta.engineVersion +
                     "' while detected Hub/Engine version is '" +
                     std::string(currentEngineVersion) + "'.");
    }

    // Standard directory structure check
    static constexpr std::array<std::string_view, 4> kRecommendedDirs{
        "Assets",
        "Scenes",
        "Scripts",
        "Config",
    };
    for (std::string_view dirName : kRecommendedDirs) {
        const std::filesystem::path dirPath = projectDir / dirName;
        if (!std::filesystem::exists(dirPath, ec) || !std::filesystem::is_directory(dirPath, ec)) {
            addIssue(report, ValidationIssueSeverity::Warning, "STANDARD_DIR_MISSING",
                     "Recommended project directory '" + std::string(dirName) + "' is missing.",
                     dirPath.string());
        }
    }

    // Declared assetDirectories check
    for (const auto& assetDir : meta.assetDirectories) {
        if (!isSafeProjectRelativePath(assetDir)) {
            addIssue(report, ValidationIssueSeverity::Error, "ASSET_DIR_UNSAFE",
                     "Asset directory path is unsafe or escapes the project root: " + assetDir,
                     assetDir);
            continue;
        }
        const std::filesystem::path fullDir = projectDir / assetDir;
        if (!std::filesystem::exists(fullDir, ec) || !std::filesystem::is_directory(fullDir, ec)) {
            addIssue(report, ValidationIssueSeverity::Error, "ASSET_DIR_MISSING",
                     "Declared asset directory does not exist: " + assetDir, fullDir.string());
        }
    }

    // Startup scene validation when configured
    if (!meta.startupScene.empty()) {
        if (!isSafeProjectRelativePath(meta.startupScene)) {
            addIssue(report, ValidationIssueSeverity::Error, "STARTUP_SCENE_UNSAFE",
                     "startupScene path escapes project root: " + meta.startupScene,
                     meta.startupScene);
        } else {
            const std::filesystem::path scenePath = projectDir / meta.startupScene;
            if (!std::filesystem::exists(scenePath, ec) ||
                !std::filesystem::is_regular_file(scenePath, ec)) {
                addIssue(report, ValidationIssueSeverity::Error, "STARTUP_SCENE_MISSING",
                         "Configured startupScene file does not exist: " + meta.startupScene,
                         scenePath.string());
            }
        }
    }

    bool hasError = false;
    bool hasWarning = false;
    for (const auto& issue : report.issues) {
        if (issue.severity == ValidationIssueSeverity::Error) {
            hasError = true;
        } else {
            hasWarning = true;
        }
    }

    if (hasError) {
        report.status = ProjectValidationStatus::Invalid;
    } else if (hasWarning) {
        report.status = ProjectValidationStatus::ValidWithWarnings;
    } else {
        report.status = ProjectValidationStatus::Valid;
    }

    return report;
}

} // namespace eng::hub
