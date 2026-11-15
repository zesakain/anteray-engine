/// @file graphics_settings_serializer.cpp
/// @brief Implementation of transactional JSON serialization for GraphicsSettings.

#include "render/graphics_settings_serializer.h"

#include "core/json.h"
#include "core/log.h"

#include <fstream>
#include <sstream>

namespace eng::render {

namespace {

using namespace core;

std::string_view presetToString(GraphicsQualityPreset p) noexcept {
    switch (p) {
    case GraphicsQualityPreset::Low:
        return "Low";
    case GraphicsQualityPreset::Medium:
        return "Medium";
    case GraphicsQualityPreset::High:
        return "High";
    case GraphicsQualityPreset::Ultra:
        return "Ultra";
    case GraphicsQualityPreset::Cinematic:
        return "Cinematic";
    case GraphicsQualityPreset::Custom:
        return "Custom";
    }
    return "High";
}

GraphicsQualityPreset stringToPreset(std::string_view s) noexcept {
    if (s == "Low")
        return GraphicsQualityPreset::Low;
    if (s == "Medium")
        return GraphicsQualityPreset::Medium;
    if (s == "High")
        return GraphicsQualityPreset::High;
    if (s == "Ultra")
        return GraphicsQualityPreset::Ultra;
    if (s == "Cinematic")
        return GraphicsQualityPreset::Cinematic;
    if (s == "Custom")
        return GraphicsQualityPreset::Custom;
    return GraphicsQualityPreset::High;
}

std::string_view textureQualityToString(TextureQuality q) noexcept {
    switch (q) {
    case TextureQuality::Low:
        return "Low";
    case TextureQuality::Medium:
        return "Medium";
    case TextureQuality::High:
        return "High";
    case TextureQuality::Ultra:
        return "Ultra";
    }
    return "High";
}

TextureQuality stringToTextureQuality(std::string_view s) noexcept {
    if (s == "Low")
        return TextureQuality::Low;
    if (s == "Medium")
        return TextureQuality::Medium;
    if (s == "High")
        return TextureQuality::High;
    if (s == "Ultra")
        return TextureQuality::Ultra;
    return TextureQuality::High;
}

std::string_view aaToString(AntiAliasingMode m) noexcept {
    switch (m) {
    case AntiAliasingMode::None:
        return "None";
    case AntiAliasingMode::FXAA:
        return "FXAA";
    case AntiAliasingMode::TAA:
        return "TAA";
    case AntiAliasingMode::MSAA2x:
        return "MSAA2x";
    case AntiAliasingMode::MSAA4x:
        return "MSAA4x";
    }
    return "None";
}

AntiAliasingMode stringToAntiAliasing(std::string_view s) noexcept {
    if (s == "FXAA")
        return AntiAliasingMode::FXAA;
    if (s == "TAA")
        return AntiAliasingMode::TAA;
    if (s == "MSAA2x")
        return AntiAliasingMode::MSAA2x;
    if (s == "MSAA4x")
        return AntiAliasingMode::MSAA4x;
    return AntiAliasingMode::None;
}

std::string_view taaJitterToString(TaaJitterSequence s) noexcept {
    switch (s) {
    case TaaJitterSequence::Halton8:
        return "Halton8";
    case TaaJitterSequence::Halton16:
        return "Halton16";
    case TaaJitterSequence::Halton32:
        return "Halton32";
    }
    return "Halton16";
}

TaaJitterSequence stringToTaaJitter(std::string_view s) noexcept {
    if (s == "Halton8" || s == "8")
        return TaaJitterSequence::Halton8;
    if (s == "Halton32" || s == "32")
        return TaaJitterSequence::Halton32;
    return TaaJitterSequence::Halton16;
}

std::string_view pcfToString(ShadowPcfQuality q) noexcept {
    switch (q) {
    case ShadowPcfQuality::Hard:
        return "Hard";
    case ShadowPcfQuality::Medium:
        return "Medium";
    case ShadowPcfQuality::High:
        return "High";
    }
    return "Medium";
}

ShadowPcfQuality stringToPcf(std::string_view s) noexcept {
    if (s == "Hard")
        return ShadowPcfQuality::Hard;
    if (s == "High")
        return ShadowPcfQuality::High;
    return ShadowPcfQuality::Medium;
}

std::string_view toneMapToString(ToneMapOperator op) noexcept {
    switch (op) {
    case ToneMapOperator::AgX:
        return "AgX";
    case ToneMapOperator::AcesFitted:
        return "AcesFitted";
    case ToneMapOperator::FilmicReinhard:
        return "FilmicReinhard";
    case ToneMapOperator::Neutral:
        return "Neutral";
    case ToneMapOperator::Bypass:
        return "Bypass";
    }
    return "AgX";
}

ToneMapOperator stringToToneMap(std::string_view s) noexcept {
    if (s == "AgX" || s == "agx")
        return ToneMapOperator::AgX;
    if (s == "AcesFitted" || s == "ACES" || s == "aces")
        return ToneMapOperator::AcesFitted;
    if (s == "FilmicReinhard" || s == "Reinhard" || s == "reinhard")
        return ToneMapOperator::FilmicReinhard;
    if (s == "Neutral" || s == "neutral")
        return ToneMapOperator::Neutral;
    if (s == "Bypass" || s == "bypass")
        return ToneMapOperator::Bypass;
    return ToneMapOperator::AgX;
}

std::string_view timeOfDayToString(EnvironmentTimeOfDay tod) noexcept {
    switch (tod) {
    case EnvironmentTimeOfDay::Daylight:
        return "Daylight";
    case EnvironmentTimeOfDay::Sunset:
        return "Sunset";
    case EnvironmentTimeOfDay::Night:
        return "Night";
    case EnvironmentTimeOfDay::Custom:
        return "Custom";
    }
    return "Daylight";
}

EnvironmentTimeOfDay stringToTimeOfDay(std::string_view s) noexcept {
    if (s == "Sunset")
        return EnvironmentTimeOfDay::Sunset;
    if (s == "Night")
        return EnvironmentTimeOfDay::Night;
    if (s == "Custom")
        return EnvironmentTimeOfDay::Custom;
    return EnvironmentTimeOfDay::Daylight;
}

std::string_view debugViewToString(DebugViewMode m) noexcept {
    switch (m) {
    case DebugViewMode::Lit:
        return "Lit";
    case DebugViewMode::Wireframe:
        return "Wireframe";
    case DebugViewMode::Normals:
        return "Normals";
    case DebugViewMode::Albedo:
        return "Albedo";
    case DebugViewMode::Roughness:
        return "Roughness";
    case DebugViewMode::Metallic:
        return "Metallic";
    case DebugViewMode::ShadowMap:
        return "ShadowMap";
    case DebugViewMode::LightingComplexity:
        return "LightingComplexity";
    }
    return "Lit";
}

DebugViewMode stringToDebugView(std::string_view s) noexcept {
    if (s == "Wireframe")
        return DebugViewMode::Wireframe;
    if (s == "Normals")
        return DebugViewMode::Normals;
    if (s == "Albedo")
        return DebugViewMode::Albedo;
    if (s == "Roughness")
        return DebugViewMode::Roughness;
    if (s == "Metallic")
        return DebugViewMode::Metallic;
    if (s == "ShadowMap")
        return DebugViewMode::ShadowMap;
    if (s == "LightingComplexity")
        return DebugViewMode::LightingComplexity;
    return DebugViewMode::Lit;
}

} // namespace

Result<std::string> GraphicsSettingsSerializer::serializeToString(const GraphicsSettings& settings,
                                                                  bool prettyPrint,
                                                                  int indentSpaces) {
    JsonValue root = JsonValue::makeObject();
    root.set("formatVersion", JsonValue(static_cast<int>(kCurrentVersion)));

    // Quality
    JsonValue qualityObj = JsonValue::makeObject();
    qualityObj.set("preset", JsonValue(presetToString(settings.quality.preset)));
    qualityObj.set("renderScale", JsonValue(static_cast<double>(settings.quality.renderScale)));
    qualityObj.set("anisotropicFiltering",
                   JsonValue(static_cast<int>(settings.quality.anisotropicFiltering)));
    qualityObj.set("antiAliasing", JsonValue(aaToString(settings.quality.antiAliasing)));
    qualityObj.set("textureQuality",
                   JsonValue(textureQualityToString(settings.quality.textureQuality)));

    JsonValue taaObj = JsonValue::makeObject();
    taaObj.set("enabled", JsonValue(settings.quality.taa.enabled));
    taaObj.set("historyWeight", JsonValue(static_cast<double>(settings.quality.taa.historyWeight)));
    taaObj.set("varianceGamma", JsonValue(static_cast<double>(settings.quality.taa.varianceGamma)));
    taaObj.set("jitterSequence", JsonValue(taaJitterToString(settings.quality.taa.jitterSequence)));
    taaObj.set("sharpeningEnabled", JsonValue(settings.quality.taa.sharpeningEnabled));
    taaObj.set("sharpeningIntensity",
               JsonValue(static_cast<double>(settings.quality.taa.sharpeningIntensity)));
    taaObj.set("useMotionVectors", JsonValue(settings.quality.taa.useMotionVectors));
    qualityObj.set("taa", std::move(taaObj));

    root.set("quality", std::move(qualityObj));

    // Lighting
    JsonValue lightingObj = JsonValue::makeObject();
    lightingObj.set("directLighting", JsonValue(settings.lighting.directLighting));
    lightingObj.set("iblEnabled", JsonValue(settings.lighting.iblEnabled));
    lightingObj.set("environmentIntensity",
                    JsonValue(static_cast<double>(settings.lighting.environmentIntensity)));
    lightingObj.set("indirectLighting", JsonValue(settings.lighting.indirectLighting));
    lightingObj.set("gtaoEnabled", JsonValue(settings.lighting.gtaoEnabled));
    lightingObj.set("gtaoRadius", JsonValue(static_cast<double>(settings.lighting.gtaoRadius)));
    lightingObj.set("gtaoIntensity",
                    JsonValue(static_cast<double>(settings.lighting.gtaoIntensity)));
    lightingObj.set("ssgiEnabled", JsonValue(settings.lighting.ssgiEnabled));
    lightingObj.set("ssgiRadius", JsonValue(static_cast<double>(settings.lighting.ssgiRadius)));
    lightingObj.set("giIntensity", JsonValue(static_cast<double>(settings.lighting.giIntensity)));
    lightingObj.set("temporalDenoise", JsonValue(settings.lighting.temporalDenoise));
    lightingObj.set("probesEnabled", JsonValue(settings.lighting.probesEnabled));
    lightingObj.set("ssrEnabled", JsonValue(settings.lighting.ssrEnabled));
    lightingObj.set("ssrRoughnessCutoff",
                    JsonValue(static_cast<double>(settings.lighting.ssrRoughnessCutoff)));
    lightingObj.set("ssrMaxDistance",
                    JsonValue(static_cast<double>(settings.lighting.ssrMaxDistance)));
    lightingObj.set("ssrIntensity", JsonValue(static_cast<double>(settings.lighting.ssrIntensity)));
    lightingObj.set("reflectionProbeBlending",
                    JsonValue(settings.lighting.reflectionProbeBlending));
    lightingObj.set("exposure", JsonValue(static_cast<double>(settings.lighting.exposure)));
    lightingObj.set("whiteBalance", JsonValue(static_cast<double>(settings.lighting.whiteBalance)));
    lightingObj.set("timeOfDay", JsonValue(timeOfDayToString(settings.lighting.timeOfDay)));
    root.set("lighting", std::move(lightingObj));

    // Shadows
    JsonValue shadowObj = JsonValue::makeObject();
    shadowObj.set("enabled", JsonValue(settings.shadows.enabled));
    shadowObj.set("resolution", JsonValue(static_cast<int>(settings.shadows.resolution)));
    shadowObj.set("distance", JsonValue(static_cast<double>(settings.shadows.distance)));
    shadowObj.set("depthBias", JsonValue(static_cast<double>(settings.shadows.depthBias)));
    shadowObj.set("slopeScaledBias",
                  JsonValue(static_cast<double>(settings.shadows.slopeScaledBias)));
    shadowObj.set("pcfQuality", JsonValue(pcfToString(settings.shadows.pcfQuality)));
    shadowObj.set("softShadows", JsonValue(settings.shadows.softShadows));
    shadowObj.set("cascadeCount", JsonValue(static_cast<int>(settings.shadows.cascadeCount)));
    shadowObj.set("splitLambda", JsonValue(static_cast<double>(settings.shadows.splitLambda)));
    shadowObj.set("stabilizeCascades", JsonValue(settings.shadows.stabilizeCascades));
    shadowObj.set("contactShadows", JsonValue(settings.shadows.contactShadows));
    shadowObj.set("contactShadowDistance",
                  JsonValue(static_cast<double>(settings.shadows.contactShadowDistance)));
    shadowObj.set("shadowCaching", JsonValue(settings.shadows.shadowCaching));
    shadowObj.set("cascadeBlendRange",
                  JsonValue(static_cast<double>(settings.shadows.cascadeBlendRange)));
    root.set("shadows", std::move(shadowObj));

    // Post Processing
    JsonValue postObj = JsonValue::makeObject();
    postObj.set("enabled", JsonValue(settings.postProcess.enabled));
    postObj.set("toneMapOp", JsonValue(toneMapToString(settings.postProcess.toneMapOp)));
    postObj.set("exposure", JsonValue(static_cast<double>(settings.postProcess.exposure)));
    postObj.set("reinhardWhitePoint",
                JsonValue(static_cast<double>(settings.postProcess.reinhardWhitePoint)));
    postObj.set("autoExposureEnabled", JsonValue(settings.postProcess.autoExposureEnabled));
    postObj.set("autoExposureMinEv",
                JsonValue(static_cast<double>(settings.postProcess.autoExposureMinEv)));
    postObj.set("autoExposureMaxEv",
                JsonValue(static_cast<double>(settings.postProcess.autoExposureMaxEv)));
    postObj.set("autoExposureSpeedUp",
                JsonValue(static_cast<double>(settings.postProcess.autoExposureSpeedUp)));
    postObj.set("autoExposureSpeedDown",
                JsonValue(static_cast<double>(settings.postProcess.autoExposureSpeedDown)));
    postObj.set("exposureCompensation",
                JsonValue(static_cast<double>(settings.postProcess.exposureCompensation)));
    postObj.set("bloomEnabled", JsonValue(settings.postProcess.bloomEnabled));
    postObj.set("bloomThreshold",
                JsonValue(static_cast<double>(settings.postProcess.bloomThreshold)));
    postObj.set("bloomKnee", JsonValue(static_cast<double>(settings.postProcess.bloomKnee)));
    postObj.set("bloomIntensity",
                JsonValue(static_cast<double>(settings.postProcess.bloomIntensity)));
    postObj.set("bloomFilterRadius",
                JsonValue(static_cast<double>(settings.postProcess.bloomFilterRadius)));
    postObj.set("dofEnabled", JsonValue(settings.postProcess.dofEnabled));
    postObj.set("dofFocusDistance",
                JsonValue(static_cast<double>(settings.postProcess.dofFocusDistance)));
    postObj.set("dofFocalLength",
                JsonValue(static_cast<double>(settings.postProcess.dofFocalLength)));
    postObj.set("dofAperture", JsonValue(static_cast<double>(settings.postProcess.dofAperture)));
    postObj.set("dofMaxBlurRadius",
                JsonValue(static_cast<double>(settings.postProcess.dofMaxBlurRadius)));
    postObj.set("motionBlurEnabled", JsonValue(settings.postProcess.motionBlurEnabled));
    postObj.set("motionBlurIntensity",
                JsonValue(static_cast<double>(settings.postProcess.motionBlurIntensity)));
    postObj.set("motionBlurSamples",
                JsonValue(static_cast<int>(settings.postProcess.motionBlurSamples)));
    postObj.set("whiteBalance", JsonValue(static_cast<double>(settings.postProcess.whiteBalance)));
    postObj.set("contrast", JsonValue(static_cast<double>(settings.postProcess.contrast)));
    postObj.set("saturation", JsonValue(static_cast<double>(settings.postProcess.saturation)));
    postObj.set("lutEnabled", JsonValue(settings.postProcess.lutEnabled));
    postObj.set("lutIntensity", JsonValue(static_cast<double>(settings.postProcess.lutIntensity)));
    postObj.set("lutFilePath", JsonValue(settings.postProcess.lutFilePath));
    postObj.set("vignetteEnabled", JsonValue(settings.postProcess.vignetteEnabled));
    postObj.set("vignetteIntensity",
                JsonValue(static_cast<double>(settings.postProcess.vignetteIntensity)));
    postObj.set("vignetteSmoothness",
                JsonValue(static_cast<double>(settings.postProcess.vignetteSmoothness)));
    postObj.set("vignetteRoundness",
                JsonValue(static_cast<double>(settings.postProcess.vignetteRoundness)));
    postObj.set("filmGrainEnabled", JsonValue(settings.postProcess.filmGrainEnabled));
    postObj.set("filmGrainIntensity",
                JsonValue(static_cast<double>(settings.postProcess.filmGrainIntensity)));
    root.set("postProcess", std::move(postObj));

    // Performance
    JsonValue perfObj = JsonValue::makeObject();
    perfObj.set("vsync", JsonValue(settings.performance.vsync));
    perfObj.set("frameRateLimit", JsonValue(static_cast<int>(settings.performance.frameRateLimit)));
    perfObj.set("dynamicResolution", JsonValue(settings.performance.dynamicResolution));
    perfObj.set("gpuProfiling", JsonValue(settings.performance.gpuProfiling));
    perfObj.set("cpuProfiling", JsonValue(settings.performance.cpuProfiling));
    perfObj.set("renderStats", JsonValue(settings.performance.renderStats));
    perfObj.set("frustumCulling", JsonValue(settings.performance.frustumCulling));
    perfObj.set("depthPrepass", JsonValue(settings.performance.depthPrepass));
    root.set("performance", std::move(perfObj));

    // Debug
    JsonValue debugObj = JsonValue::makeObject();
    debugObj.set("viewMode", JsonValue(debugViewToString(settings.debug.viewMode)));
    debugObj.set("visualizeShadowMap", JsonValue(settings.debug.visualizeShadowMap));
    debugObj.set("showPassTimings", JsonValue(settings.debug.showPassTimings));
    root.set("debug", std::move(debugObj));

    return Result<std::string>::success(root.serialize(prettyPrint ? indentSpaces : 0));
}

Result<void> GraphicsSettingsSerializer::serializeToFile(const GraphicsSettings& settings,
                                                         const std::filesystem::path& path,
                                                         bool prettyPrint, int indentSpaces) {
    auto strRes = serializeToString(settings, prettyPrint, indentSpaces);
    if (!strRes.hasValue()) {
        return strRes.error();
    }

    std::filesystem::path parentDir = path.parent_path();
    if (!parentDir.empty() && !std::filesystem::exists(parentDir)) {
        std::error_code ec;
        std::filesystem::create_directories(parentDir, ec);
        if (ec) {
            return Error{.code = ErrorCode::InvalidArgument,
                         .message = "Failed to create target directories for settings file"};
        }
    }

    std::filesystem::path tmpPath = path;
    tmpPath += ".tmp";

    {
        std::ofstream ofs(tmpPath, std::ios::out | std::ios::trunc);
        if (!ofs.is_open()) {
            return Error{.code = ErrorCode::DeviceCreationFailed,
                         .message = "Failed to open temporary settings file for writing"};
        }
        ofs << strRes.value();
        if (ofs.fail()) {
            return Error{.code = ErrorCode::DeviceCreationFailed,
                         .message = "Failed writing serialized settings stream"};
        }
    }

    std::error_code renameEc;
    std::filesystem::rename(tmpPath, path, renameEc);
    if (renameEc) {
        return Error{.code = ErrorCode::DeviceCreationFailed,
                     .message = "Atomic rename of graphics settings failed: " + renameEc.message()};
    }

    return Result<void>::success();
}

Result<GraphicsSettings>
GraphicsSettingsSerializer::deserializeFromString(std::string_view jsonText) {
    auto parseRes = JsonValue::parse(jsonText);
    if (!parseRes.hasValue()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = std::string("JSON syntax error parsing graphics settings: ") +
                                std::string(parseRes.error().message)};
    }

    const auto& root = parseRes.value();
    if (!root.isObject()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Root of graphics settings JSON must be an object"};
    }

    // Baseline default settings (High tier)
    GraphicsSettings settings = GraphicsSettings::createPreset(GraphicsQualityPreset::High);

    if (root.contains("formatVersion")) {
        settings.version = static_cast<uint32_t>(root.getInt("formatVersion", 1));
    }

    // 1. Quality
    if (const auto* q = root.find("quality"); q != nullptr && q->isObject()) {
        if (q->contains("preset")) {
            settings.quality.preset = stringToPreset(q->getString("preset", "High"));
        }
        settings.quality.renderScale = q->getFloat("renderScale", settings.quality.renderScale);
        if (q->contains("anisotropicFiltering")) {
            int aniso = q->getInt("anisotropicFiltering", 16);
            if (aniso == 0)
                settings.quality.anisotropicFiltering = AnisotropicFiltering::Off;
            else if (aniso <= 2)
                settings.quality.anisotropicFiltering = AnisotropicFiltering::X2;
            else if (aniso <= 4)
                settings.quality.anisotropicFiltering = AnisotropicFiltering::X4;
            else if (aniso <= 8)
                settings.quality.anisotropicFiltering = AnisotropicFiltering::X8;
            else
                settings.quality.anisotropicFiltering = AnisotropicFiltering::X16;
        }
        if (q->contains("antiAliasing")) {
            settings.quality.antiAliasing =
                stringToAntiAliasing(q->getString("antiAliasing", "None"));
        }
        if (q->contains("textureQuality")) {
            settings.quality.textureQuality =
                stringToTextureQuality(q->getString("textureQuality", "High"));
        }
        if (const auto* taa = q->find("taa"); taa != nullptr && taa->isObject()) {
            settings.quality.taa.enabled = taa->getBool("enabled", settings.quality.taa.enabled);
            settings.quality.taa.historyWeight =
                taa->getFloat("historyWeight", settings.quality.taa.historyWeight);
            settings.quality.taa.varianceGamma =
                taa->getFloat("varianceGamma", settings.quality.taa.varianceGamma);
            if (taa->contains("jitterSequence")) {
                settings.quality.taa.jitterSequence =
                    stringToTaaJitter(taa->getString("jitterSequence", "Halton16"));
            }
            settings.quality.taa.sharpeningEnabled =
                taa->getBool("sharpeningEnabled", settings.quality.taa.sharpeningEnabled);
            settings.quality.taa.sharpeningIntensity =
                taa->getFloat("sharpeningIntensity", settings.quality.taa.sharpeningIntensity);
            settings.quality.taa.useMotionVectors =
                taa->getBool("useMotionVectors", settings.quality.taa.useMotionVectors);
        } else if (settings.quality.antiAliasing == AntiAliasingMode::TAA) {
            settings.quality.taa.enabled = true;
        } else {
            settings.quality.taa.enabled = false;
        }
    }

    // 2. Lighting
    if (const auto* l = root.find("lighting"); l != nullptr && l->isObject()) {
        settings.lighting.directLighting =
            l->getBool("directLighting", settings.lighting.directLighting);
        settings.lighting.iblEnabled = l->getBool("iblEnabled", settings.lighting.iblEnabled);
        settings.lighting.environmentIntensity =
            l->getFloat("environmentIntensity", settings.lighting.environmentIntensity);
        settings.lighting.indirectLighting =
            l->getBool("indirectLighting", settings.lighting.indirectLighting);
        settings.lighting.gtaoEnabled = l->getBool("gtaoEnabled", settings.lighting.gtaoEnabled);
        settings.lighting.gtaoRadius = l->getFloat("gtaoRadius", settings.lighting.gtaoRadius);
        settings.lighting.gtaoIntensity =
            l->getFloat("gtaoIntensity", settings.lighting.gtaoIntensity);
        settings.lighting.ssgiEnabled = l->getBool("ssgiEnabled", settings.lighting.ssgiEnabled);
        settings.lighting.ssgiRadius = l->getFloat("ssgiRadius", settings.lighting.ssgiRadius);
        settings.lighting.giIntensity = l->getFloat("giIntensity", settings.lighting.giIntensity);
        settings.lighting.temporalDenoise =
            l->getBool("temporalDenoise", settings.lighting.temporalDenoise);
        settings.lighting.probesEnabled =
            l->getBool("probesEnabled", settings.lighting.probesEnabled);
        settings.lighting.ssrEnabled = l->getBool("ssrEnabled", settings.lighting.ssrEnabled);
        settings.lighting.ssrRoughnessCutoff =
            l->getFloat("ssrRoughnessCutoff", settings.lighting.ssrRoughnessCutoff);
        settings.lighting.ssrMaxDistance =
            l->getFloat("ssrMaxDistance", settings.lighting.ssrMaxDistance);
        settings.lighting.ssrIntensity =
            l->getFloat("ssrIntensity", settings.lighting.ssrIntensity);
        settings.lighting.reflectionProbeBlending =
            l->getBool("reflectionProbeBlending", settings.lighting.reflectionProbeBlending);
        settings.lighting.exposure = l->getFloat("exposure", settings.lighting.exposure);
        settings.lighting.whiteBalance =
            l->getFloat("whiteBalance", settings.lighting.whiteBalance);
        if (l->contains("timeOfDay")) {
            settings.lighting.timeOfDay = stringToTimeOfDay(l->getString("timeOfDay", "Daylight"));
        }
    }

    // 3. Shadows
    if (const auto* s = root.find("shadows"); s != nullptr && s->isObject()) {
        settings.shadows.enabled = s->getBool("enabled", settings.shadows.enabled);
        settings.shadows.resolution = static_cast<uint32_t>(
            s->getInt("resolution", static_cast<int>(settings.shadows.resolution)));
        settings.shadows.distance = s->getFloat("distance", settings.shadows.distance);
        settings.shadows.depthBias = s->getFloat("depthBias", settings.shadows.depthBias);
        settings.shadows.slopeScaledBias =
            s->getFloat("slopeScaledBias", settings.shadows.slopeScaledBias);
        if (s->contains("pcfQuality")) {
            settings.shadows.pcfQuality = stringToPcf(s->getString("pcfQuality", "Medium"));
        }
        settings.shadows.softShadows = s->getBool("softShadows", settings.shadows.softShadows);
        settings.shadows.cascadeCount = static_cast<uint32_t>(
            s->getInt("cascadeCount", static_cast<int>(settings.shadows.cascadeCount)));
        settings.shadows.splitLambda = s->getFloat("splitLambda", settings.shadows.splitLambda);
        settings.shadows.stabilizeCascades =
            s->getBool("stabilizeCascades", settings.shadows.stabilizeCascades);
        settings.shadows.contactShadows =
            s->getBool("contactShadows", settings.shadows.contactShadows);
        settings.shadows.contactShadowDistance =
            s->getFloat("contactShadowDistance", settings.shadows.contactShadowDistance);
        settings.shadows.shadowCaching =
            s->getBool("shadowCaching", settings.shadows.shadowCaching);
        settings.shadows.cascadeBlendRange =
            s->getFloat("cascadeBlendRange", settings.shadows.cascadeBlendRange);
    }

    // 4. Post Processing
    if (const auto* pp = root.find("postProcess"); pp != nullptr && pp->isObject()) {
        settings.postProcess.enabled = pp->getBool("enabled", settings.postProcess.enabled);
        if (pp->contains("toneMapOp")) {
            settings.postProcess.toneMapOp = stringToToneMap(pp->getString("toneMapOp", "AgX"));
        }
        settings.postProcess.exposure = pp->getFloat("exposure", settings.postProcess.exposure);
        settings.postProcess.reinhardWhitePoint =
            pp->getFloat("reinhardWhitePoint", settings.postProcess.reinhardWhitePoint);
        settings.postProcess.autoExposureEnabled =
            pp->getBool("autoExposureEnabled", settings.postProcess.autoExposureEnabled);
        settings.postProcess.autoExposureMinEv =
            pp->getFloat("autoExposureMinEv", settings.postProcess.autoExposureMinEv);
        settings.postProcess.autoExposureMaxEv =
            pp->getFloat("autoExposureMaxEv", settings.postProcess.autoExposureMaxEv);
        settings.postProcess.autoExposureSpeedUp =
            pp->getFloat("autoExposureSpeedUp", settings.postProcess.autoExposureSpeedUp);
        settings.postProcess.autoExposureSpeedDown =
            pp->getFloat("autoExposureSpeedDown", settings.postProcess.autoExposureSpeedDown);
        settings.postProcess.exposureCompensation =
            pp->getFloat("exposureCompensation", settings.postProcess.exposureCompensation);
        settings.postProcess.bloomEnabled =
            pp->getBool("bloomEnabled", settings.postProcess.bloomEnabled);
        settings.postProcess.bloomThreshold =
            pp->getFloat("bloomThreshold", settings.postProcess.bloomThreshold);
        settings.postProcess.bloomKnee = pp->getFloat("bloomKnee", settings.postProcess.bloomKnee);
        settings.postProcess.bloomIntensity =
            pp->getFloat("bloomIntensity", settings.postProcess.bloomIntensity);
        settings.postProcess.bloomFilterRadius =
            pp->getFloat("bloomFilterRadius", settings.postProcess.bloomFilterRadius);
        settings.postProcess.dofEnabled =
            pp->getBool("dofEnabled", settings.postProcess.dofEnabled);
        settings.postProcess.dofFocusDistance =
            pp->getFloat("dofFocusDistance", settings.postProcess.dofFocusDistance);
        settings.postProcess.dofFocalLength =
            pp->getFloat("dofFocalLength", settings.postProcess.dofFocalLength);
        settings.postProcess.dofAperture =
            pp->getFloat("dofAperture", settings.postProcess.dofAperture);
        settings.postProcess.dofMaxBlurRadius =
            pp->getFloat("dofMaxBlurRadius", settings.postProcess.dofMaxBlurRadius);
        settings.postProcess.motionBlurEnabled =
            pp->getBool("motionBlurEnabled", settings.postProcess.motionBlurEnabled);
        settings.postProcess.motionBlurIntensity =
            pp->getFloat("motionBlurIntensity", settings.postProcess.motionBlurIntensity);
        settings.postProcess.motionBlurSamples = static_cast<uint32_t>(
            std::max(1, pp->getInt("motionBlurSamples",
                                   static_cast<int>(settings.postProcess.motionBlurSamples))));
        settings.postProcess.whiteBalance =
            pp->getFloat("whiteBalance", settings.postProcess.whiteBalance);
        settings.postProcess.contrast = pp->getFloat("contrast", settings.postProcess.contrast);
        settings.postProcess.saturation =
            pp->getFloat("saturation", settings.postProcess.saturation);
        settings.postProcess.lutEnabled =
            pp->getBool("lutEnabled", settings.postProcess.lutEnabled);
        settings.postProcess.lutIntensity =
            pp->getFloat("lutIntensity", settings.postProcess.lutIntensity);
        if (pp->contains("lutFilePath")) {
            settings.postProcess.lutFilePath =
                std::string(pp->getString("lutFilePath", settings.postProcess.lutFilePath));
        }
        settings.postProcess.vignetteEnabled =
            pp->getBool("vignetteEnabled", settings.postProcess.vignetteEnabled);
        settings.postProcess.vignetteIntensity =
            pp->getFloat("vignetteIntensity", settings.postProcess.vignetteIntensity);
        settings.postProcess.vignetteSmoothness =
            pp->getFloat("vignetteSmoothness", settings.postProcess.vignetteSmoothness);
        settings.postProcess.vignetteRoundness =
            pp->getFloat("vignetteRoundness", settings.postProcess.vignetteRoundness);
        settings.postProcess.filmGrainEnabled =
            pp->getBool("filmGrainEnabled", settings.postProcess.filmGrainEnabled);
        settings.postProcess.filmGrainIntensity =
            pp->getFloat("filmGrainIntensity", settings.postProcess.filmGrainIntensity);
    }

    // 5. Performance
    if (const auto* perf = root.find("performance"); perf != nullptr && perf->isObject()) {
        settings.performance.vsync = perf->getBool("vsync", settings.performance.vsync);
        settings.performance.frameRateLimit = static_cast<uint32_t>(
            perf->getInt("frameRateLimit", static_cast<int>(settings.performance.frameRateLimit)));
        settings.performance.dynamicResolution =
            perf->getBool("dynamicResolution", settings.performance.dynamicResolution);
        settings.performance.gpuProfiling =
            perf->getBool("gpuProfiling", settings.performance.gpuProfiling);
        settings.performance.cpuProfiling =
            perf->getBool("cpuProfiling", settings.performance.cpuProfiling);
        settings.performance.renderStats =
            perf->getBool("renderStats", settings.performance.renderStats);
        settings.performance.frustumCulling =
            perf->getBool("frustumCulling", settings.performance.frustumCulling);
        settings.performance.depthPrepass =
            perf->getBool("depthPrepass", settings.performance.depthPrepass);
    }

    // 6. Debug
    if (const auto* d = root.find("debug"); d != nullptr && d->isObject()) {
        if (d->contains("viewMode")) {
            settings.debug.viewMode = stringToDebugView(d->getString("viewMode", "Lit"));
        }
        settings.debug.visualizeShadowMap =
            d->getBool("visualizeShadowMap", settings.debug.visualizeShadowMap);
        settings.debug.showPassTimings =
            d->getBool("showPassTimings", settings.debug.showPassTimings);
    }

    // Update preset detector to reflect parsed parameters
    settings.quality.preset = settings.detectPreset();
    return Result<GraphicsSettings>::success(settings);
}

Result<GraphicsSettings>
GraphicsSettingsSerializer::deserializeFromFile(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Graphics settings file not found: " + path.string()};
    }

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return Error{.code = ErrorCode::DeviceCreationFailed,
                     .message = "Failed to open graphics settings file: " + path.string()};
    }

    std::stringstream ss;
    ss << ifs.rdbuf();
    return deserializeFromString(ss.str());
}

} // namespace eng::render
