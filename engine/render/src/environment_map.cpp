/// @file environment_map.cpp
/// @brief Implementation of HDR environment map loading, cubemap conversion,
///        diffuse irradiance convolution, and GGX specular prefiltering.

#include "render/environment_map.h"

#include "core/log.h"
#include "render/image.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace eng::render {

namespace {

[[nodiscard]] inline Vec3 normalize(const Vec3& v) noexcept {
    return v.normalized();
}
[[nodiscard]] inline float dot(const Vec3& a, const Vec3& b) noexcept {
    return Vec3::dot(a, b);
}
[[nodiscard]] inline Vec3 cross(const Vec3& a, const Vec3& b) noexcept {
    return Vec3::cross(a, b);
}

constexpr float kPi = 3.14159265358979323846F;
constexpr float kTwoPi = 2.0F * kPi;
constexpr float kInvPi = 1.0F / kPi;

[[nodiscard]] inline float radicalInverseVdC(uint32_t bits) noexcept {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<float>(bits) * 2.3283064365386963e-10F;
}

[[nodiscard]] inline Vec2 hammersley(uint32_t i, uint32_t n) noexcept {
    return Vec2(static_cast<float>(i) / static_cast<float>(n), radicalInverseVdC(i));
}

[[nodiscard]] inline Vec3 importanceSampleGGX(const Vec2& xi, const Vec3& n,
                                              float roughness) noexcept {
    const float a = roughness * roughness;
    const float phi = kTwoPi * xi.x;
    const float cosTheta = std::sqrt((1.0F - xi.y) / (1.0F + (a * a - 1.0F) * xi.y));
    const float sinTheta = std::sqrt(std::max(0.0F, 1.0F - cosTheta * cosTheta));

    const Vec3 h(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);

    const Vec3 up = (std::abs(n.y) < 0.999F) ? Vec3(0.0F, 1.0F, 0.0F) : Vec3(0.0F, 0.0F, 1.0F);
    const Vec3 tangent = normalize(cross(up, n));
    const Vec3 bitangent = cross(n, tangent);

    return normalize(tangent * h.x + bitangent * h.y + n * h.z);
}

[[nodiscard]] inline float geometrySmithGGXIBL(float nDotV, float nDotL, float roughness) noexcept {
    const float k = (roughness * roughness) * 0.5F;
    const float gv = nDotV / (nDotV * (1.0F - k) + k);
    const float gl = nDotL / (nDotL * (1.0F - k) + k);
    return gv * gl;
}

} // namespace

Vec3 HdrImage::sampleEquirectangular(const Vec3& dir) const noexcept {
    if (!isValid()) {
        return Vec3::zero();
    }

    const Vec3 d = normalize(dir);
    const float phi = std::atan2(d.z, d.x);
    const float theta = std::asin(std::clamp(d.y, -1.0F, 1.0F));

    float u = (phi / kTwoPi) + 0.5F;
    float v = 0.5F - (theta * kInvPi);

    // Wrap u around [0, 1]
    u = u - std::floor(u);
    v = std::clamp(v, 0.0F, 1.0F);

    const float fx = u * static_cast<float>(width);
    const float fy = v * static_cast<float>(height);

    const auto x0 = static_cast<uint32_t>(fx) % width;
    const auto y0 = std::min(static_cast<uint32_t>(fy), height - 1U);
    const uint32_t x1 = (x0 + 1U) % width;
    const uint32_t y1 = std::min(y0 + 1U, height - 1U);

    const float fracX = fx - std::floor(fx);
    const float fracY = fy - std::floor(fy);

    const auto fetch = [this](uint32_t px, uint32_t py) noexcept -> Vec3 {
        const size_t idx = (static_cast<size_t>(py) * width + px) * channels;
        return Vec3(pixels[idx], pixels[idx + 1], pixels[idx + 2]);
    };

    const Vec3 c00 = fetch(x0, y0);
    const Vec3 c10 = fetch(x1, y0);
    const Vec3 c01 = fetch(x0, y1);
    const Vec3 c11 = fetch(x1, y1);

    const Vec3 c0 = c00 * (1.0F - fracX) + c10 * fracX;
    const Vec3 c1 = c01 * (1.0F - fracX) + c11 * fracX;

    return c0 * (1.0F - fracY) + c1 * fracY;
}

Vec3 CubemapData::sample(const Vec3& dir) const noexcept {
    if (!isValid()) {
        return Vec3::zero();
    }

    uint32_t faceIndex = 0;
    float u = 0.0F;
    float v = 0.0F;
    IblProcessor::directionToCubeFaceUV(dir, faceIndex, u, v);

    const auto& face = mips[0].faces[faceIndex];
    const auto fx = u * static_cast<float>(face.width - 1U);
    const auto fy = v * static_cast<float>(face.height - 1U);

    const auto x0 = std::min(static_cast<uint32_t>(fx), face.width - 1U);
    const auto y0 = std::min(static_cast<uint32_t>(fy), face.height - 1U);
    const uint32_t x1 = std::min(x0 + 1U, face.width - 1U);
    const uint32_t y1 = std::min(y0 + 1U, face.height - 1U);

    const float fracX = fx - static_cast<float>(x0);
    const float fracY = fy - static_cast<float>(y0);

    const auto fetch = [&face](uint32_t px, uint32_t py) noexcept -> Vec3 {
        const size_t idx = (static_cast<size_t>(py) * face.width + px) * 4U;
        return Vec3(halfToFloat(face.halfPixels[idx]), halfToFloat(face.halfPixels[idx + 1U]),
                    halfToFloat(face.halfPixels[idx + 2U]));
    };

    const Vec3 c00 = fetch(x0, y0);
    const Vec3 c10 = fetch(x1, y0);
    const Vec3 c01 = fetch(x0, y1);
    const Vec3 c11 = fetch(x1, y1);

    const Vec3 c0 = c00 * (1.0F - fracX) + c10 * fracX;
    const Vec3 c1 = c01 * (1.0F - fracX) + c11 * fracX;

    return c0 * (1.0F - fracY) + c1 * fracY;
}

Result<HdrImage> IblProcessor::loadHdrFromFile(std::string_view filePath) {
    auto res = ImageDecoder::decodeHdrFromFile(filePath, 4);
    if (!res.hasValue()) {
        return res.error();
    }
    return Result<HdrImage>::success(HdrImage(std::move(res.value())));
}

Result<HdrImage> IblProcessor::loadHdrFromMemory(std::span<const uint8_t> buffer) {
    auto res = ImageDecoder::decodeHdrFromMemory(buffer, 4);
    if (!res.hasValue()) {
        return res.error();
    }
    return Result<HdrImage>::success(HdrImage(std::move(res.value())));
}

HdrImage IblProcessor::generateProceduralSky(uint32_t width, uint32_t height, const Vec3& sunDir,
                                             float sunIntensity) {
    HdrImage image{};
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.pixels.resize(static_cast<size_t>(width) * height * 4U);

    const Vec3 normalizedSun = normalize(sunDir);

    // Physically motivated sky palette:
    // Zenith: deep Rayleigh scattering blue
    // Horizon: warm scattering white-orange
    // Ground: subtle earthy bounce
    const Vec3 zenithColor(0.18F, 0.42F, 0.85F);
    const Vec3 horizonColor(0.75F, 0.85F, 0.95F);
    const Vec3 groundColor(0.20F, 0.18F, 0.16F);
    const Vec3 sunDiscColor(1.0F, 0.96F, 0.88F);

    for (uint32_t y = 0; y < height; ++y) {
        const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
        const float theta = (0.5F - v) * kPi; // from +pi/2 (top) to -pi/2 (bottom)
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);

        for (uint32_t x = 0; x < width; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(width);
            const float phi = (u - 0.5F) * kTwoPi;

            const Vec3 dir(cosTheta * std::cos(phi), sinTheta, cosTheta * std::sin(phi));

            Vec3 pixelColor;
            if (dir.y >= 0.0F) {
                // Sky hemisphere
                const float horizonFactor = std::pow(1.0F - dir.y, 4.0F);
                pixelColor = zenithColor * (1.0F - horizonFactor) + horizonColor * horizonFactor;

                // Sun disc & forward Mie scattering
                const float sunDot = std::max(dot(dir, normalizedSun), 0.0F);
                const float mieGlow = std::pow(sunDot, 16.0F) * 0.4F;
                const float sunDisc = std::pow(sunDot, 1024.0F) * sunIntensity;

                pixelColor += (sunDiscColor * sunDisc) + (horizonColor * mieGlow);
            } else {
                // Ground hemisphere
                const float groundFactor = std::clamp(-dir.y * 4.0F, 0.0F, 1.0F);
                pixelColor = horizonColor * (1.0F - groundFactor) + groundColor * groundFactor;
            }

            const size_t idx = (static_cast<size_t>(y) * width + x) * 4U;
            image.pixels[idx] = pixelColor.x;
            image.pixels[idx + 1U] = pixelColor.y;
            image.pixels[idx + 2U] = pixelColor.z;
            image.pixels[idx + 3U] = 1.0F;
        }
    }

    return image;
}

Vec3 IblProcessor::cubeFaceUVToDirection(uint32_t faceIndex, float u, float v) noexcept {
    const float s = 2.0F * u - 1.0F;
    const float t = 2.0F * v - 1.0F;

    Vec3 dir;
    switch (faceIndex) {
    case 0: // +X
        dir = Vec3(1.0F, -t, -s);
        break;
    case 1: // -X
        dir = Vec3(-1.0F, -t, s);
        break;
    case 2: // +Y
        dir = Vec3(s, 1.0F, t);
        break;
    case 3: // -Y
        dir = Vec3(s, -1.0F, -t);
        break;
    case 4: // +Z
        dir = Vec3(s, -t, 1.0F);
        break;
    case 5: // -Z
    default:
        dir = Vec3(-s, -t, -1.0F);
        break;
    }
    return normalize(dir);
}

void IblProcessor::directionToCubeFaceUV(const Vec3& dir, uint32_t& outFace, float& outU,
                                         float& outV) noexcept {
    const float absX = std::abs(dir.x);
    const float absY = std::abs(dir.y);
    const float absZ = std::abs(dir.z);

    float s = 0.0F;
    float t = 0.0F;
    float ma = 1.0F;

    if (absX >= absY && absX >= absZ) {
        ma = absX;
        if (dir.x > 0.0F) {
            outFace = 0; // +X
            s = -dir.z;
            t = -dir.y;
        } else {
            outFace = 1; // -X
            s = dir.z;
            t = -dir.y;
        }
    } else if (absY >= absX && absY >= absZ) {
        ma = absY;
        if (dir.y > 0.0F) {
            outFace = 2; // +Y
            s = dir.x;
            t = dir.z;
        } else {
            outFace = 3; // -Y
            s = dir.x;
            t = -dir.z;
        }
    } else {
        ma = absZ;
        if (dir.z > 0.0F) {
            outFace = 4; // +Z
            s = dir.x;
            t = -dir.y;
        } else {
            outFace = 5; // -Z
            s = -dir.x;
            t = -dir.y;
        }
    }

    outU = std::clamp(0.5F * (s / ma + 1.0F), 0.0F, 1.0F);
    outV = std::clamp(0.5F * (t / ma + 1.0F), 0.0F, 1.0F);
}

CubemapData IblProcessor::equirectangularToCubemap(const HdrImage& hdr, uint32_t faceResolution) {
    CubemapData cubemap{};
    cubemap.size = faceResolution;
    cubemap.mipLevels = 1;
    cubemap.format = rhi::TextureFormat::RGBA16_FLOAT;
    cubemap.mips.resize(1);

    auto& mip0 = cubemap.mips[0];
    mip0.faces.resize(6);

    for (uint32_t f = 0; f < 6; ++f) {
        auto& face = mip0.faces[f];
        face.width = faceResolution;
        face.height = faceResolution;
        face.halfPixels.resize(static_cast<size_t>(faceResolution) * faceResolution * 4U);

        for (uint32_t y = 0; y < faceResolution; ++y) {
            const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(faceResolution);
            for (uint32_t x = 0; x < faceResolution; ++x) {
                const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(faceResolution);

                const Vec3 dir = cubeFaceUVToDirection(f, u, v);
                const Vec3 color = hdr.sampleEquirectangular(dir);

                const size_t idx = (static_cast<size_t>(y) * faceResolution + x) * 4U;
                face.halfPixels[idx] = floatToHalf(color.x);
                face.halfPixels[idx + 1U] = floatToHalf(color.y);
                face.halfPixels[idx + 2U] = floatToHalf(color.z);
                face.halfPixels[idx + 3U] = floatToHalf(1.0F);
            }
        }
    }

    return cubemap;
}

CubemapData IblProcessor::convolveDiffuseIrradiance(const CubemapData& envCubemap,
                                                    uint32_t faceResolution, float sampleDelta) {
    CubemapData irradianceMap{};
    irradianceMap.size = faceResolution;
    irradianceMap.mipLevels = 1;
    irradianceMap.format = rhi::TextureFormat::RGBA16_FLOAT;
    irradianceMap.mips.resize(1);

    auto& mip0 = irradianceMap.mips[0];
    mip0.faces.resize(6);

    const float delta = std::clamp(sampleDelta, 0.02F, 0.2F);
    const auto numPhi = static_cast<uint32_t>(std::ceil(kTwoPi / delta));
    const auto numTheta = static_cast<uint32_t>(std::ceil((0.5F * kPi) / delta));
    const float stepPhi = kTwoPi / static_cast<float>(numPhi);
    const float stepTheta = (0.5F * kPi) / static_cast<float>(numTheta);

    for (uint32_t f = 0; f < 6; ++f) {
        auto& face = mip0.faces[f];
        face.width = faceResolution;
        face.height = faceResolution;
        face.halfPixels.resize(static_cast<size_t>(faceResolution) * faceResolution * 4U);

        for (uint32_t y = 0; y < faceResolution; ++y) {
            const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(faceResolution);
            for (uint32_t x = 0; x < faceResolution; ++x) {
                const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(faceResolution);

                const Vec3 normal = cubeFaceUVToDirection(f, u, v);

                const Vec3 up =
                    (std::abs(normal.y) < 0.999F) ? Vec3(0.0F, 1.0F, 0.0F) : Vec3(0.0F, 0.0F, 1.0F);
                const Vec3 right = normalize(cross(up, normal));
                const Vec3 realUp = cross(normal, right);

                Vec3 irradianceSum = Vec3::zero();
                float totalWeight = 0.0F;

                for (uint32_t p = 0; p < numPhi; ++p) {
                    const float phi = static_cast<float>(p) * stepPhi;
                    const float cosPhi = std::cos(phi);
                    const float sinPhi = std::sin(phi);

                    for (uint32_t t = 0; t < numTheta; ++t) {
                        const float theta = (static_cast<float>(t) + 0.5F) * stepTheta;
                        const float sinTheta = std::sin(theta);
                        const float cosTheta = std::cos(theta);

                        const Vec3 tangentSample(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
                        const Vec3 sampleVec = right * tangentSample.x + realUp * tangentSample.y +
                                               normal * tangentSample.z;

                        const float weight = cosTheta * sinTheta;
                        irradianceSum += envCubemap.sample(sampleVec) * weight;
                        totalWeight += weight;
                    }
                }

                const Vec3 finalIrradiance =
                    (totalWeight > 0.0F) ? (irradianceSum * (1.0F / totalWeight)) : Vec3::zero();

                const size_t idx = (static_cast<size_t>(y) * faceResolution + x) * 4U;
                face.halfPixels[idx] = floatToHalf(finalIrradiance.x);
                face.halfPixels[idx + 1U] = floatToHalf(finalIrradiance.y);
                face.halfPixels[idx + 2U] = floatToHalf(finalIrradiance.z);
                face.halfPixels[idx + 3U] = floatToHalf(1.0F);
            }
        }
    }

    return irradianceMap;
}

CubemapData IblProcessor::prefilterSpecularEnvironment(const CubemapData& envCubemap,
                                                       uint32_t faceResolution, uint32_t mipCount,
                                                       uint32_t sampleCount) {
    CubemapData prefiltered{};
    prefiltered.size = faceResolution;
    prefiltered.mipLevels = std::max(1U, mipCount);
    prefiltered.format = rhi::TextureFormat::RGBA16_FLOAT;
    prefiltered.mips.resize(prefiltered.mipLevels);

    const uint32_t nSamples = std::max(16U, sampleCount);

    for (uint32_t mip = 0; mip < prefiltered.mipLevels; ++mip) {
        const uint32_t mipRes = std::max(1U, faceResolution >> mip);
        const float roughness =
            (prefiltered.mipLevels > 1U)
                ? (static_cast<float>(mip) / static_cast<float>(prefiltered.mipLevels - 1U))
                : 0.0F;

        auto& mipLevel = prefiltered.mips[mip];
        mipLevel.faces.resize(6);

        for (uint32_t f = 0; f < 6; ++f) {
            auto& face = mipLevel.faces[f];
            face.width = mipRes;
            face.height = mipRes;
            face.halfPixels.resize(static_cast<size_t>(mipRes) * mipRes * 4U);

            for (uint32_t y = 0; y < mipRes; ++y) {
                const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(mipRes);
                for (uint32_t x = 0; x < mipRes; ++x) {
                    const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(mipRes);

                    const Vec3 normal = cubeFaceUVToDirection(f, u, v);
                    const Vec3 r = normal;
                    const Vec3 view = r;

                    Vec3 prefilteredColor = Vec3::zero();
                    float totalWeight = 0.0F;

                    if (roughness < 0.01F) {
                        // Roughness 0: perfect specular mirror reflection
                        prefilteredColor = envCubemap.sample(r);
                        totalWeight = 1.0F;
                    } else {
                        for (uint32_t i = 0; i < nSamples; ++i) {
                            const Vec2 xi = hammersley(i, nSamples);
                            const Vec3 h = importanceSampleGGX(xi, normal, roughness);
                            const Vec3 l = normalize(h * (2.0F * dot(view, h)) - view);

                            const float nDotL = std::max(dot(normal, l), 0.0F);
                            if (nDotL > 0.0F) {
                                prefilteredColor += envCubemap.sample(l) * nDotL;
                                totalWeight += nDotL;
                            }
                        }
                    }

                    const Vec3 finalColor = (totalWeight > 0.0F) ? (prefilteredColor / totalWeight)
                                                                 : envCubemap.sample(normal);

                    const size_t idx = (static_cast<size_t>(y) * mipRes + x) * 4U;
                    face.halfPixels[idx] = floatToHalf(finalColor.x);
                    face.halfPixels[idx + 1U] = floatToHalf(finalColor.y);
                    face.halfPixels[idx + 2U] = floatToHalf(finalColor.z);
                    face.halfPixels[idx + 3U] = floatToHalf(1.0F);
                }
            }
        }
    }

    return prefiltered;
}

BrdfLutData IblProcessor::generateBrdfLut(uint32_t resolution, uint32_t sampleCount) {
    BrdfLutData lut{};
    lut.width = resolution;
    lut.height = resolution;
    lut.format = rhi::TextureFormat::RGBA16_FLOAT;
    lut.halfPixels.resize(static_cast<size_t>(resolution) * resolution * 4U);

    const uint32_t nSamples = std::max(64U, sampleCount);

    for (uint32_t y = 0; y < resolution; ++y) {
        const float roughness =
            std::max(0.001F, (static_cast<float>(y) + 0.5F) / static_cast<float>(resolution));

        for (uint32_t x = 0; x < resolution; ++x) {
            const float nDotV =
                std::max(0.001F, (static_cast<float>(x) + 0.5F) / static_cast<float>(resolution));

            const Vec3 view(std::sqrt(std::max(0.0F, 1.0F - nDotV * nDotV)), 0.0F, nDotV);
            const Vec3 normal(0.0F, 0.0F, 1.0F);

            float scaleA = 0.0F;
            float biasB = 0.0F;

            for (uint32_t i = 0; i < nSamples; ++i) {
                const Vec2 xi = hammersley(i, nSamples);
                const Vec3 h = importanceSampleGGX(xi, normal, roughness);
                const Vec3 l = normalize(h * (2.0F * dot(view, h)) - view);

                const float nDotL = std::max(l.z, 0.0F);
                const float nDotH = std::max(h.z, 0.0F);
                const float vDotH = std::max(dot(view, h), 0.0F);

                if (nDotL > 0.0F && nDotV > 0.0F && nDotH > 0.0F) {
                    const float g = geometrySmithGGXIBL(nDotV, nDotL, roughness);
                    const float gVis = (g * vDotH) / (nDotH * nDotV);
                    const float fc = std::pow(1.0F - vDotH, 5.0F);

                    scaleA += (1.0F - fc) * gVis;
                    biasB += fc * gVis;
                }
            }

            scaleA /= static_cast<float>(nSamples);
            biasB /= static_cast<float>(nSamples);

            const size_t idx = (static_cast<size_t>(y) * resolution + x) * 4U;
            lut.halfPixels[idx] = floatToHalf(std::clamp(scaleA, 0.0F, 1.0F));
            lut.halfPixels[idx + 1U] = floatToHalf(std::clamp(biasB, 0.0F, 1.0F));
            lut.halfPixels[idx + 2U] = floatToHalf(0.0F);
            lut.halfPixels[idx + 3U] = floatToHalf(1.0F);
        }
    }

    return lut;
}

} // namespace eng::render
