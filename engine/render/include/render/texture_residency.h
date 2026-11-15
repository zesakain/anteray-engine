/// @file texture_residency.h
/// @brief Texture residency lifecycle states, memory budgeting, and telemetry definitions.
///
/// Purpose:
/// Defines the explicit state machine for texture asset residency, hardware-aware VRAM memory
/// budget configurations, and real-time telemetry metrics for Milestone 11F Phase 5.
///
/// Contract:
/// - Residency states follow a strictly validated unidirectional and bidirectional transition
/// graph.
/// - Memory footprint calculations represent conservative std140 / GPU alignment approximations.
/// - No runtime heap allocation inside memory metric queries or calculation helpers.
///
/// Ownership and Lifetime:
/// Owned and queried by AssetManager. Outlives per-frame rendering passes.
///
/// Thread-safety:
/// Read methods and estimation functions are const / pure; state mutations are serialized
/// on the main render thread.

#pragma once

#include "rhi/texture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace eng::render {

/// @enum TextureResidencyState
/// @brief Explicit lifecycle states of a managed texture asset.
enum class TextureResidencyState : uint8_t {
    Unloaded = 0,         ///< Registered metadata only; no CPU bitmap or GPU memory allocated.
    Loading = 1,          ///< Synchronous or asynchronous decode in progress.
    CpuResident = 2,      ///< Host bitmap pixels resident in CPU memory; not uploaded to GPU.
    GpuUploadPending = 3, ///< Host bitmap queued for Vulkan staging and transfer.
    GpuResident = 4,      ///< Vulkan VkImage, memory, and view allocated and sampled on GPU.
    EvictionPending = 5,  ///< Marked for reclaim under memory pressure; awaiting GPU retirement.
    Failed = 6,           ///< Read or allocation failure; safely bound to fallback texture.
};

/// @brief Returns human-readable string representation of a residency state.
[[nodiscard]] constexpr const char* toString(TextureResidencyState state) noexcept {
    switch (state) {
    case TextureResidencyState::Unloaded:
        return "Unloaded";
    case TextureResidencyState::Loading:
        return "Loading";
    case TextureResidencyState::CpuResident:
        return "CpuResident";
    case TextureResidencyState::GpuUploadPending:
        return "GpuUploadPending";
    case TextureResidencyState::GpuResident:
        return "GpuResident";
    case TextureResidencyState::EvictionPending:
        return "EvictionPending";
    case TextureResidencyState::Failed:
        return "Failed";
    }
    return "Unknown";
}

/// @enum TextureFailureStage
/// @brief Pipeline stage where texture loading or allocation failed.
enum class TextureFailureStage : uint8_t {
    None = 0,
    FileRead,
    Decode,
    GpuAllocation,
    GpuUpload,
    EvictionRetirement,
};

/// @brief Returns human-readable string representation of a failure stage.
[[nodiscard]] constexpr const char* toString(TextureFailureStage stage) noexcept {
    switch (stage) {
    case TextureFailureStage::None:
        return "None";
    case TextureFailureStage::FileRead:
        return "FileRead";
    case TextureFailureStage::Decode:
        return "Decode";
    case TextureFailureStage::GpuAllocation:
        return "GpuAllocation";
    case TextureFailureStage::GpuUpload:
        return "GpuUpload";
    case TextureFailureStage::EvictionRetirement:
        return "EvictionRetirement";
    }
    return "Unknown";
}

/// @enum TextureFailureReason
/// @brief Root cause classification for texture load or allocation failure.
enum class TextureFailureReason : uint8_t {
    None = 0,
    FileNotFound,
    InvalidHeader,
    CorruptData,
    UnsupportedFormat,
    OutOfMemoryCpu,
    OutOfMemoryGpu,
    DeviceLost,
    SubmissionFailed,
};

/// @brief Returns human-readable string representation of a failure reason.
[[nodiscard]] constexpr const char* toString(TextureFailureReason reason) noexcept {
    switch (reason) {
    case TextureFailureReason::None:
        return "None";
    case TextureFailureReason::FileNotFound:
        return "FileNotFound";
    case TextureFailureReason::InvalidHeader:
        return "InvalidHeader";
    case TextureFailureReason::CorruptData:
        return "CorruptData";
    case TextureFailureReason::UnsupportedFormat:
        return "UnsupportedFormat";
    case TextureFailureReason::OutOfMemoryCpu:
        return "OutOfMemoryCpu";
    case TextureFailureReason::OutOfMemoryGpu:
        return "OutOfMemoryGpu";
    case TextureFailureReason::DeviceLost:
        return "DeviceLost";
    case TextureFailureReason::SubmissionFailed:
        return "SubmissionFailed";
    }
    return "Unknown";
}

/// @struct TextureFailureInfo
/// @brief Structured diagnostics for texture failures with bounded retry tracking.
struct TextureFailureInfo {
    TextureFailureStage stage{TextureFailureStage::None};
    TextureFailureReason reason{TextureFailureReason::None};
    uint32_t retryCount{0};
    static constexpr uint32_t kMaxRetries = 3;

    [[nodiscard]] constexpr bool canRetry() const noexcept {
        return retryCount < kMaxRetries && reason != TextureFailureReason::UnsupportedFormat &&
               reason != TextureFailureReason::FileNotFound;
    }
};

/// @brief Validates whether a state transition adheres to the formal residency transition graph.
[[nodiscard]] constexpr bool isValidResidencyTransition(TextureResidencyState from,
                                                        TextureResidencyState to) noexcept {
    if (from == to) {
        return true;
    }
    switch (from) {
    case TextureResidencyState::Unloaded:
        return to == TextureResidencyState::Loading || to == TextureResidencyState::Failed;
    case TextureResidencyState::Loading:
        return to == TextureResidencyState::CpuResident ||
               to == TextureResidencyState::Unloaded || // cancelled
               to == TextureResidencyState::Failed;
    case TextureResidencyState::CpuResident:
        return to == TextureResidencyState::GpuUploadPending ||
               to == TextureResidencyState::Unloaded || // evicted from host memory
               to == TextureResidencyState::Failed;
    case TextureResidencyState::GpuUploadPending:
        return to == TextureResidencyState::GpuResident ||
               to == TextureResidencyState::CpuResident || // upload cancelled
               to == TextureResidencyState::Failed;
    case TextureResidencyState::GpuResident:
        return to == TextureResidencyState::EvictionPending;
    case TextureResidencyState::EvictionPending:
        return to == TextureResidencyState::Unloaded ||    // retired: host bitmap discarded
               to == TextureResidencyState::CpuResident || // retired: host bitmap preserved
               to == TextureResidencyState::Failed;        // retirement error / device lost
    case TextureResidencyState::Failed:
        return to == TextureResidencyState::Unloaded || // reset
               to == TextureResidencyState::Loading;    // retry
    }
    return false;
}

/// @struct TextureBudgetConfig
/// @brief Configurable texture memory budgets and thresholds.
struct TextureBudgetConfig {
    /// Maximum GPU texture memory before eviction is triggered (default: 1024 MB).
    size_t maxGpuMemoryBytes{1024ULL * 1024ULL * 1024ULL};

    /// Target GPU texture memory to recover down to during eviction (default: 800 MB).
    size_t targetGpuMemoryBytes{800ULL * 1024ULL * 1024ULL};

    /// Maximum host CPU memory for decoded bitmaps (default: 512 MB).
    size_t maxCpuMemoryBytes{512ULL * 1024ULL * 1024ULL};

    /// Memory pressure assertion threshold ratio (default: 0.90 = 90% of max budget).
    float memoryPressureThreshold{0.90F};

    /// Maximum resident texture count limit (default: 4096).
    uint32_t maxResidentTextures{4096};

    /// Creates Tier 0 baseline budget configuration (512 MB VRAM / 256 MB RAM).
    [[nodiscard]] static constexpr TextureBudgetConfig createTier0Baseline() noexcept {
        return TextureBudgetConfig{
            .maxGpuMemoryBytes = 512ULL * 1024ULL * 1024ULL,
            .targetGpuMemoryBytes = 400ULL * 1024ULL * 1024ULL,
            .maxCpuMemoryBytes = 256ULL * 1024ULL * 1024ULL,
            .memoryPressureThreshold = 0.85F,
            .maxResidentTextures = 2048,
        };
    }
};

/// @struct TextureMemoryMetrics
/// @brief Observable real-time telemetry tracking texture memory residency and pressure.
struct TextureMemoryMetrics {
    size_t residentGpuMemoryBytes{0};       ///< Total resident GPU memory footprint in bytes.
    size_t pendingUploadBytes{0};           ///< Memory queued for GPU staging and upload.
    size_t retiredGpuMemoryBytes{0};        ///< Memory pending deferred GPU retirement.
    size_t pinnedBytes{0};                  ///< Memory footprint held by pinned textures.
    size_t estimatedTotalGpuMemoryBytes{0}; ///< Sum of resident + pending + retired allocations.
    size_t estimatedGpuMemoryBytes{0};   ///< Backward-compatible alias for residentGpuMemoryBytes.
    size_t estimatedCpuMemoryBytes{0};   ///< Total host CPU memory footprint in bytes.
    uint32_t residentTextureCount{0};    ///< Number of textures currently in GpuResident state.
    uint32_t pendingRetirementsCount{0}; ///< Number of textures in EvictionPending state.
    uint32_t pinnedTextureCount{0};      ///< Number of textures flagged as pinned.
    uint32_t residentMipLevelsTotal{0};  ///< Total resident mip levels across all GPU textures.
    size_t uploadBytesLifetime{0};       ///< Cumulative upload throughput in bytes.
    uint32_t evictionCountLifetime{0};   ///< Total number of textures evicted across session.
    uint32_t failedAllocationsLifetime{0}; ///< Total failed load or allocation attempts.
    float budgetUsageRatio{0.0F};      ///< Current ratio of total GPU memory to max budget [0..1+].
    bool isUnderMemoryPressure{false}; ///< True if budgetUsageRatio >= memoryPressureThreshold.
};

/// @brief Computes conservative estimated GPU memory footprint in bytes for a texture.
///
/// Takes into account width, height, format, and full or partial mipmap chains (approx 4/3 factor).
///
/// @param width Base level width in pixels.
/// @param height Base level height in pixels.
/// @param mipLevels Total resident mip levels.
/// @param format Texture pixel format.
/// @return Total estimated memory in bytes.
[[nodiscard]] constexpr size_t estimateTextureMemoryBytes(uint32_t width, uint32_t height,
                                                          uint32_t mipLevels,
                                                          rhi::TextureFormat format) noexcept {
    if (width == 0 || height == 0 || mipLevels == 0) {
        return 0;
    }

    size_t bytesPerPixel = 4;
    switch (format) {
    case rhi::TextureFormat::R8_UNORM:
        bytesPerPixel = 1;
        break;
    case rhi::TextureFormat::RGBA8_UNORM:
    case rhi::TextureFormat::RGBA8_SRGB:
    case rhi::TextureFormat::BGRA8_UNORM:
    case rhi::TextureFormat::BGRA8_SRGB:
        bytesPerPixel = 4;
        break;
    case rhi::TextureFormat::RGBA16_FLOAT:
    case rhi::TextureFormat::RGBA16_UNORM:
        bytesPerPixel = 8;
        break;
    case rhi::TextureFormat::R32_FLOAT:
        bytesPerPixel = 4;
        break;
    case rhi::TextureFormat::RG32_FLOAT:
        bytesPerPixel = 8;
        break;
    case rhi::TextureFormat::RGB32_FLOAT:
        bytesPerPixel = 12;
        break;
    case rhi::TextureFormat::RGBA32_FLOAT:
        bytesPerPixel = 16;
        break;
    case rhi::TextureFormat::D32_FLOAT:
        bytesPerPixel = 4;
        break;
    case rhi::TextureFormat::D24_UNORM_S8_UINT:
    case rhi::TextureFormat::D32_FLOAT_S8_UINT:
        bytesPerPixel = 4;
        break;
    default:
        bytesPerPixel = 4;
        break;
    }

    size_t totalBytes = 0;
    uint32_t curW = width;
    uint32_t curH = height;
    for (uint32_t m = 0; m < mipLevels; ++m) {
        totalBytes += static_cast<size_t>(curW) * static_cast<size_t>(curH) * bytesPerPixel;
        curW = std::max(1U, curW / 2U);
        curH = std::max(1U, curH / 2U);
    }
    return totalBytes;
}

} // namespace eng::render
