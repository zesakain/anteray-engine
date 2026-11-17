/// @file project_metadata.h
/// @brief Versioned Hikari project metadata schema, serialization, and validation.
///
/// Purpose:
/// Defines the `.hikari-project` JSON manifest format used by Hikari Machine Hub
/// to describe, validate, and launch standalone Hikari projects without coupling
/// project management into the core engine runtime.
///
/// Ownership and Lifetime:
/// ProjectMetadata is an owning value type.
///
/// Thread-safety:
/// Pure value types and stateless functions; safe for concurrent use on distinct inputs.

#pragma once

#include "core/result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace eng::hub {

/// Current supported `.hikari-project` format version.
inline constexpr uint32_t kCurrentProjectFormatVersion = 1U;

/// Current engine version string advertised by this Hub build.
inline constexpr std::string_view kCurrentEngineVersion = "0.1.0";

/// Standard file extension for ANTERAY project manifests.
inline constexpr std::string_view kProjectFileExtension = ".anteray-project";

/// Legacy Hikari project manifest extension kept readable for backward compatibility.
inline constexpr std::string_view kLegacyProjectFileExtension = ".hikari-project";

/// Returns true if `ext` is either `.anteray-project` or legacy `.hikari-project`.
[[nodiscard]] constexpr bool isSupportedProjectManifestExtension(std::string_view ext) noexcept {
    return ext == kProjectFileExtension || ext == kLegacyProjectFileExtension;
}

/// @struct ProjectMetadata
/// @brief Deserialized representation of a `.hikari-project` manifest.
struct ProjectMetadata {
    uint32_t formatVersion{kCurrentProjectFormatVersion};
    std::string projectName{};
    std::string engineVersion{std::string(kCurrentEngineVersion)};
    std::string startupScene{};
    std::vector<std::string> assetDirectories{"Assets", "Scenes", "Scripts", "Config"};
    std::string templateOrigin{};
    int64_t createdTimestamp{0};
    int64_t lastModifiedTimestamp{0};
};

/// Validates a human-entered project name for filesystem and manifest safety.
[[nodiscard]] Result<void> validateProjectName(std::string_view name);

/// Returns true if a relative path stays strictly inside the project root (no absolute paths or
/// parent traversal `..`).
[[nodiscard]] bool isSafeProjectRelativePath(std::string_view relativePath);

/// Serializes ProjectMetadata to formatted JSON text.
[[nodiscard]] Result<std::string> serializeProjectMetadata(const ProjectMetadata& metadata);

/// Parses and validates ProjectMetadata from JSON text.
/// Rejects unsupported format versions, malformed schemas, and unsafe paths without silent
/// rewriting.
[[nodiscard]] Result<ProjectMetadata> parseProjectMetadata(std::string_view jsonText);

/// Loads and validates a `.hikari-project` file from disk.
[[nodiscard]] Result<ProjectMetadata>
loadProjectMetadataFile(const std::filesystem::path& manifestPath);

/// Writes a `.hikari-project` file to disk atomically (temporary file + rename).
[[nodiscard]] Result<void> saveProjectMetadataFile(const std::filesystem::path& manifestPath,
                                                   const ProjectMetadata& metadata);

/// Finds the `.hikari-project` manifest inside a project root directory.
/// Returns an error if zero or multiple `.hikari-project` files exist.
[[nodiscard]] Result<std::filesystem::path>
findProjectManifestInDirectory(const std::filesystem::path& projectDir);

} // namespace eng::hub
