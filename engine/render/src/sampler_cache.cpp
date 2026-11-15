/// @file sampler_cache.cpp
/// @brief Implementation of deduplicating RHI texture sampler cache.

#include "render/sampler_cache.h"

namespace eng::render {

Result<rhi::SamplerHandle> SamplerCache::getOrCreate(rhi::RhiDevice& device,
                                                     const rhi::SamplerDesc& desc) {
    const SamplerCacheKey key = SamplerCacheKey::fromDesc(desc);
    auto it = samplers_.find(key);
    if (it != samplers_.end()) {
        cacheHits_++;
        return Result<rhi::SamplerHandle>::success(it->second);
    }

    cacheMisses_++;
    auto samplerRes = device.createSampler(desc);
    if (!samplerRes.hasValue()) {
        return samplerRes.error();
    }

    const rhi::SamplerHandle handle = samplerRes.value();
    samplers_[key] = handle;
    return Result<rhi::SamplerHandle>::success(handle);
}

void SamplerCache::clear(rhi::RhiDevice& device) noexcept {
    for (const auto& [key, handle] : samplers_) {
        if (handle.isValid()) {
            device.destroySampler(handle);
        }
    }
    samplers_.clear();
}

bool SamplerCache::contains(const rhi::SamplerDesc& desc) const noexcept {
    const SamplerCacheKey key = SamplerCacheKey::fromDesc(desc);
    return samplers_.find(key) != samplers_.end();
}

} // namespace eng::render
