/// @file sample_catalog.cpp
/// @brief Sample and benchmark scene discovery implementation.

#include "hub/sample_catalog.h"

#include <cstdio>
#include <system_error>

namespace eng::hub {

namespace {

uint64_t measurePathBytes(const std::filesystem::path& target) {
    std::error_code ec;
    if (!std::filesystem::exists(target, ec)) {
        return 0;
    }
    if (std::filesystem::is_regular_file(target, ec)) {
        const auto sz = std::filesystem::file_size(target, ec);
        return ec ? 0 : static_cast<uint64_t>(sz);
    }
    if (!std::filesystem::is_directory(target, ec)) {
        return 0;
    }

    uint64_t total = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(target, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file(ec)) {
            const auto sz = entry.file_size(ec);
            if (!ec) {
                total += static_cast<uint64_t>(sz);
            }
        }
    }
    return total;
}

SampleEntry inspectSample(const std::filesystem::path& repoRoot, std::string id, std::string name,
                          std::string purpose, std::string sceneRel,
                          std::vector<std::string> assetRels, std::string limitations) {
    SampleEntry entry{};
    entry.id = std::move(id);
    entry.name = std::move(name);
    entry.purpose = std::move(purpose);
    entry.sceneRelativePath = std::move(sceneRel);
    entry.requiredAssetPaths = std::move(assetRels);
    entry.knownLimitations = std::move(limitations);

    std::error_code ec;
    const std::filesystem::path sceneAbs = repoRoot / entry.sceneRelativePath;
    entry.sceneExists =
        std::filesystem::exists(sceneAbs, ec) && std::filesystem::is_regular_file(sceneAbs, ec);
    entry.measuredDiskBytes += measurePathBytes(sceneAbs);

    bool allAssetsPresent = !entry.requiredAssetPaths.empty();
    for (const auto& assetRel : entry.requiredAssetPaths) {
        const std::filesystem::path assetAbs = repoRoot / assetRel;
        const bool exists = std::filesystem::exists(assetAbs, ec);
        if (!exists) {
            allAssetsPresent = false;
        } else {
            entry.measuredDiskBytes += measurePathBytes(assetAbs);
        }
    }
    entry.assetsExist = allAssetsPresent;
    entry.canOpenInEditor = entry.sceneExists && entry.assetsExist;
    return entry;
}

} // namespace

std::vector<SampleEntry>
SampleCatalog::discoverSamples(const std::filesystem::path& repositoryRoot) {
    std::vector<SampleEntry> samples;
    samples.reserve(4);

    samples.push_back(inspectSample(
        repositoryRoot, "sponza", "Sponza",
        "Architectural courtyard PBR, directional shadows, GI, and tone-mapping benchmark.",
        "assets/benchmark-scenes/Sponza_Benchmark.hikari", {"test-assets/benchmark/sponza"},
        "Requires repository working directory for relative test-assets/benchmark/sponza/ paths."));

    samples.push_back(inspectSample(
        repositoryRoot, "damaged_helmet", "DamagedHelmet",
        "Single-hero PBR metal/roughness, emissive, and IBL specular validation scene.",
        "assets/benchmark-scenes/DamagedHelmet_Benchmark.hikari", {"test-assets/DamagedHelmet.glb"},
        "Scene manifest references relative texture paths that may report missing external "
        "textures in the async loader modal while mesh geometry loads from GLB."));

    samples.push_back(inspectSample(
        repositoryRoot, "bistro", "Bistro",
        "Large-scale multi-material urban scene stress test (338 textures, async streaming).",
        "assets/benchmark-scenes/Bistro_Benchmark.hikari", {"test-assets/benchmark/bistro"},
        "Large ~365 MB dataset; initial async decode and GPU upload take several seconds and "
        "require substantial VRAM/RAM."));

    samples.push_back(inspectSample(
        repositoryRoot, "material_lab", "MaterialLab",
        "PBR material calibration grid (metals, dielectrics, clearcoat, sheen, transmission).",
        "assets/benchmark-scenes/MaterialLab_Benchmark.hikari",
        {"assets/benchmark-scenes/material_lab.gltf", "assets/benchmark-scenes/material_lab.bin"},
        "Designed as a static lighting/material calibration chart rather than an interactive "
        "level."));

    return samples;
}

std::string SampleCatalog::formatByteSize(uint64_t bytes) {
    char buf[64]{};
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.2f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return std::string(buf);
}

} // namespace eng::hub
