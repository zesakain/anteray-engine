#pragma once

#include <cstdint>
#include <string>

namespace eng::render {

/// @enum ToneMapOperator
/// @brief Available tone mapping operators mapping high dynamic range (HDR) radiance values to
/// display-referred dynamic range.
enum class ToneMapOperator : uint8_t {
    /// Troy Sobotka AgX tone mapper providing natural highlight roll-off and chroma preservation.
    AgX = 0,

    /// Fitted ACES curve (Stephen Hill RRT+ODT fit) providing cinematic contrast and highlight
    /// roll-off.
    AcesFitted = 1,

    /// Filmic extended Reinhard curve with white point preservation.
    FilmicReinhard = 2,

    /// Khronos PBR Neutral curve preventing color desaturation, hue shifts, and highlight burnout.
    Neutral = 3,

    /// Linear bypass: no non-linear tonemapping curve applied (clamped to [0, 1]).
    Bypass = 4,
};

/// @struct PostProcessSettings
/// @brief Runtime configuration for cinematic post-processing, tone mapping, exposure adaptation,
/// depth of field, motion blur, color grading, bloom, vignette, and film grain.
///
/// Ownership and lifetime: Plain value struct; copied freely across rendering interfaces.
/// Thread-safety: Thread-independent data value.
/// Cost: Zero hot-path allocation overhead when `lutFilePath` is unchanged.
struct PostProcessSettings {
    /// Master toggle for post-processing pipeline. When false, scene renders directly to standard
    /// low dynamic range target.
    bool enabled{false};

    /// Linear photometric exposure multiplier applied prior to tone mapping.
    /// Range: [0.001, 100.0], default: 1.0.
    float exposure{1.0F};

    /// Selected tone reproduction operator. Default: AgX.
    ToneMapOperator toneMapOp{ToneMapOperator::AgX};

    /// Master toggle for bloom bright extraction and multi-scale blur pyramid.
    bool bloomEnabled{true};

    /// Luminance threshold for bright pass extraction.
    /// Range: [0.0, 50.0], default: 1.0.
    float bloomThreshold{1.0F};

    /// Soft-knee transition half-width for bloom extraction.
    /// Range: [0.0, 2.0], default: 0.5.
    float bloomKnee{0.5F};

    /// Configurable white point luminance for Filmic Reinhard tone mapping.
    /// Range: [1.0, 50.0], default: 4.0.
    float reinhardWhitePoint{4.0F};

    /// Scale factor applied to the blurred bloom pyramid during final composite.
    /// Range: [0.0, 1.0], default: 0.04.
    float bloomIntensity{0.04F};

    /// 9-tap tent blur reconstruction filter radius.
    /// Range: [0.5, 4.0], default: 1.0.
    float bloomFilterRadius{1.0F};

    /// Correlated color temperature in Kelvin for Planckian chromatic adaptation.
    /// Range: [1000.0, 12000.0], default: 6500.0 (D65 standard daylight).
    float whiteBalance{6500.0F};

    /// Midtone contrast adjustment factor around 18% gray in linear space.
    /// Range: [0.5, 2.0], default: 1.0 (neutral).
    float contrast{1.0F};

    /// Color saturation multiplier.
    /// Range: [0.0, 2.0], default: 1.0 (neutral).
    float saturation{1.0F};

    // --- Milestone 11I: Exposure Adaptation (Auto-Exposure) ---

    /// Master toggle for temporal luminance exposure adaptation.
    bool autoExposureEnabled{false};

    /// Minimum allowed exposure value (EV100 offset in stops) for auto-exposure clamping.
    /// Range: [-10.0, 10.0], default: -4.0.
    float autoExposureMinEv{-4.0F};

    /// Maximum allowed exposure value (EV100 offset in stops) for auto-exposure clamping.
    /// Range: [-10.0, 10.0], default: 4.0.
    float autoExposureMaxEv{4.0F};

    /// Temporal adaptation rate (1/seconds) when adapting from dark to bright scenes.
    /// Range: [0.1, 20.0], default: 3.0.
    float autoExposureSpeedUp{3.0F};

    /// Temporal adaptation rate (1/seconds) when adapting from bright to dark scenes.
    /// Range: [0.1, 20.0], default: 1.5.
    float autoExposureSpeedDown{1.5F};

    /// Exposure compensation bias in stops (EV) applied on top of manual or auto exposure.
    /// Range: [-5.0, 5.0], default: 0.0.
    float exposureCompensation{0.0F};

    // --- Milestone 11I: Depth of Field (DoF) ---

    /// Master toggle for physically-based Circle-of-Confusion (CoC) Depth of Field pass.
    bool dofEnabled{false};

    /// Distance from the camera to the plane of sharp focus, in meters.
    /// Range: [0.1, 1000.0], default: 10.0.
    float dofFocusDistance{10.0F};

    /// Lens focal length in millimeters controlling depth of field thickness.
    /// Range: [10.0, 300.0], default: 50.0.
    float dofFocalLength{50.0F};

    /// Lens f-stop aperture number (f/N). Smaller values produce shallower depth of field.
    /// Range: [1.0, 32.0], default: 2.8.
    float dofAperture{2.8F};

    /// Maximum Circle-of-Confusion blur radius in pixels at reference resolution.
    /// Range: [1.0, 16.0], default: 8.0.
    float dofMaxBlurRadius{8.0F};

    // --- Milestone 11I: Motion Blur ---

    /// Master toggle for velocity-aware directional motion blur pass.
    bool motionBlurEnabled{false};

    /// Shutter-angle scale factor applied to screen-space velocity vectors.
    /// Range: [0.0, 2.0], default: 0.5.
    float motionBlurIntensity{0.5F};

    /// Number of reconstruction taps along the motion vector (4 to 16).
    /// Range: [4, 16], default: 8.
    uint32_t motionBlurSamples{8};

    // --- Milestone 11I: Color Grading LUT ---

    /// Master toggle for 3D LUT color grading (256x16 unwrapped 16x16x16 LUT strip).
    bool lutEnabled{false};

    /// Blend weight between ungraded tone-mapped color (0.0) and LUT-graded color (1.0).
    /// Range: [0.0, 1.0], default: 1.0.
    float lutIntensity{1.0F};

    /// Optional asset path to a 256x16 RGBA8 color grading LUT strip texture.
    /// When empty or unset, falls back to the built-in neutral identity LUT.
    std::string lutFilePath{};

    // --- Milestone 11I: Vignette ---

    /// Master toggle for optical lens vignette darkening toward screen edges.
    bool vignetteEnabled{false};

    /// Strength of peripheral darkening at screen corners.
    /// Range: [0.0, 1.0], default: 0.25.
    float vignetteIntensity{0.25F};

    /// Falloff feathering smoothness of the vignette transition.
    /// Range: [0.05, 1.0], default: 0.5.
    float vignetteSmoothness{0.5F};

    /// Aspect-ratio roundness factor (1.0 = circular, 0.0 = screen-aspect oval).
    /// Range: [0.0, 1.0], default: 1.0.
    float vignetteRoundness{1.0F};

    // --- Milestone 11I: Film Grain ---

    /// Master toggle for temporally-stable luminance-weighted cinematic film grain.
    bool filmGrainEnabled{false};

    /// Amplitude of film grain noise modulation in display space.
    /// Range: [0.0, 0.25], default: 0.03.
    float filmGrainIntensity{0.03F};

    bool operator==(const PostProcessSettings& other) const noexcept = default;
};

} // namespace eng::render
