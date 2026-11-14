/// @file graphics_settings.h
/// @brief Centralized, strongly typed graphics settings and scalability configuration model.
///
/// Purpose:
/// Declares the master GraphicsSettings struct and modular sub-configurations (quality,
/// lighting, shadows, post-processing, performance, and debug rendering) governing
/// Hikari Engine's rendering pipeline.
///
/// Ownership and Lifetime:
/// Value types with zero heap allocation in hot path. Safely copyable across rendering,
/// editor UI, and persistence boundaries.
///
/// Thread Safety:
/// Plain data structs; const access is thread-safe. Runtime mutations must occur on the
/// main rendering/simulation thread before command recording.

#pragma once

#include "core/math.h"
#include "render/environment_map.h"
#include "render/post_process_types.h"
#include "rhi/gpu_caps.h"

#include <cstdint>
#include <string_view>

namespace eng::render {

/// @enum GraphicsQualityPreset
/// @brief Standardized engine quality tiers.
enum class GraphicsQualityPreset : uint8_t {
    Low = 0,
    Medium = 1,
    High = 2,
    Ultra = 3,
    Cinematic = 4,
    Custom = 5,
};

/// @enum TextureQuality
/// @brief Texture resolution and mipmap streaming tier.
enum class TextureQuality : uint8_t {
    Low = 0,    ///< Half-resolution / mip level 1 drop
    Medium = 1, ///< Standard compressed mips
    High = 2,   ///< Full-resolution original textures
    Ultra = 3,  ///< Full-resolution uncompressed / max mips
};

/// @enum AnisotropicFiltering
/// @brief Hardware anisotropic texture filtering multiplier.
enum class AnisotropicFiltering : uint8_t {
    Off = 0,
    X2 = 2,
    X4 = 4,
    X8 = 8,
    X16 = 16,
};

/// @enum AntiAliasingMode
/// @brief Edge antialiasing technique.
enum class AntiAliasingMode : uint8_t {
    None = 0,
    FXAA = 1,   ///< Fast Approximate Anti-Aliasing (post-process)
    TAA = 2,    ///< Temporal Anti-Aliasing (multiframe jitter)
    MSAA2x = 3, ///< Hardware Multi-Sample 2x
    MSAA4x = 4, ///< Hardware Multi-Sample 4x
};

/// @enum ShadowPcfQuality
/// @brief Percentage-Closer Filtering kernel quality for directional shadow sampling.
enum class ShadowPcfQuality : uint8_t {
    Hard = 0,   ///< Single tap depth comparison
    Medium = 1, ///< 3x3 PCF kernel (9 samples)
    High = 2,   ///< 5x5 PCF kernel (25 samples)
};

/// @enum DebugViewMode
/// @brief Diagnostic render visualization modes for materials, lighting, and passes.
enum class DebugViewMode : uint8_t {
    Lit = 0,                ///< Full Cook-Torrance PBR shading with HDR & tonemapping
    Wireframe = 1,          ///< Non-solid rasterization (VK_POLYGON_MODE_LINE)
    Normals = 2,            ///< World-space surface normal visualization
    Albedo = 3,             ///< Diffuse base color pass
    Roughness = 4,          ///< Material roughness scalar visualization
    Metallic = 5,           ///< Material metalness scalar visualization
    ShadowMap = 6,          ///< Orthographic directional shadow depth visualization
    LightingComplexity = 7, ///< Light source overlap evaluation
};

/// @enum SettingChangeSeverity
/// @brief Indicates the resource recreation cost of applying a modified setting.
enum class SettingChangeSeverity : uint8_t {
    None = 0,                     ///< Identical configurations
    Immediate = 1,                ///< Dynamic state, push constants, or UBO writes
    RenderResourceRecreation = 2, ///< Texture recreation (e.g. shadow map, HDR target)
    PipelineRecompilation = 3,    ///< Graphics pipeline rebuild (e.g. polygon mode)
    SwapchainRecreation = 4,      ///< VSync present mode switch
    RestartRequired = 5,          ///< Fundamental architectural changes
};

/// @struct QualitySettings
/// @brief Scalability settings controlling resolution, filtering, and geometry density.
struct QualitySettings {
    GraphicsQualityPreset preset{GraphicsQualityPreset::High};
    float renderScale{1.0F}; ///< Viewport scale factor in range [0.5, 2.0]
    AnisotropicFiltering anisotropicFiltering{AnisotropicFiltering::X16};
    AntiAliasingMode antiAliasing{AntiAliasingMode::None};
    TextureQuality textureQuality{TextureQuality::High};

    bool operator==(const QualitySettings& other) const noexcept = default;
};

/// @struct LightingSettings
/// @brief Illumination, environment, and exposure parameters.
struct LightingSettings {
    bool directLighting{true};
    bool iblEnabled{false};           ///< Image-Based Lighting (Milestone 11C+)
    float environmentIntensity{1.0F}; ///< Ambient light scale in range [0.0, 10.0]
    bool indirectLighting{false};     ///< Diffuse GI (Milestone 11C+)
    float exposure{1.0F};             ///< Photometric camera exposure in range [0.01, 100.0]
    float whiteBalance{6500.0F};      ///< Correlated color temperature in Kelvin [2000, 12000]
    EnvironmentTimeOfDay timeOfDay{
        EnvironmentTimeOfDay::Daylight}; ///< Atmospheric condition (Milestone 11C+)

    // Milestone 11D: Global Illumination & Ambient Occlusion controls
    bool gtaoEnabled{false};    ///< Ground-Truth Ambient Occlusion (Milestone 11D)
    float gtaoRadius{1.5F};     ///< GTAO world-space search radius in meters [0.2, 10.0]
    float gtaoIntensity{1.0F};  ///< GTAO occlusion power multiplier [0.0, 5.0]
    bool ssgiEnabled{false};    ///< Screen-Space Global Illumination (Milestone 11D)
    float ssgiRadius{3.0F};     ///< SSGI max ray march distance in meters [0.5, 20.0]
    float giIntensity{1.0F};    ///< Diffuse indirect bounce radiance multiplier [0.0, 5.0]
    bool temporalDenoise{true}; ///< Multi-frame temporal accumulation and history clamping
    bool probesEnabled{false};  ///< Scene-aware Spherical Harmonics irradiance probes

    bool operator==(const LightingSettings& other) const noexcept = default;
};

/// @struct ShadowSettings
/// @brief Directional shadow depth mapping, resolution, and filtering parameters.
struct ShadowSettings {
    bool enabled{true};
    uint32_t resolution{2048};   ///< Shadow map dimension: 512, 1024, 2048, or 4096
    float distance{50.0F};       ///< Directional shadow projection distance in meters [10.0, 300.0]
    float depthBias{0.005F};     ///< Constant depth bias in range [0.0001, 0.05]
    float slopeScaledBias{1.0F}; ///< Slope-scaled depth bias multiplier [0.0, 5.0]
    ShadowPcfQuality pcfQuality{ShadowPcfQuality::Medium};
    bool softShadows{true};

    // Milestone 11E: Cascaded Shadow Maps, Contact Shadows & Shadow Stability
    uint32_t cascadeCount{4};          ///< Number of shadow cascades [1, 4]
    float splitLambda{0.80F};          ///< Logarithmic vs uniform split distribution factor [0.0, 1.0]
    bool stabilizeCascades{true};      ///< Sub-texel projection snapping to eliminate edge shimmering
    bool contactShadows{false};        ///< Screen-space ray-marched contact shadows for fine contact grounding
    float contactShadowDistance{0.6F}; ///< Contact shadow search distance in meters [0.1, 2.0]
    bool shadowCaching{true};          ///< Static shadow caching to eliminate redundant draw-call recording
    float cascadeBlendRange{0.10F};    ///< Cascade boundary overlap blend fraction [0.0, 0.3]

    bool operator==(const ShadowSettings& other) const noexcept = default;
};

/// @struct PerformanceSettings
/// @brief Presentation synchronization, frame rate caps, and telemetry toggles.
struct PerformanceSettings {
    bool vsync{false};
    uint32_t frameRateLimit{0}; ///< Target FPS cap (0 = uncapped)
    bool dynamicResolution{false};
    bool gpuProfiling{true};
    bool cpuProfiling{true};
    bool renderStats{true};
    bool frustumCulling{true}; ///< CPU world-space frustum culling for camera and shadows
    bool depthPrepass{false};  ///< Early depth prepass before main PBR rasterization

    bool operator==(const PerformanceSettings& other) const noexcept = default;
};

/// @struct DebugSettings
/// @brief Diagnostic views and visual debugging modes.
struct DebugSettings {
    DebugViewMode viewMode{DebugViewMode::Lit};
    bool visualizeShadowMap{false};
    bool showPassTimings{true};

    bool operator==(const DebugSettings& other) const noexcept = default;
};

/// @struct GraphicsSettings
/// @brief Master centralized configuration model for Hikari Engine rendering.
struct GraphicsSettings {
    uint32_t version{1};
    QualitySettings quality{};
    LightingSettings lighting{};
    ShadowSettings shadows{};
    PostProcessSettings postProcess{};
    PerformanceSettings performance{};
    DebugSettings debug{};

    /// Returns the standardized GraphicsSettings configuration for a predefined quality tier.
    [[nodiscard]] static GraphicsSettings createPreset(GraphicsQualityPreset preset) noexcept;

    /// Evaluates current settings against standard presets and returns the matching preset,
    /// or GraphicsQualityPreset::Custom if individual settings diverge.
    [[nodiscard]] GraphicsQualityPreset detectPreset() const noexcept;

    /// Overwrites settings with the defined preset configuration and updates quality.preset.
    void applyPreset(GraphicsQualityPreset preset) noexcept;

    /// Validates requested settings against active GPU hardware capabilities and returns
    /// an effective configuration with unsupported options clamped or safely fallen back.
    [[nodiscard]] GraphicsSettings validateAgainstCaps(const rhi::GpuCaps& caps) const noexcept;

    /// Computes the maximum change severity between this configuration and another.
    [[nodiscard]] SettingChangeSeverity
    computeChangeSeverity(const GraphicsSettings& other) const noexcept;

    /// Formats a human-readable name for a quality preset.
    [[nodiscard]] static std::string_view getPresetName(GraphicsQualityPreset preset) noexcept;

    bool operator==(const GraphicsSettings& other) const noexcept;
    bool operator!=(const GraphicsSettings& other) const noexcept { return !(*this == other); }
};

} // namespace eng::render
