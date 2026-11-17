/// @file project_metadata.cpp
/// @brief Versioned Hikari project metadata serialization and validation implementation.

#include "hub/project_metadata.h"

#include "core/json.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace eng::hub {

namespace {

bool isForbiddenNameChar(char ch) noexcept {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (uch < 32U || uch == 127U) {
        return true;
    }
    switch (ch) {
    case '/':
    case '\\':
    case ':':
    case '*':
    case '?':
    case '"':
    case '<':
    case '>':
    case '|':
        return true;
    default:
        return false;
    }
}

Result<std::string> readTextFile(const std::filesystem::path& filePath) {
    std::ifstream file(filePath, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return Error{.code = ErrorCode::InvalidState,
                     .message = "Unable to open project manifest file"};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    if (file.bad()) {
        return Error{.code = ErrorCode::InvalidState,
                     .message = "Failed while reading project manifest file"};
    }
    return ss.str();
}

} // namespace

Result<void> validateProjectName(std::string_view name) {
    if (name.empty()) {
        return Error{.code = ErrorCode::InvalidArgument, .message = "Project name cannot be empty"};
    }
    if (name.size() > 128U) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Project name must be 128 characters or fewer"};
    }
    const bool allWhitespace =
        std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c) != 0; });
    if (allWhitespace) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Project name cannot be whitespace only"};
    }
    if (name == "." || name == "..") {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Project name cannot be '.' or '..'"};
    }
    if (std::isspace(static_cast<unsigned char>(name.front())) != 0 ||
        std::isspace(static_cast<unsigned char>(name.back())) != 0) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Project name cannot begin or end with whitespace"};
    }
    if (name.back() == '.') {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Project name cannot end with '.'"};
    }
    for (char ch : name) {
        if (isForbiddenNameChar(ch)) {
            return Error{.code = ErrorCode::InvalidArgument,
                         .message = "Project name contains invalid filesystem characters"};
        }
    }
    return Result<void>::success();
}

bool isSafeProjectRelativePath(std::string_view relativePath) {
    if (relativePath.empty()) {
        return false;
    }
    if (relativePath.front() == '/' || relativePath.front() == '\\') {
        return false;
    }
    if (relativePath.size() >= 2U &&
        std::isalpha(static_cast<unsigned char>(relativePath[0])) != 0 && relativePath[1] == ':') {
        return false;
    }
    const std::filesystem::path p(relativePath);
    if (p.is_absolute()) {
        return false;
    }
    for (const auto& part : p) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

Result<std::string> serializeProjectMetadata(const ProjectMetadata& metadata) {
    auto nameCheck = validateProjectName(metadata.projectName);
    if (!nameCheck.hasValue()) {
        return nameCheck.error();
    }
    if (metadata.formatVersion != kCurrentProjectFormatVersion) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Unsupported project formatVersion for serialization"};
    }
    if (!metadata.startupScene.empty() && !isSafeProjectRelativePath(metadata.startupScene)) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "startupScene must be a safe project-relative path"};
    }
    for (const auto& dir : metadata.assetDirectories) {
        if (!isSafeProjectRelativePath(dir)) {
            return Error{.code = ErrorCode::InvalidArgument,
                         .message = "Asset directory must be a safe project-relative path"};
        }
    }

    core::JsonValue root = core::JsonValue::makeObject();
    root.set("formatVersion", static_cast<int64_t>(metadata.formatVersion));
    root.set("projectName", metadata.projectName);
    root.set("engineVersion", metadata.engineVersion);
    root.set("startupScene", metadata.startupScene);

    core::JsonValue dirs = core::JsonValue::makeArray();
    for (const auto& dir : metadata.assetDirectories) {
        dirs.pushBack(core::JsonValue(dir));
    }
    root.set("assetDirectories", std::move(dirs));
    root.set("templateOrigin", metadata.templateOrigin);
    root.set("createdTimestamp", metadata.createdTimestamp);
    root.set("lastModifiedTimestamp", metadata.lastModifiedTimestamp);

    return root.serialize(2) + "\n";
}

Result<ProjectMetadata> parseProjectMetadata(std::string_view jsonText) {
    auto parseRes = core::JsonValue::parse(jsonText);
    if (!parseRes.hasValue()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Malformed project manifest JSON"};
    }

    const core::JsonValue& root = parseRes.value();
    if (!root.isObject()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Project manifest root must be a JSON object"};
    }

    const core::JsonValue* versionVal = root.find("formatVersion");
    if (versionVal == nullptr || !versionVal->isNumber()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Project manifest is missing numeric 'formatVersion'"};
    }

    const int64_t rawVersion = versionVal->asInt(-1);
    if (rawVersion <= 0) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Project manifest 'formatVersion' must be a positive integer"};
    }
    if (static_cast<uint32_t>(rawVersion) != kCurrentProjectFormatVersion) {
        return Error{.code = ErrorCode::NotSupported,
                     .message =
                         "Unsupported project formatVersion (no automatic migration available)"};
    }

    const core::JsonValue* nameVal = root.find("projectName");
    if (nameVal == nullptr || !nameVal->isString()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Project manifest is missing string 'projectName'"};
    }

    auto nameCheck = validateProjectName(nameVal->asString());
    if (!nameCheck.hasValue()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Invalid 'projectName' in manifest"};
    }

    ProjectMetadata metadata{};
    metadata.formatVersion = static_cast<uint32_t>(rawVersion);
    metadata.projectName = std::string(nameVal->asString());
    metadata.engineVersion = std::string(root.getString("engineVersion", ""));
    metadata.startupScene = std::string(root.getString("startupScene", ""));

    if (!metadata.startupScene.empty() && !isSafeProjectRelativePath(metadata.startupScene)) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Manifest 'startupScene' must be project-relative without '..'"};
    }

    const core::JsonValue* dirsVal = root.find("assetDirectories");
    if (dirsVal != nullptr) {
        if (!dirsVal->isArray()) {
            return Error{.code = ErrorCode::InvalidArgument,
                         .message = "Manifest 'assetDirectories' must be an array of strings"};
        }
        metadata.assetDirectories.clear();
        for (const auto& entry : dirsVal->asArray()) {
            if (!entry.isString()) {
                return Error{.code = ErrorCode::InvalidArgument,
                             .message = "Manifest 'assetDirectories' contains a non-string entry"};
            }
            const std::string dir(entry.asString());
            if (!isSafeProjectRelativePath(dir)) {
                return Error{.code = ErrorCode::InvalidArgument,
                             .message = "Unsafe path in 'assetDirectories'"};
            }
            metadata.assetDirectories.push_back(dir);
        }
    }

    metadata.templateOrigin = std::string(root.getString("templateOrigin", ""));
    metadata.createdTimestamp = root.getInt("createdTimestamp", 0);
    metadata.lastModifiedTimestamp = root.getInt("lastModifiedTimestamp", 0);
    return metadata;
}

Result<ProjectMetadata> loadProjectMetadataFile(const std::filesystem::path& manifestPath) {
    std::error_code ec;
    if (!std::filesystem::exists(manifestPath, ec) ||
        !std::filesystem::is_regular_file(manifestPath, ec)) {
        return Error{.code = ErrorCode::InvalidState,
                     .message = "Project manifest file does not exist"};
    }
    auto textRes = readTextFile(manifestPath);
    if (!textRes.hasValue()) {
        return textRes.error();
    }
    return parseProjectMetadata(textRes.value());
}

Result<void> saveProjectMetadataFile(const std::filesystem::path& manifestPath,
                                     const ProjectMetadata& metadata) {
    auto jsonRes = serializeProjectMetadata(metadata);
    if (!jsonRes.hasValue()) {
        return jsonRes.error();
    }

    std::error_code ec;
    if (manifestPath.has_parent_path()) {
        std::filesystem::create_directories(manifestPath.parent_path(), ec);
        if (ec) {
            return Error{.code = ErrorCode::InvalidState,
                         .message = "Failed to create manifest parent directory"};
        }
    }

    const std::filesystem::path tempPath = manifestPath.string() + ".tmp";
    {
        std::ofstream out(tempPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return Error{.code = ErrorCode::InvalidState,
                         .message = "Failed to open temporary manifest for writing"};
        }
        out << jsonRes.value();
        out.flush();
        if (!out.good()) {
            out.close();
            std::filesystem::remove(tempPath, ec);
            return Error{.code = ErrorCode::InvalidState,
                         .message = "Failed while writing temporary manifest"};
        }
    }

    std::filesystem::rename(tempPath, manifestPath, ec);
    if (ec) {
        std::filesystem::remove(tempPath, ec);
        return Error{.code = ErrorCode::InvalidState,
                     .message = "Failed to finalize project manifest"};
    }

    return Result<void>::success();
}

Result<std::filesystem::path>
findProjectManifestInDirectory(const std::filesystem::path& projectDir) {
    std::error_code ec;
    if (!std::filesystem::exists(projectDir, ec) ||
        !std::filesystem::is_directory(projectDir, ec)) {
        return Error{.code = ErrorCode::InvalidState, .message = "Project directory not found"};
    }

    std::vector<std::filesystem::path> found;
    for (const auto& entry : std::filesystem::directory_iterator(projectDir, ec)) {
        if (ec) {
            return Error{.code = ErrorCode::InvalidState,
                         .message = "Failed to scan project directory"};
        }
        if (entry.is_regular_file(ec) &&
            isSupportedProjectManifestExtension(entry.path().extension().string())) {
            found.push_back(entry.path());
        }
    }

    if (found.empty()) {
        return Error{.code = ErrorCode::InvalidState,
                     .message =
                         "No .anteray-project or .hikari-project manifest found in directory"};
    }
    if (found.size() > 1U) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Multiple project manifests found in directory"};
    }
    return found.front();
}

} // namespace eng::hub
