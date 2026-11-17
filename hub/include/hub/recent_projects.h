/// @file recent_projects.h
/// @brief Persistent recent-project registry with atomic writes and missing-directory tolerance.
///
/// Purpose:
/// Tracks recently opened or created Hikari projects across Hub restarts without duplicating
/// entries and without silently deleting missing or moved project directories.

#pragma once

#include "core/result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace eng::hub {

/// @struct RecentProjectEntry
/// @brief Persisted metadata for a single recently accessed project.
struct RecentProjectEntry {
    std::string projectName{};
    std::filesystem::path projectDirectory{};
    std::filesystem::path manifestPath{};
    int64_t lastOpenedTimestamp{0};
    std::string engineVersion{};
    bool existsOnDisk{false};
};

/// @class RecentProjectsStore
/// @brief Manages loading, deduplication, availability checking, and atomic saving of recents.
class RecentProjectsStore {
public:
    /// Resolves the default user-config path for `hub_recent_projects.json` (`~/.config/anteray/`).
    [[nodiscard]] static std::filesystem::path defaultStoragePath();

    /// Resolves the legacy Hikari user-config path (`~/.config/hikari/hub_recent_projects.json`).
    [[nodiscard]] static std::filesystem::path legacyStoragePath();

    /// Loads recent projects from `storagePath`. Missing file is treated as an empty list.
    [[nodiscard]] Result<void> load(const std::filesystem::path& storagePath);

    /// Atomically persists the recent-projects list to `storagePath` via temp file + rename.
    [[nodiscard]] Result<void> saveAtomic(const std::filesystem::path& storagePath) const;

    /// Inserts or updates an entry, deduplicating by normalized project directory and moving it to
    /// the front of the list.
    void upsert(RecentProjectEntry entry);

    /// Removes an entry matching `projectDirectory` from the list without touching files on disk.
    bool remove(const std::filesystem::path& projectDirectory);

    /// Updates `existsOnDisk` for every entry without removing missing/moved projects.
    void refreshAvailability();

    /// Returns the ordered list of recent projects (most recent first).
    [[nodiscard]] const std::vector<RecentProjectEntry>& entries() const noexcept {
        return entries_;
    }

    /// Clears all in-memory entries.
    void clear() noexcept { entries_.clear(); }

private:
    std::vector<RecentProjectEntry> entries_{};
};

} // namespace eng::hub
