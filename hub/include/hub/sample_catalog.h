/// @file sample_catalog.h
/// @brief Read-only discovery and cataloging of repository benchmark/sample scenes.
///
/// Purpose:
/// Exposes existing benchmark/sample scenes (Sponza, DamagedHelmet, Bistro, MaterialLab)
/// without duplicating large asset directories into user projects.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace eng::hub {

/// @struct SampleEntry
/// @brief Metadata and availability state for a repository sample/benchmark scene.
struct SampleEntry {
    std::string id{};
    std::string name{};
    std::string purpose{};
    std::string sceneRelativePath{};
    std::vector<std::string> requiredAssetPaths{};
    std::string knownLimitations{};
    uint64_t measuredDiskBytes{0};
    bool sceneExists{false};
    bool assetsExist{false};
    bool canOpenInEditor{false};
};

/// @class SampleCatalog
/// @brief Inspects repository sample and benchmark scenes on disk.
class SampleCatalog {
public:
    /// Discovers and measures available samples relative to `repositoryRoot`.
    [[nodiscard]] static std::vector<SampleEntry>
    discoverSamples(const std::filesystem::path& repositoryRoot);

    /// Formats a byte count into a human-readable string (e.g. "51.2 MB").
    [[nodiscard]] static std::string formatByteSize(uint64_t bytes);
};

} // namespace eng::hub
