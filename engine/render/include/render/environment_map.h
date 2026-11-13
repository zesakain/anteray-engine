/// @file environment_map.h
/// @brief CPU-side HDR environment map decoding, equirectangular cubemap generation,
///        diffuse irradiance convolution, and GGX specular prefiltering.
///
/// Purpose:
/// Provides mathematically rigorous, offline and pre-warm Image-Based Lighting (IBL)
/// processors conforming to the split-sum approximation. Computes diffuse irradiance
/// cubemaps, mipmapped GGX specular prefiltered cubemaps, and 2D BRDF integration LUTs.
///
/// Ownership and Lifetime:
/// Data structures (HdrImage, CubemapFace, CubemapData) own their host memory storage.
/// Conversion and convolution functions are pure or state-free.
///
/// Thread-safety:
/// Mathematical generation and sampling routines are thread-safe and re-entrant.
///
/// Performance:
/// Uses 16-bit half-precision floating point (RGBA16_FLOAT) for optimal GPU memory footprint
/// and texture cache throughput. All allocations occur during startup or scene load.

#pragma once

#include "core/error.h"
#include "core/math.h"
#include "core/result.h"
#include "render/image.h"
#include "rhi/rhi_types.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace eng::render {

/// @brief Converts standard 32-bit single-precision float to 16-bit IEEE 754 half-precision float.
[[nodiscard]] inline uint16_t floatToHalf(float val) noexcept {
    uint32_t x = 0;
    std::memcpy(&x, &val, sizeof(float));
    const uint32_t sign = (x >> 16U) & 0x8000U;
    const int32_t exp = static_cast<int32_t>((x >> 23U) & 0xFFU) - 127 + 15;
    uint32_t mantissa = x & 0x007FFFFFU;

    if (exp <= 0) {
        if (exp < -10) {
            return static_cast<uint16_t>(sign);
        }
        mantissa = (mantissa | 0x00800000U) >> static_cast<uint32_t>(1 - exp);
        return static_cast<uint16_t>(sign | (mantissa >> 13U));
    }
    if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00U);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10U) | (mantissa >> 13U));
}

/// @brief Converts 16-bit IEEE 754 half-precision float to 32-bit single-precision float.
[[nodiscard]] inline float halfToFloat(uint16_t val) noexcept {
    const uint32_t sign = static_cast<uint32_t>(val & 0x8000U) << 16U;
    uint32_t exp = static_cast<uint32_t>(val & 0x7C00U) >> 10U;
    uint32_t mantissa = static_cast<uint32_t>(val & 0x03FFU) << 13U;

    uint32_t out = 0;
    if (exp == 0U) {
        if (mantissa != 0U) {
            while ((mantissa & 0x00800000U) == 0U) {
                mantissa <<= 1U;
                --exp;
            }
            ++exp;
            mantissa &= ~0x00800000U;
            out = sign | ((exp + 127U - 15U) << 23U) | mantissa;
        } else {
            out = sign;
        }
    } else if (exp == 31U) {
        out = sign | 0x7F800000U | mantissa;
    } else {
        out = sign | ((exp + 127U - 15U) << 23U) | mantissa;
    }

    float res = 0.0F;
    std::memcpy(&res, &out, sizeof(float));
    return res;
}

/// @struct HdrImage
/// @brief Host memory 32-bit floating point high dynamic range image.
struct HdrImage : public HdrImageData {
    HdrImage() = default;
    explicit HdrImage(HdrImageData&& data) noexcept : HdrImageData(std::move(data)) {}

    /// @brief Samples image using bilinear interpolation and spherical equirectangular wrap-around.
    [[nodiscard]] Vec3 sampleEquirectangular(const Vec3& dir) const noexcept;
};

/// @struct CubemapFace
/// @brief Pixel payload for one face of a cubemap mip level.
struct CubemapFace {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint16_t> halfPixels; // RGBA16_FLOAT (4 halfs per texel)
};

/// @struct CubemapData
/// @brief Full cubemap resource comprising 6 faces across all mip levels.
struct CubemapData {
    uint32_t size{0};
    uint32_t mipLevels{1};
    rhi::TextureFormat format{rhi::TextureFormat::RGBA16_FLOAT};
    // Indexed as: mips[mipLevel].faces[faceIndex (0..5)]
    struct MipLevel {
        std::vector<CubemapFace> faces; // Exactly 6 faces
    };
    std::vector<MipLevel> mips;

    [[nodiscard]] bool isValid() const noexcept {
        if (size == 0 || mipLevels == 0 || mips.size() != mipLevels) {
            return false;
        }
        for (const auto& mip : mips) {
            if (mip.faces.size() != 6) {
                return false;
            }
            for (const auto& face : mip.faces) {
                if (face.halfPixels.empty()) {
                    return false;
                }
            }
        }
        return true;
    }

    /// @brief Samples the cubemap at base mip level using standard Vulkan face mapping.
    [[nodiscard]] Vec3 sample(const Vec3& dir) const noexcept;
};

/// @struct BrdfLutData
/// @brief 2D split-sum BRDF integration lookup table.
struct BrdfLutData {
    uint32_t width{256};
    uint32_t height{256};
    rhi::TextureFormat format{rhi::TextureFormat::RGBA16_FLOAT};
    std::vector<uint16_t> halfPixels; // RGBA16_FLOAT: (scale, bias, 0, 1)

    [[nodiscard]] bool isValid() const noexcept {
        return width > 0 && height > 0 && !halfPixels.empty();
    }
};

/// @class IblProcessor
/// @brief Mathematical generator and processor for Image-Based Lighting assets.
class IblProcessor {
public:
    /// @brief Loads an HDR (Radiance RGBE .hdr) file from disk.
    ///
    /// @param filePath Path to the .hdr file.
    /// @return Decoded HdrImage or error code.
    [[nodiscard]] static Result<HdrImage> loadHdrFromFile(std::string_view filePath);

    /// @brief Decodes an HDR file from memory bytes.
    [[nodiscard]] static Result<HdrImage> loadHdrFromMemory(std::span<const uint8_t> buffer);

    /// @brief Generates a procedural high dynamic range physical sky environment.
    ///
    /// @param width Width of equirectangular map.
    /// @param height Height of equirectangular map.
    /// @param sunDir Normalized direction pointing towards the sun.
    /// @param sunIntensity Radiance multiplier for the sun disk and atmospheric glow.
    [[nodiscard]] static HdrImage generateProceduralSky(uint32_t width = 1024,
                                                        uint32_t height = 512,
                                                        const Vec3& sunDir = Vec3(0.4F, 0.7F, 0.5F),
                                                        float sunIntensity = 15.0F);

    /// @brief Projects an equirectangular HDR image onto 6 faces of a cubemap.
    ///
    /// @param hdr Source HDR image.
    /// @param faceResolution Resolution of each square face (e.g. 512).
    /// @return Generated CubemapData with 1 mip level.
    [[nodiscard]] static CubemapData equirectangularToCubemap(const HdrImage& hdr,
                                                              uint32_t faceResolution = 512);

    /// @brief Computes Lambertian cosine-weighted diffuse irradiance cubemap.
    ///
    /// @param envCubemap Source environment radiance cubemap.
    /// @param faceResolution Resolution of irradiance cubemap (e.g. 32 or 64).
    /// @param sampleDelta Angular sampling step size in radians (e.g. 0.05 rad).
    [[nodiscard]] static CubemapData convolveDiffuseIrradiance(const CubemapData& envCubemap,
                                                               uint32_t faceResolution = 32,
                                                               float sampleDelta = 0.05F);

    /// @brief Computes GGX importance sampled specular prefiltered cubemap with mip progression.
    ///
    /// @param envCubemap Source environment radiance cubemap.
    /// @param faceResolution Base resolution of mip 0 (e.g. 128 or 256).
    /// @param mipCount Number of roughness mip levels (e.g. 5).
    /// @param sampleCount Number of Monte Carlo Hammersley samples per texel (e.g. 256).
    [[nodiscard]] static CubemapData prefilterSpecularEnvironment(const CubemapData& envCubemap,
                                                                  uint32_t faceResolution = 128,
                                                                  uint32_t mipCount = 5,
                                                                  uint32_t sampleCount = 256);

    /// @brief Generates 2D split-sum BRDF integration lookup table.
    ///
    /// @param resolution Width and height of LUT texture (e.g. 256).
    /// @param sampleCount Number of Monte Carlo samples per texel (e.g. 512).
    [[nodiscard]] static BrdfLutData generateBrdfLut(uint32_t resolution = 256,
                                                     uint32_t sampleCount = 512);

    /// @brief Maps cube face index and normalized coordinates (u, v) in [0, 1] to 3D unit
    /// direction.
    [[nodiscard]] static Vec3 cubeFaceUVToDirection(uint32_t faceIndex, float u, float v) noexcept;

    /// @brief Maps 3D unit direction to cube face index and normalized (u, v) coordinates in [0,
    /// 1].
    static void directionToCubeFaceUV(const Vec3& dir, uint32_t& outFace, float& outU,
                                      float& outV) noexcept;
};

} // namespace eng::render
