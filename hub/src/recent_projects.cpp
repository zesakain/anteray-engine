/// @file recent_projects.cpp
/// @brief Persistent recent-project registry implementation.

#include "hub/recent_projects.h"

#include "core/json.h"
#include "hub/project_metadata.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace eng::hub {

namespace {

std::filesystem::path normalizeProjectDir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::path absPath = std::filesystem::absolute(dir, ec);
    if (ec) {
        absPath = dir;
    }
    return absPath.lexically_normal();
}

bool pathsMatch(const std::filesystem::path& a, const std::filesystem::path& b) {
    const std::filesystem::path na = normalizeProjectDir(a);
    const std::filesystem::path nb = normalizeProjectDir(b);
    if (na == nb) {
        return true;
    }
    std::error_code ecA;
    std::error_code ecB;
    const std::filesystem::path ca = std::filesystem::weakly_canonical(na, ecA);
    const std::filesystem::path cb = std::filesystem::weakly_canonical(nb, ecB);
    if (!ecA && !ecB && ca == cb) {
        return true;
    }
    return false;
}

} // namespace

std::filesystem::path RecentProjectsStore::defaultStoragePath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "anteray" / "hub_recent_projects.json";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "anteray" / "hub_recent_projects.json";
    }
    return std::filesystem::current_path() / "hub_recent_projects.json";
}

std::filesystem::path RecentProjectsStore::legacyStoragePath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "hikari" / "hub_recent_projects.json";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "hikari" / "hub_recent_projects.json";
    }
    return std::filesystem::current_path() / "hub_recent_projects.json";
}

Result<void> RecentProjectsStore::load(const std::filesystem::path& storagePath) {
    entries_.clear();

    std::error_code ec;
    std::filesystem::path effectivePath = storagePath;
    if (!std::filesystem::exists(effectivePath, ec)) {
        // Non-destructive fallback: read legacy Hikari recents only when the ANTERAY file does
        // not yet exist.
        const std::filesystem::path legacyPath = legacyStoragePath();
        if (pathsMatch(storagePath, defaultStoragePath()) &&
            std::filesystem::exists(legacyPath, ec)) {
            effectivePath = legacyPath;
        } else {
            return Result<void>::success();
        }
    }

    std::ifstream file(effectivePath, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return Error{.code = ErrorCode::InvalidState,
                     .message = "Unable to open recent projects file"};
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    auto parseRes = core::JsonValue::parse(ss.str());
    if (!parseRes.hasValue()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Malformed recent projects JSON"};
    }

    const core::JsonValue& root = parseRes.value();
    if (!root.isObject()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Recent projects file root must be a JSON object"};
    }

    const core::JsonValue* itemsVal = root.find("recentProjects");
    if (itemsVal == nullptr || !itemsVal->isArray()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Recent projects file is missing 'recentProjects' array"};
    }

    for (const auto& item : itemsVal->asArray()) {
        if (!item.isObject()) {
            continue;
        }
        const std::string dirStr(item.getString("projectDirectory", ""));
        if (dirStr.empty()) {
            continue;
        }
        RecentProjectEntry entry{};
        entry.projectName = std::string(item.getString("projectName", ""));
        entry.projectDirectory = normalizeProjectDir(dirStr);
        entry.manifestPath = std::string(item.getString("manifestPath", ""));
        entry.lastOpenedTimestamp = item.getInt("lastOpenedTimestamp", 0);
        entry.engineVersion = std::string(item.getString("engineVersion", ""));

        const bool duplicate =
            std::any_of(entries_.begin(), entries_.end(), [&](const RecentProjectEntry& existing) {
                return pathsMatch(existing.projectDirectory, entry.projectDirectory);
            });
        if (!duplicate) {
            entries_.push_back(std::move(entry));
        }
    }

    refreshAvailability();
    return Result<void>::success();
}

Result<void> RecentProjectsStore::saveAtomic(const std::filesystem::path& storagePath) const {
    core::JsonValue root = core::JsonValue::makeObject();
    root.set("formatVersion", 1);

    core::JsonValue arr = core::JsonValue::makeArray();
    for (const auto& entry : entries_) {
        core::JsonValue obj = core::JsonValue::makeObject();
        obj.set("projectName", entry.projectName);
        obj.set("projectDirectory", entry.projectDirectory.string());
        obj.set("manifestPath", entry.manifestPath.string());
        obj.set("lastOpenedTimestamp", entry.lastOpenedTimestamp);
        obj.set("engineVersion", entry.engineVersion);
        arr.pushBack(std::move(obj));
    }
    root.set("recentProjects", std::move(arr));

    std::error_code ec;
    if (storagePath.has_parent_path()) {
        std::filesystem::create_directories(storagePath.parent_path(), ec);
        if (ec) {
            return Error{.code = ErrorCode::InvalidState,
                         .message = "Failed to create recent-projects directory"};
        }
    }

    const std::filesystem::path tempPath = storagePath.string() + ".tmp";
    {
        std::ofstream out(tempPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return Error{.code = ErrorCode::InvalidState,
                         .message = "Failed to open temporary recent-projects file"};
        }
        out << root.serialize(2) << "\n";
        out.flush();
        if (!out.good()) {
            out.close();
            std::filesystem::remove(tempPath, ec);
            return Error{.code = ErrorCode::InvalidState,
                         .message = "Failed while writing temporary recent-projects file"};
        }
    }

    std::filesystem::rename(tempPath, storagePath, ec);
    if (ec) {
        std::filesystem::remove(tempPath, ec);
        return Error{.code = ErrorCode::InvalidState,
                     .message = "Failed to save recent-projects file"};
    }
    return Result<void>::success();
}

void RecentProjectsStore::upsert(RecentProjectEntry entry) {
    entry.projectDirectory = normalizeProjectDir(entry.projectDirectory);
    if (entry.manifestPath.empty() && !entry.projectName.empty()) {
        entry.manifestPath =
            entry.projectDirectory / (entry.projectName + std::string(kProjectFileExtension));
    }

    std::error_code ec;
    entry.existsOnDisk = std::filesystem::exists(entry.projectDirectory, ec) &&
                         std::filesystem::is_directory(entry.projectDirectory, ec);

    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const RecentProjectEntry& existing) {
                                      return pathsMatch(existing.projectDirectory,
                                                        entry.projectDirectory);
                                  }),
                   entries_.end());

    entries_.insert(entries_.begin(), std::move(entry));
}

bool RecentProjectsStore::remove(const std::filesystem::path& projectDirectory) {
    const size_t before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const RecentProjectEntry& existing) {
                                      return pathsMatch(existing.projectDirectory,
                                                        projectDirectory);
                                  }),
                   entries_.end());
    return entries_.size() != before;
}

void RecentProjectsStore::refreshAvailability() {
    std::error_code ec;
    for (auto& entry : entries_) {
        entry.existsOnDisk = std::filesystem::exists(entry.projectDirectory, ec) &&
                             std::filesystem::is_directory(entry.projectDirectory, ec);
    }
}

} // namespace eng::hub
