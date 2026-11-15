/// @file gpu_texture.h
/// @brief GPU texture and sampler resource wrapper and upload abstraction.
///
/// Purpose:
/// Owns and manages the GPU texture resource and associated sampler allocated for an ImageData
/// object. Provides RAII destruction via the owning RhiDevice.
///
/// Ownership and Lifetime:
/// Created by GpuTexture::create(RhiDevice&, const ImageData&, bool, const SamplerDesc*).
/// Owned typically by AssetManager via std::unique_ptr<GpuTexture>.
/// Automatically destroys GPU texture and sampler handles upon destruction.
///
/// Thread-safety:
/// Main thread / render thread only. Not safe for concurrent mutation.
///
/// Performance:
/// Staging buffers are allocated during upload and discarded immediately after submission.
/// Zero per-frame allocations during texture sampling and binding.

#pragma once

#include "core/error.h"
#include "core/result.h"
#include "render/environment_map.h"
#include "render/image.h"
#include "rhi/rhi_device.h"
#include "rhi/sampler.h"
#include "rhi/texture.h"

#include <cstdint>
#include <memory>

namespace eng::render {

/// @class GpuTexture
/// @brief RAII wrapper for GPU texture and sampler resources representing a 2D image.
class GpuTexture {
public:
    /// @brief Factory function creating GPU texture, uploading image pixels, and creating sampler.
    ///
    /// @param device The RHI device used to allocate resources and record upload commands.
    /// @param imageData Validated host-memory bitmap pixel data.
    /// @param generateMips Whether to compute and generate complete mipmap chain.
    /// @param customSamplerDesc Optional custom sampler configuration; defaults to linear repeat.
    /// @return Unique pointer to the allocated GpuTexture on success, or Error on failure.
    [[nodiscard]] static Result<std::unique_ptr<GpuTexture>>
    create(rhi::RhiDevice& device, const ImageData& imageData, bool generateMips = true,
           const rhi::SamplerDesc* customSamplerDesc = nullptr);

    /// @brief Creates GPU cubemap texture (6 faces) from CubemapData across all mip levels.
    ///
    /// @param device The RHI device used to allocate resources and record upload commands.
    /// @param cubemapData Validated cubemap with 6 faces per mip level in RGBA16_FLOAT.
    /// @param customSamplerDesc Optional custom sampler configuration; defaults to linear clamp.
    /// @return Unique pointer to allocated GpuTexture or Error on failure.
    [[nodiscard]] static Result<std::unique_ptr<GpuTexture>>
    createCubemap(rhi::RhiDevice& device, const CubemapData& cubemapData,
                  const rhi::SamplerDesc* customSamplerDesc = nullptr);

    /// @brief Creates GPU 2D texture and clamp sampler from BrdfLutData.
    ///
    /// @param device The RHI device used to allocate resources and record upload commands.
    /// @param brdfLutData Validated 2D BRDF integration lookup table data in RGBA16_FLOAT.
    /// @return Unique pointer to allocated GpuTexture or Error on failure.
    [[nodiscard]] static Result<std::unique_ptr<GpuTexture>>
    createBrdfLut(rhi::RhiDevice& device, const BrdfLutData& brdfLutData);

    ~GpuTexture() noexcept;

    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;
    GpuTexture(GpuTexture&& other) noexcept;
    GpuTexture& operator=(GpuTexture&& other) noexcept;

    [[nodiscard]] rhi::TextureHandle getTexture() const noexcept { return texture_; }
    [[nodiscard]] rhi::SamplerHandle getSampler() const noexcept { return sampler_; }
    [[nodiscard]] uint32_t getWidth() const noexcept { return width_; }
    [[nodiscard]] uint32_t getHeight() const noexcept { return height_; }
    [[nodiscard]] uint32_t getMipLevels() const noexcept { return mipLevels_; }
    [[nodiscard]] rhi::TextureFormat getFormat() const noexcept { return format_; }
    [[nodiscard]] bool isValid() const noexcept { return texture_.isValid(); }
    [[nodiscard]] bool ownsSampler() const noexcept { return ownsSampler_; }

    /// Sets an externally managed sampler handle (e.g. from SamplerCache).
    /// Disables GpuTexture ownership of the sampler destruction.
    void setSharedSampler(rhi::SamplerHandle sampler) noexcept;

private:
    GpuTexture(rhi::RhiDevice& device, rhi::TextureHandle texture, rhi::SamplerHandle sampler,
               uint32_t width, uint32_t height, uint32_t mipLevels,
               rhi::TextureFormat format) noexcept;

    rhi::RhiDevice* device_{nullptr};
    rhi::TextureHandle texture_{};
    rhi::SamplerHandle sampler_{};
    uint32_t width_{0};
    uint32_t height_{0};
    uint32_t mipLevels_{1};
    rhi::TextureFormat format_{rhi::TextureFormat::RGBA8_UNORM};
    bool ownsSampler_{true};
};

} // namespace eng::render
