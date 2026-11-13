/// @file gpu_environment_map.h
/// @brief GPU environment map and Image-Based Lighting resource container.
///
/// Purpose:
/// Owns the GPU-resident textures (diffuse irradiance cubemap, GGX specular prefiltered cubemap,
/// and split-sum BRDF integration 2D LUT) along with runtime metadata.
///
/// Ownership and Lifetime:
/// Created by GpuEnvironmentMap::createFromHdr(...) or createDefault(...).
/// Owned by SceneRenderer or AssetManager via value/move semantics.
/// Destroys all owned GpuTexture instances on destruction.
///
/// Thread-safety:
/// Read-only access during command list recording is thread-safe.

#pragma once

#include "core/result.h"
#include "render/environment_map.h"
#include "render/gpu_texture.h"

#include <memory>

namespace eng::render {

/// @struct GpuEnvironmentMap
/// @brief Encapsulates GPU-resident IBL resources for diffuse and specular ambient lighting.
struct GpuEnvironmentMap {
    std::unique_ptr<GpuTexture> irradianceMap{};
    std::unique_ptr<GpuTexture> prefilteredMap{};
    std::unique_ptr<GpuTexture> brdfLut{};
    float maxPrefilterMipLevel{4.0F};
    float intensity{1.0F};

    [[nodiscard]] bool isValid() const noexcept {
        return irradianceMap != nullptr && irradianceMap->isValid() && prefilteredMap != nullptr &&
               prefilteredMap->isValid() && brdfLut != nullptr && brdfLut->isValid();
    }

    /// @brief Factory creating GPU environment map from decoded HDR image.
    ///
    /// @param device The RHI device used to allocate GPU textures and upload pixels.
    /// @param hdr Source floating-point equirectangular HDR image.
    /// @param envRes Resolution per face of environment cubemap.
    /// @param irradianceRes Resolution per face of convolved irradiance cubemap.
    /// @param prefilterRes Base resolution per face of specular prefiltered cubemap.
    /// @param mipCount Number of GGX prefilter mip levels.
    /// @return Allocated GpuEnvironmentMap or error.
    [[nodiscard]] static Result<GpuEnvironmentMap>
    createFromHdr(rhi::RhiDevice& device, const HdrImage& hdr, uint32_t envRes = 256,
                  uint32_t irradianceRes = 32, uint32_t prefilterRes = 128, uint32_t mipCount = 5);

    /// @brief Creates default fallback procedural physical sky environment map.
    ///
    /// @param device The RHI device used to allocate GPU resources.
    /// @return Allocated GpuEnvironmentMap with default physical sky.
    [[nodiscard]] static Result<GpuEnvironmentMap> createDefault(rhi::RhiDevice& device);
};

} // namespace eng::render
