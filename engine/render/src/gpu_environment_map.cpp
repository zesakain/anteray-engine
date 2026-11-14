/// @file gpu_environment_map.cpp
/// @brief Implementation of GpuEnvironmentMap creation from HDR images and procedural fallback.

#include "render/gpu_environment_map.h"

#include "core/log.h"

namespace eng::render {

Result<GpuEnvironmentMap>
GpuEnvironmentMap::createFromHdr(rhi::RhiDevice& device, const HdrImage& hdr, uint32_t envRes,
                                 uint32_t irradianceRes, uint32_t prefilterRes, uint32_t mipCount) {
    if (!hdr.isValid()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Cannot create GpuEnvironmentMap from invalid HdrImage"};
    }

    // 1. Convert equirectangular HDR to cubemap
    const CubemapData baseCubemap = IblProcessor::equirectangularToCubemap(hdr, envRes);
    if (!baseCubemap.isValid()) {
        return Error{.code = ErrorCode::ResourceCreationFailed,
                     .message = "Failed to convert HDR image to cubemap"};
    }

    // 2. Convolve diffuse irradiance
    const float sampleDelta = (irradianceRes <= 16) ? 0.15F : 0.08F;
    const CubemapData irradianceCubemap =
        IblProcessor::convolveDiffuseIrradiance(baseCubemap, irradianceRes, sampleDelta);
    if (!irradianceCubemap.isValid()) {
        return Error{.code = ErrorCode::ResourceCreationFailed,
                     .message = "Failed to convolve diffuse irradiance cubemap"};
    }

    auto irrTexRes = GpuTexture::createCubemap(device, irradianceCubemap);
    if (!irrTexRes.hasValue()) {
        return irrTexRes.error();
    }

    // 3. Prefilter GGX specular environment
    const uint32_t sampleCount = (prefilterRes <= 32) ? 32U : 128U;
    const CubemapData prefilterCubemap = IblProcessor::prefilterSpecularEnvironment(
        baseCubemap, prefilterRes, mipCount, sampleCount);
    if (!prefilterCubemap.isValid()) {
        return Error{.code = ErrorCode::ResourceCreationFailed,
                     .message = "Failed to prefilter specular environment cubemap"};
    }

    auto prefTexRes = GpuTexture::createCubemap(device, prefilterCubemap);
    if (!prefTexRes.hasValue()) {
        return prefTexRes.error();
    }

    // 4. Generate 2D split-sum BRDF integration LUT
    // Cached once across instances as Cook-Torrance GGX split-sum is invariant to environment maps.
    static const BrdfLutData s_cachedBrdfLut = IblProcessor::generateBrdfLut(128, 128);
    if (!s_cachedBrdfLut.isValid()) {
        return Error{.code = ErrorCode::ResourceCreationFailed,
                     .message = "Failed to generate BRDF integration LUT"};
    }

    auto lutTexRes = GpuTexture::createBrdfLut(device, s_cachedBrdfLut);
    if (!lutTexRes.hasValue()) {
        return lutTexRes.error();
    }

    GpuEnvironmentMap envMap{};
    envMap.irradianceMap = std::move(irrTexRes.value());
    envMap.prefilteredMap = std::move(prefTexRes.value());
    envMap.brdfLut = std::move(lutTexRes.value());
    envMap.maxPrefilterMipLevel = static_cast<float>(mipCount - 1U);
    envMap.intensity = 1.0F;

    return Result<GpuEnvironmentMap>::success(std::move(envMap));
}

Result<GpuEnvironmentMap> GpuEnvironmentMap::createDefault(rhi::RhiDevice& device) {
    const HdrImage sky = IblProcessor::generateProceduralSky(64, 32, Vec3(0.4F, 0.7F, 0.5F), 10.0F);
    return createFromHdr(device, sky, 32, 16, 32, 4);
}

} // namespace eng::render
