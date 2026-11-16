/// @file project_validator.h
/// @brief Project structure, manifest, and scene-reference validation for Hikari Machine Hub.
///
/// Purpose:
/// Inspects a project directory without modifying or silently rewriting malformed manifests,
/// distinguishing Valid, ValidWithWarnings, and Invalid projects with actionable diagnostics.

#pragma once

#include "hub/project_metadata.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace eng::hub {

/// @enum ProjectValidationStatus
/// @brief Overall health classification of a Hikari project.
enum class ProjectValidationStatus : uint8_t {
    Valid = 0,
    ValidWithWarnings = 1,
    Invalid = 2,
};

/// @enum ValidationIssueSeverity
/// @brief Severity level of an individual validation finding.
enum class ValidationIssueSeverity : uint8_t {
    Warning = 0,
    Error = 1,
};

/// @struct ValidationIssue
/// @brief Actionable diagnostic produced during project validation.
struct ValidationIssue {
    ValidationIssueSeverity severity{ValidationIssueSeverity::Error};
    std::string code{};
    std::string message{};
    std::string path{};
};

/// @struct ProjectValidationReport
/// @brief Complete validation outcome for a project directory or manifest.
struct ProjectValidationReport {
    ProjectValidationStatus status{ProjectValidationStatus::Invalid};
    std::filesystem::path projectDirectory{};
    std::filesystem::path manifestPath{};
    std::optional<ProjectMetadata> metadata{};
    std::vector<ValidationIssue> issues{};

    [[nodiscard]] bool isValid() const noexcept {
        return status != ProjectValidationStatus::Invalid;
    }

    [[nodiscard]] bool hasWarnings() const noexcept {
        return status == ProjectValidationStatus::ValidWithWarnings;
    }
};

/// Validates a project directory and its `.hikari-project` manifest.
/// Never modifies or silently repairs files on disk.
[[nodiscard]] ProjectValidationReport
validateProjectDirectory(const std::filesystem::path& projectDir,
                         std::string_view currentEngineVersion = kCurrentEngineVersion);

} // namespace eng::hub
