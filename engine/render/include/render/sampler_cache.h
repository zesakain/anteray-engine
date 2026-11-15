/// @file sampler_cache.h
/// @brief Deduplicating resource cache for RHI texture samplers.
///
/// Purpose:
/// Prevents redundant GPU sampler creation by hashing and deduplicating compatible
/// rhi::SamplerDesc configurations.
///
/// Ownership and Lifetime:
/// Owned by AssetManager. Automatically destroys cached GPU samplers on clear()
/// or destruction when an RHI device is provided.
///
/// Thread-safety:
/// Main thread / render thread only. Const accessors are thread-safe.
///
/// Performance:
/// O(1) hash table lookup; zero allocations in steady state.
/// Drastically reduces GPU sampler allocations (e.g. from hundreds to < 10).

#pragma once

#include "core/error.h"
#include "core/result.h"
#include "rhi/rhi_device.h"
#include "rhi/sampler.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace eng::render {

/// @struct SamplerCacheKey
/// @brief Normalized, hashable state representation of rhi::SamplerDesc.
struct SamplerCacheKey {
    rhi::Filter minFilter{rhi::Filter::Linear};
    rhi::Filter magFilter{rhi::Filter::Linear};
    rhi::Filter mipmapMode{rhi::Filter::Linear};
    rhi::SamplerAddressMode addressU{rhi::SamplerAddressMode::Repeat};
    rhi::SamplerAddressMode addressV{rhi::SamplerAddressMode::Repeat};
    rhi::SamplerAddressMode addressW{rhi::SamplerAddressMode::Repeat};
    bool anisotropyEnable{false};
    float maxAnisotropy{1.0F};
    float minLod{0.0F};
    float maxLod{1000.0F};
    float mipLodBias{0.0F};
    bool compareEnable{false};
    rhi::CompareOp compareOp{rhi::CompareOp::Always};

    [[nodiscard]] static SamplerCacheKey fromDesc(const rhi::SamplerDesc& desc) noexcept {
        return SamplerCacheKey{
            .minFilter = desc.minFilter,
            .magFilter = desc.magFilter,
            .mipmapMode = desc.mipmapMode,
            .addressU = desc.addressU,
            .addressV = desc.addressV,
            .addressW = desc.addressW,
            .anisotropyEnable = desc.anisotropyEnable,
            .maxAnisotropy = desc.maxAnisotropy,
            .minLod = desc.minLod,
            .maxLod = desc.maxLod,
            .mipLodBias = desc.mipLodBias,
            .compareEnable = desc.compareEnable,
            .compareOp = desc.compareOp,
        };
    }

    [[nodiscard]] bool operator==(const SamplerCacheKey& other) const noexcept {
        return minFilter == other.minFilter && magFilter == other.magFilter &&
               mipmapMode == other.mipmapMode && addressU == other.addressU &&
               addressV == other.addressV && addressW == other.addressW &&
               anisotropyEnable == other.anisotropyEnable &&
               std::abs(maxAnisotropy - other.maxAnisotropy) < 1e-4F &&
               std::abs(minLod - other.minLod) < 1e-4F && std::abs(maxLod - other.maxLod) < 1e-4F &&
               std::abs(mipLodBias - other.mipLodBias) < 1e-4F &&
               compareEnable == other.compareEnable && compareOp == other.compareOp;
    }
};

} // namespace eng::render

namespace std {
template <> struct hash<eng::render::SamplerCacheKey> {
    size_t operator()(const eng::render::SamplerCacheKey& k) const noexcept {
        size_t h = 0x9e3779b9;
        auto hashCombine = [&h](size_t v) { h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2); };
        hashCombine(static_cast<size_t>(k.minFilter));
        hashCombine(static_cast<size_t>(k.magFilter));
        hashCombine(static_cast<size_t>(k.mipmapMode));
        hashCombine(static_cast<size_t>(k.addressU));
        hashCombine(static_cast<size_t>(k.addressV));
        hashCombine(static_cast<size_t>(k.addressW));
        hashCombine(k.anisotropyEnable ? 1U : 0U);
        hashCombine(std::hash<int32_t>{}(static_cast<int32_t>(k.maxAnisotropy * 100.0F)));
        hashCombine(std::hash<int32_t>{}(static_cast<int32_t>(k.minLod * 100.0F)));
        hashCombine(std::hash<int32_t>{}(static_cast<int32_t>(k.maxLod * 100.0F)));
        hashCombine(std::hash<int32_t>{}(static_cast<int32_t>(k.mipLodBias * 100.0F)));
        hashCombine(k.compareEnable ? 1U : 0U);
        hashCombine(static_cast<size_t>(k.compareOp));
        return h;
    }
};
} // namespace std

namespace eng::render {

/// @class SamplerCache
/// @brief Resource manager deduplicating identical RHI texture samplers.
class SamplerCache {
public:
    SamplerCache() = default;
    ~SamplerCache() noexcept = default;

    SamplerCache(const SamplerCache&) = delete;
    SamplerCache& operator=(const SamplerCache&) = delete;
    SamplerCache(SamplerCache&&) noexcept = default;
    SamplerCache& operator=(SamplerCache&&) noexcept = default;

    /// @brief Retrieves a cached sampler matching desc, or creates and stores a new one.
    ///
    /// @param device The active RHI device for sampler creation.
    /// @param desc Sampler configuration to look up or instantiate.
    /// @return Generational SamplerHandle on success, or Error on failure.
    Result<rhi::SamplerHandle> getOrCreate(rhi::RhiDevice& device, const rhi::SamplerDesc& desc);

    /// @brief Explicitly destroys all cached GPU samplers and clears the lookup table.
    /// Must only be called when the GPU is idle (e.g. after device.waitIdle()).
    void clear(rhi::RhiDevice& device) noexcept;

    /// @brief Returns the total count of distinct GPU samplers currently cached.
    [[nodiscard]] size_t getSamplerCount() const noexcept { return samplers_.size(); }

    /// @brief Returns cumulative cache hit count for telemetry.
    [[nodiscard]] uint64_t getCacheHits() const noexcept { return cacheHits_; }

    /// @brief Returns cumulative cache miss count for telemetry.
    [[nodiscard]] uint64_t getCacheMisses() const noexcept { return cacheMisses_; }

    /// @brief Checks whether a sampler configuration is already resident in the cache.
    [[nodiscard]] bool contains(const rhi::SamplerDesc& desc) const noexcept;

private:
    std::unordered_map<SamplerCacheKey, rhi::SamplerHandle> samplers_{};
    uint64_t cacheHits_{0};
    uint64_t cacheMisses_{0};
};

} // namespace eng::render
