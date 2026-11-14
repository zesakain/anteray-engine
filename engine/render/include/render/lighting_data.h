/// @file lighting_data.h
/// @brief GPU lighting uniform buffer and scene descriptor layout definitions.
///
/// Purpose:
/// Declares binary-compatible std140 GPU lighting uniform structures and descriptor set layouts
/// for forward multi-light Cook-Torrance PBR shading and directional shadow mapping.
///
/// Contract:
/// - Exact 16-byte alignment and padding matching GLSL std140 rules.
/// - Total buffer size must match static assertion (1680 bytes).
///
/// Ownership and Lifetime:
/// Value types copied directly to GPU uniform buffers via RhiDevice::writeBuffer.
///
/// Thread-safety:
/// Const value structs; thread-safe for reading and stack allocations.
///
/// Cost:
/// Standard layout structs, zero heap allocation.

#pragma once

#include "core/math.h"
#include "rhi/descriptor.h"

#include <cstdint>

namespace eng::render {

inline constexpr uint32_t kMaxPointLights = 16;
inline constexpr uint32_t kMaxSpotLights = 16;

/// @struct GpuDirectionalLight
/// @brief std140-aligned representation of directional sun illumination.
struct GpuDirectionalLight {
    /// xyz: Light direction vector; w: castShadows flag (1.0 = true, 0.0 = false).
    Vec4 directionAndCastShadow{0.5F, 0.8F, 0.5F, 1.0F};

    /// rgb: Emitted radiance color; a: intensity multiplier.
    Vec4 colorAndIntensity{1.0F, 1.0F, 1.0F, 2.5F};
};

/// @struct GpuPointLight
/// @brief std140-aligned representation of omnidirectional point light.
struct GpuPointLight {
    /// xyz: World-space position; w: effective range in meters.
    Vec4 positionAndRange{0.0F, 0.0F, 0.0F, 10.0F};

    /// rgb: Emitted radiance color; a: intensity multiplier.
    Vec4 colorAndIntensity{1.0F, 1.0F, 1.0F, 0.0F};
};

/// @struct GpuSpotLight
/// @brief std140-aligned representation of directed conical spotlight.
struct GpuSpotLight {
    /// xyz: World-space position; w: effective range in meters.
    Vec4 positionAndRange{0.0F, 0.0F, 0.0F, 15.0F};

    /// xyz: World-space direction; w: intensity multiplier.
    Vec4 directionAndIntensity{0.0F, -1.0F, 0.0F, 0.0F};

    /// rgb: Emitted radiance color; a: castShadows flag (1.0 = true, 0.0 = false).
    Vec4 colorAndCastShadow{1.0F, 1.0F, 1.0F, 0.0F};

    /// x: cos(innerConeAngle); y: cos(outerConeAngle); zw: padding.
    Vec4 coneAngles{0.9F, 0.8F, 0.0F, 0.0F};
};

/// @struct GpuLightingData
/// @brief Uniform buffer block bound to Set 1, Binding 0 (size: 2240 bytes).
struct alignas(16) GpuLightingData {
    /// Cascade light-space view-projection transformation matrices (4 * 64 = 256 bytes).
    /// cascadeLightSpaceMatrices[0] serves as primary directional shadow matrix.
    Mat4 cascadeLightSpaceMatrices[4]{Mat4::identity(), Mat4::identity(), Mat4::identity(), Mat4::identity()};

    /// Directional sunlight parameters (32 bytes).
    GpuDirectionalLight dirLight{};

    /// Omnidirectional point lights array (16 * 32 = 512 bytes).
    GpuPointLight pointLights[kMaxPointLights]{};

    /// Conical spotlights array (16 * 64 = 1024 bytes).
    GpuSpotLight spotLights[kMaxSpotLights]{};

    /// xyz: World-space camera position; w: ambient intensity scalar (16 bytes).
    Vec4 cameraPosAndAmbientIntensity{0.0F, 0.0F, 5.0F, 0.15F};

    /// rgb: Ambient light color; w: constant shadow bias (16 bytes).
    Vec4 ambientColorAndShadowBias{1.0F, 1.0F, 1.0F, 0.005F};

    /// Number of active point lights (up to 16).
    uint32_t numPointLights{0};

    /// Number of active spotlights (up to 16).
    uint32_t numSpotLights{0};

    /// Whether directional light is active (1 = active, 0 = inactive).
    uint32_t dirLightActive{1};

    /// Packed shadow parameters: lower 16 bits = resolution, upper 16 bits = PCF quality.
    uint32_t shadowPackedParams{0};

    /// x: hasIbl (1.0 = true, 0.0 = false), y: intensity, z: maxPrefilterMipLevel, w: unused pad
    Vec4 iblParams{0.0F, 1.0F, 4.0F, 0.0F};

    /// x: giEnabled (1.0 = true, 0.0 = false), y: giIntensity, z: ssgiRadius, w: gtaoIntensity
    Vec4 giParams{0.0F, 1.0F, 2.0F, 1.0F};

    /// x: probesEnabled (1.0 = true, 0.0 = false), y: probeIntensity, zw: pad
    Vec4 probeParams{0.0F, 1.0F, 0.0F, 0.0F};

    /// Previous frame view-projection matrix for temporal reprojection (64 bytes)
    Mat4 prevViewProj{Mat4::identity()};

    /// Inverse view-projection matrix for world-space position reconstruction (64 bytes)
    Mat4 invViewProj{Mat4::identity()};

    /// Order-2 Spherical Harmonics coefficients for localized diffuse irradiance (9 * 16 = 144 bytes)
    Vec4 probeSh[9]{};

    /// Cascade split far distances in view space: x: cascade 0, y: cascade 1, z: cascade 2, w: cascade 3 (16 bytes)
    Vec4 cascadeSplits{6.0F, 18.0F, 45.0F, 100.0F};

    /// Cascade and contact shadow parameters (16 bytes):
    /// x: cascadeCount (1.0 to 4.0), y: cascadeBlendRange (0.10), z: contactShadowsEnabled, w: softShadowsEnabled
    Vec4 cascadeParams{4.0F, 0.10F, 1.0F, 1.0F};

    /// Explicit std140 alignment padding (16 bytes)
    Vec4 padShadow{0.0F, 0.0F, 0.0F, 0.0F};

    /// Backward-compatibility accessor for cascade 0 light-space matrix.
    [[nodiscard]] const Mat4& getLightSpaceMatrix() const noexcept {
        return cascadeLightSpaceMatrices[0];
    }
    void setLightSpaceMatrix(const Mat4& m) noexcept {
        cascadeLightSpaceMatrices[0] = m;
    }
};

static_assert(sizeof(GpuLightingData) == 2240,
              "GpuLightingData must strictly match std140 layout (2240 bytes)");

/// @brief Generates descriptor set layout descriptor for Scene/Lighting/Shadows (Set 1).
///
/// Binding 0: UniformBuffer (LightingData) in Vertex | Fragment stages.
/// Binding 1: CombinedImageSampler (Directional Shadow Map) in Fragment stage.
/// Binding 2: CombinedImageSampler (Diffuse Irradiance Cubemap) in Fragment stage.
/// Binding 3: CombinedImageSampler (Specular Prefiltered Cubemap) in Fragment stage.
/// Binding 4: CombinedImageSampler (Split-Sum BRDF 2D LUT) in Fragment stage.
/// Binding 5: CombinedImageSampler (Ground-Truth Ambient Occlusion Map) in Fragment stage.
/// Binding 6: CombinedImageSampler (Screen-Space Indirect Light Map) in Fragment stage.
[[nodiscard]] inline rhi::DescriptorSetLayoutDesc createSceneDescriptorSetLayoutDesc() {
    return rhi::DescriptorSetLayoutDesc{
        .bindings =
            {
                {
                    .binding = 0,
                    .descriptorType = rhi::DescriptorType::UniformBuffer,
                    .descriptorCount = 1,
                    .stageFlags = rhi::ShaderStageFlags::Vertex | rhi::ShaderStageFlags::Fragment,
                },
                {
                    .binding = 1,
                    .descriptorType = rhi::DescriptorType::CombinedImageSampler,
                    .descriptorCount = 1,
                    .stageFlags = rhi::ShaderStageFlags::Fragment,
                },
                {
                    .binding = 2,
                    .descriptorType = rhi::DescriptorType::CombinedImageSampler,
                    .descriptorCount = 1,
                    .stageFlags = rhi::ShaderStageFlags::Fragment,
                },
                {
                    .binding = 3,
                    .descriptorType = rhi::DescriptorType::CombinedImageSampler,
                    .descriptorCount = 1,
                    .stageFlags = rhi::ShaderStageFlags::Fragment,
                },
                {
                    .binding = 4,
                    .descriptorType = rhi::DescriptorType::CombinedImageSampler,
                    .descriptorCount = 1,
                    .stageFlags = rhi::ShaderStageFlags::Fragment,
                },
                {
                    .binding = 5,
                    .descriptorType = rhi::DescriptorType::CombinedImageSampler,
                    .descriptorCount = 1,
                    .stageFlags = rhi::ShaderStageFlags::Fragment,
                },
                {
                    .binding = 6,
                    .descriptorType = rhi::DescriptorType::CombinedImageSampler,
                    .descriptorCount = 1,
                    .stageFlags = rhi::ShaderStageFlags::Fragment,
                },
            },
        .debugName = "SceneLightingDescriptorSetLayout",
    };
}

} // namespace eng::render
