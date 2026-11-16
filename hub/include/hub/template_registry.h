/// @file template_registry.h
/// @brief Project template definitions and safe project-creation workflow for Hikari Machine Hub.
///
/// Purpose:
/// Provides built-in project templates with honest capability/requirement declarations and an
/// atomic-rollback creation flow that prevents partial projects and accidental overwrites.

#pragma once

#include "core/result.h"
#include "hub/project_metadata.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace eng::hub {

/// @struct ProjectTemplateDesc
/// @brief Declarative specification of a built-in Hikari project template.
struct ProjectTemplateDesc {
    std::string id{};
    std::string name{};
    std::string category{};
    std::string description{};
    std::string defaultStartupScene{};
    std::vector<std::string> capabilities{};
    std::vector<std::string> requirements{};
};

/// @struct ProjectCreateRequest
/// @brief Parameters for instantiating a new Hikari project on disk.
struct ProjectCreateRequest {
    std::string projectName{};
    std::filesystem::path parentDirectory{};
    std::string templateId{"empty"};
    std::filesystem::path repositoryRoot{};
};

/// @struct ProjectCreateResult
/// @brief Output metadata and paths produced by successful project creation.
struct ProjectCreateResult {
    std::filesystem::path projectDirectory{};
    std::filesystem::path manifestPath{};
    ProjectMetadata metadata{};
    std::vector<std::filesystem::path> createdPaths{};
};

/// Validates a template definition before instantiation.
[[nodiscard]] Result<void> validateTemplateDesc(const ProjectTemplateDesc& desc);

/// @class TemplateRegistry
/// @brief Registry of built-in MVP project templates and project-instantiation logic.
class TemplateRegistry {
public:
    /// Returns all built-in MVP templates.
    [[nodiscard]] static const std::vector<ProjectTemplateDesc>& builtinTemplates();

    /// Finds a built-in template by its unique ID.
    [[nodiscard]] static const ProjectTemplateDesc* findById(std::string_view id);

    /// Creates a new project directory from a template specification.
    /// Rejects invalid names, missing parent directories, and pre-existing target directories.
    /// Cleans up partially created files if any write fails.
    [[nodiscard]] static Result<ProjectCreateResult>
    createProject(const ProjectCreateRequest& request, const ProjectTemplateDesc& tmpl);

    /// Convenience overload looking up `request.templateId` from `builtinTemplates()`.
    [[nodiscard]] static Result<ProjectCreateResult>
    createProjectFromBuiltin(const ProjectCreateRequest& request);
};

} // namespace eng::hub
