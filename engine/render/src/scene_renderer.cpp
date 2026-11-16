/// @file scene_renderer.cpp
/// @brief Scene mesh rendering traversal and command recording implementation.

#include "render/scene_renderer.h"

#include "core/assert.h"
#include "core/log.h"
#include "core/time.h"
#include "physics/physics_components.h"
#include "render/gpu_material.h"
#include "render/image.h"
#include "render/mesh_shader_spirv.h"
#include "world/components.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace eng::render {

namespace {

struct DofPushConstants {
    Vec2 texelSize;
    float focusDistance;
    float focalLengthMm;
    float aperture;
    float maxBlurRadius;
    float zNear;
    float zFar;
};
static_assert(sizeof(DofPushConstants) == 32, "DofPushConstants must be 32 bytes");

struct MotionBlurPushConstants {
    Vec2 texelSize;
    float intensity;
    int32_t sampleCount;
    float maxVelocityPixels;
    float zNear;
    float zFar;
    float pad0;
};
static_assert(sizeof(MotionBlurPushConstants) == 32, "MotionBlurPushConstants must be 32 bytes");

struct BloomDownsamplePushConstants {
    Vec2 texelSize;
    float threshold;
    float knee;
    int32_t isFirstPass;
    float padding[3];
};

struct BloomUpsamplePushConstants {
    Vec2 texelSize;
    float filterRadius;
    float blendFactor;
};

struct ToneMappingPushConstants {
    float exposure;
    int32_t toneMapOp;
    float bloomIntensity;
    int32_t bloomEnabled;
    float whitePoint;
    float whiteBalance;
    float contrast;
    float saturation;
    float lutIntensity;
    float vignetteIntensity;
    float vignetteSmoothness;
    float vignetteRoundness;
    float filmGrainIntensity;
    uint32_t frameIndex;
    float aspectRatio;
    float pad0;
};
static_assert(sizeof(ToneMappingPushConstants) == 64, "ToneMappingPushConstants must be 64 bytes");

rhi::TextureFormat selectHdrFormat(const rhi::RhiDevice& device) {
    const auto requiredUsage =
        rhi::TextureUsageFlags::ColorAttachment | rhi::TextureUsageFlags::Sampled;
    if (device.isTextureFormatSupported(rhi::TextureFormat::RGBA16_FLOAT, requiredUsage) &&
        device.isTextureFormatFilterable(rhi::TextureFormat::RGBA16_FLOAT)) {
        return rhi::TextureFormat::RGBA16_FLOAT;
    }
    if (device.isTextureFormatSupported(rhi::TextureFormat::RGBA32_FLOAT, requiredUsage) &&
        device.isTextureFormatFilterable(rhi::TextureFormat::RGBA32_FLOAT)) {
        return rhi::TextureFormat::RGBA32_FLOAT;
    }
    return rhi::TextureFormat::RGBA8_UNORM;
}

} // namespace

SceneRenderer::~SceneRenderer() {
    shutdown();
}

SceneRenderer::SceneRenderer(SceneRenderer&& other) noexcept
    : device_(other.device_), colorTarget_(other.colorTarget_), depthTarget_(other.depthTarget_),
      vertexShader_(other.vertexShader_), fragmentShader_(other.fragmentShader_),
      fragmentShaderOpaque_(other.fragmentShaderOpaque_),
      materialDescriptorSetLayout_(other.materialDescriptorSetLayout_),
      sceneDescriptorSetLayout_(other.sceneDescriptorSetLayout_),
      sceneDescriptorPool_(other.sceneDescriptorPool_),
      sceneDescriptorSet_(other.sceneDescriptorSet_),
      lightingUniformBuffer_(other.lightingUniformBuffer_),
      shadowDepthTarget_(other.shadowDepthTarget_), staticShadowTarget_(other.staticShadowTarget_),
      shadowSampler_(other.shadowSampler_), shadowVertexShader_(other.shadowVertexShader_),
      shadowPipelineLayout_(other.shadowPipelineLayout_), shadowPipeline_(other.shadowPipeline_),
      depthPrepassPipeline_(other.depthPrepassPipeline_), pipelineLayout_(other.pipelineLayout_),
      pipeline_(other.pipeline_), pipelineOpaque_(other.pipelineOpaque_),
      pipelineDepthPrepass_(other.pipelineDepthPrepass_),
      pipelineDepthPrepassOpaque_(other.pipelineDepthPrepassOpaque_),
      hdrPipelineDepthPrepass_(other.hdrPipelineDepthPrepass_),
      hdrPipelineDepthPrepassOpaque_(other.hdrPipelineDepthPrepassOpaque_),
      hdrTarget_(other.hdrTarget_), hdrFormat_(other.hdrFormat_),
      postProcessSettings_(other.postProcessSettings_),
      postProcessSampler_(other.postProcessSampler_),
      fullscreenVertexShader_(other.fullscreenVertexShader_),
      bloomDownFragmentShader_(other.bloomDownFragmentShader_),
      bloomUpFragmentShader_(other.bloomUpFragmentShader_),
      toneMappingFragmentShader_(other.toneMappingFragmentShader_),
      singleTextureSetLayout_(other.singleTextureSetLayout_),
      dualTextureSetLayout_(other.dualTextureSetLayout_),
      postProcessDescriptorPool_(other.postProcessDescriptorPool_),
      toneMappingSet_(other.toneMappingSet_),
      bloomDownPipelineLayout_(other.bloomDownPipelineLayout_),
      bloomUpPipelineLayout_(other.bloomUpPipelineLayout_),
      toneMappingPipelineLayout_(other.toneMappingPipelineLayout_),
      bloomDownPipeline_(other.bloomDownPipeline_), bloomUpPipeline_(other.bloomUpPipeline_),
      toneMappingPipeline_(other.toneMappingPipeline_), hdrPipeline_(other.hdrPipeline_),
      hdrPipelineOpaque_(other.hdrPipelineOpaque_), width_(other.width_), height_(other.height_),
      shadowResolution_(other.shadowResolution_), colorFormat_(other.colorFormat_),
      depthFormat_(other.depthFormat_), settings_(other.settings_),
      wireframePipeline_(other.wireframePipeline_),
      wireframeLdrPipeline_(other.wireframeLdrPipeline_),
      renderGraph_(std::move(other.renderGraph_)),
      postProcessGraph_(std::move(other.postProcessGraph_)) {
    for (size_t i = 0; i < kBloomPyramidLevels; ++i) {
        bloomDownTarget_[i] = other.bloomDownTarget_[i];
        bloomDownSets_[i] = other.bloomDownSets_[i];
        other.bloomDownTarget_[i] = {};
        other.bloomDownSets_[i] = {};
    }
    for (size_t i = 0; i < kBloomPyramidLevels - 1; ++i) {
        bloomUpTarget_[i] = other.bloomUpTarget_[i];
        bloomUpSets_[i] = other.bloomUpSets_[i];
        other.bloomUpTarget_[i] = {};
        other.bloomUpSets_[i] = {};
    }
    postProcessTempTarget_[0] = other.postProcessTempTarget_[0];
    postProcessTempTarget_[1] = other.postProcessTempTarget_[1];
    dofFragmentShader_ = other.dofFragmentShader_;
    motionBlurFragmentShader_ = other.motionBlurFragmentShader_;
    dofSet_ = other.dofSet_;
    motionBlurSet_ = other.motionBlurSet_;
    dofPipelineLayout_ = other.dofPipelineLayout_;
    motionBlurPipelineLayout_ = other.motionBlurPipelineLayout_;
    dofPipeline_ = other.dofPipeline_;
    motionBlurPipeline_ = other.motionBlurPipeline_;
    defaultLutTexture_ = std::move(other.defaultLutTexture_);
    customLutTexture_ = std::move(other.customLutTexture_);
    loadedLutPath_ = std::move(other.loadedLutPath_);
    adaptedLuminance_ = other.adaptedLuminance_;
    adaptedExposure_ = other.adaptedExposure_;
    other.postProcessTempTarget_[0] = {};
    other.postProcessTempTarget_[1] = {};
    other.dofFragmentShader_ = {};
    other.motionBlurFragmentShader_ = {};
    other.dofSet_ = {};
    other.motionBlurSet_ = {};
    other.dofPipelineLayout_ = {};
    other.motionBlurPipelineLayout_ = {};
    other.dofPipeline_ = {};
    other.motionBlurPipeline_ = {};

    gtaoRawTarget_ = other.gtaoRawTarget_;
    gtaoFilteredTarget_ = other.gtaoFilteredTarget_;
    ssgiRawTarget_ = other.ssgiRawTarget_;
    ssgiFilteredTarget_ = other.ssgiFilteredTarget_;
    ssgiHistoryTarget_[0] = other.ssgiHistoryTarget_[0];
    ssgiHistoryTarget_[1] = other.ssgiHistoryTarget_[1];
    ssrRawTarget_ = other.ssrRawTarget_;
    ssrFilteredTarget_ = other.ssrFilteredTarget_;
    ssrHistoryTarget_[0] = other.ssrHistoryTarget_[0];
    ssrHistoryTarget_[1] = other.ssrHistoryTarget_[1];
    hdrHistoryTarget_[0] = other.hdrHistoryTarget_[0];
    hdrHistoryTarget_[1] = other.hdrHistoryTarget_[1];
    defaultAoTexture_ = std::move(other.defaultAoTexture_);
    defaultIndirectTexture_ = std::move(other.defaultIndirectTexture_);
    defaultSsrTexture_ = std::move(other.defaultSsrTexture_);
    linearClampSampler_ = other.linearClampSampler_;
    pointClampSampler_ = other.pointClampSampler_;
    gtaoFragmentShader_ = other.gtaoFragmentShader_;
    bilateralBlurFragmentShader_ = other.bilateralBlurFragmentShader_;
    ssgiFragmentShader_ = other.ssgiFragmentShader_;
    ssrFragmentShader_ = other.ssrFragmentShader_;
    temporalFilterFragmentShader_ = other.temporalFilterFragmentShader_;
    tripleTextureSetLayout_ = other.tripleTextureSetLayout_;
    giDescriptorPool_ = other.giDescriptorPool_;
    gtaoDescriptorSet_ = other.gtaoDescriptorSet_;
    gtaoBlurDescriptorSet_ = other.gtaoBlurDescriptorSet_;
    ssgiDescriptorSet_ = other.ssgiDescriptorSet_;
    ssrDescriptorSet_ = other.ssrDescriptorSet_;
    temporalFilterDescriptorSet_ = other.temporalFilterDescriptorSet_;
    ssrTemporalFilterDescriptorSet_ = other.ssrTemporalFilterDescriptorSet_;
    gtaoPipelineLayout_ = other.gtaoPipelineLayout_;
    gtaoBlurPipelineLayout_ = other.gtaoBlurPipelineLayout_;
    ssgiPipelineLayout_ = other.ssgiPipelineLayout_;
    ssrPipelineLayout_ = other.ssrPipelineLayout_;
    temporalFilterPipelineLayout_ = other.temporalFilterPipelineLayout_;
    gtaoPipeline_ = other.gtaoPipeline_;
    gtaoBlurPipeline_ = other.gtaoBlurPipeline_;
    ssgiPipeline_ = other.ssgiPipeline_;
    ssrPipeline_ = other.ssrPipeline_;
    temporalFilterPipeline_ = other.temporalFilterPipeline_;
    prevViewProj_ = other.prevViewProj_;
    isHistoryValid_ = other.isHistoryValid_;
    giFrameIndex_ = other.giFrameIndex_;
    staticShadowDirty_ = other.staticShadowDirty_;

    // Milestone 11H: Motion Vectors, TAA, and Sharpening
    velocityTarget_ = other.velocityTarget_;
    hdrRawTarget_ = other.hdrRawTarget_;
    velocityFragmentShader_ = other.velocityFragmentShader_;
    meshVelocityVertexShader_ = other.meshVelocityVertexShader_;
    meshVelocityFragmentShader_ = other.meshVelocityFragmentShader_;
    taaFragmentShader_ = other.taaFragmentShader_;
    sharpenFragmentShader_ = other.sharpenFragmentShader_;
    velocitySetLayout_ = other.velocitySetLayout_;
    taaSetLayout_ = other.taaSetLayout_;
    sharpenSetLayout_ = other.sharpenSetLayout_;
    taaDescriptorPool_ = other.taaDescriptorPool_;
    velocityDescriptorSet_ = other.velocityDescriptorSet_;
    taaDescriptorSet_[0] = other.taaDescriptorSet_[0];
    taaDescriptorSet_[1] = other.taaDescriptorSet_[1];
    sharpenDescriptorSet_[0] = other.sharpenDescriptorSet_[0];
    sharpenDescriptorSet_[1] = other.sharpenDescriptorSet_[1];
    velocityPipelineLayout_ = other.velocityPipelineLayout_;
    meshVelocityPipelineLayout_ = other.meshVelocityPipelineLayout_;
    taaPipelineLayout_ = other.taaPipelineLayout_;
    sharpenPipelineLayout_ = other.sharpenPipelineLayout_;
    velocityPipeline_ = other.velocityPipeline_;
    meshVelocityPipeline_ = other.meshVelocityPipeline_;
    taaPipeline_ = other.taaPipeline_;
    sharpenPipeline_ = other.sharpenPipeline_;
    unjitteredViewProj_ = other.unjitteredViewProj_;
    unjitteredPrevViewProj_ = other.unjitteredPrevViewProj_;
    taaJitterPhase_ = other.taaJitterPhase_;
    isTaaHistoryValid_ = other.isTaaHistoryValid_;
    prevEntityTransforms_ = std::move(other.prevEntityTransforms_);

    other.device_ = nullptr;
    other.colorTarget_ = {};
    other.depthTarget_ = {};
    other.vertexShader_ = {};
    other.fragmentShader_ = {};
    other.fragmentShaderOpaque_ = {};
    other.materialDescriptorSetLayout_ = {};
    other.sceneDescriptorSetLayout_ = {};
    other.sceneDescriptorPool_ = {};
    other.sceneDescriptorSet_ = {};
    other.lightingUniformBuffer_ = {};
    other.shadowDepthTarget_ = {};
    other.staticShadowTarget_ = {};
    other.shadowSampler_ = {};
    other.shadowVertexShader_ = {};
    other.shadowPipelineLayout_ = {};
    other.shadowPipeline_ = {};
    other.depthPrepassPipeline_ = {};
    other.pipelineLayout_ = {};
    other.pipeline_ = {};
    other.pipelineOpaque_ = {};
    other.pipelineDepthPrepass_ = {};
    other.pipelineDepthPrepassOpaque_ = {};
    other.hdrPipelineDepthPrepass_ = {};
    other.hdrPipelineDepthPrepassOpaque_ = {};
    other.hdrTarget_ = {};
    other.postProcessSampler_ = {};
    other.fullscreenVertexShader_ = {};
    other.bloomDownFragmentShader_ = {};
    other.bloomUpFragmentShader_ = {};
    other.toneMappingFragmentShader_ = {};
    other.singleTextureSetLayout_ = {};
    other.dualTextureSetLayout_ = {};
    other.postProcessDescriptorPool_ = {};
    other.toneMappingSet_ = {};
    other.bloomDownPipelineLayout_ = {};
    other.bloomUpPipelineLayout_ = {};
    other.toneMappingPipelineLayout_ = {};
    other.bloomDownPipeline_ = {};
    other.bloomUpPipeline_ = {};
    other.toneMappingPipeline_ = {};
    other.hdrPipeline_ = {};
    other.hdrPipelineOpaque_ = {};
    other.pipelineDoubleSided_ = {};
    other.hdrPipelineDoubleSided_ = {};
    other.pipelineTransparent_ = {};
    other.pipelineTransparentDoubleSided_ = {};
    other.hdrPipelineTransparent_ = {};
    other.hdrPipelineTransparentDoubleSided_ = {};
    other.wireframePipeline_ = {};
    other.wireframeLdrPipeline_ = {};
    other.gtaoRawTarget_ = {};
    other.gtaoFilteredTarget_ = {};
    other.ssgiRawTarget_ = {};
    other.ssgiFilteredTarget_ = {};
    other.ssgiHistoryTarget_[0] = {};
    other.ssgiHistoryTarget_[1] = {};
    other.ssrRawTarget_ = {};
    other.ssrFilteredTarget_ = {};
    other.ssrHistoryTarget_[0] = {};
    other.ssrHistoryTarget_[1] = {};
    other.hdrHistoryTarget_[0] = {};
    other.hdrHistoryTarget_[1] = {};
    other.linearClampSampler_ = {};
    other.pointClampSampler_ = {};
    other.gtaoFragmentShader_ = {};
    other.bilateralBlurFragmentShader_ = {};
    other.ssgiFragmentShader_ = {};
    other.ssrFragmentShader_ = {};
    other.temporalFilterFragmentShader_ = {};
    other.tripleTextureSetLayout_ = {};
    other.giDescriptorPool_ = {};
    other.gtaoDescriptorSet_ = {};
    other.gtaoBlurDescriptorSet_ = {};
    other.ssgiDescriptorSet_ = {};
    other.ssrDescriptorSet_ = {};
    other.temporalFilterDescriptorSet_ = {};
    other.ssrTemporalFilterDescriptorSet_ = {};
    other.gtaoPipelineLayout_ = {};
    other.gtaoBlurPipelineLayout_ = {};
    other.ssgiPipelineLayout_ = {};
    other.ssrPipelineLayout_ = {};
    other.temporalFilterPipelineLayout_ = {};
    other.gtaoPipeline_ = {};
    other.gtaoBlurPipeline_ = {};
    other.ssgiPipeline_ = {};
    other.ssrPipeline_ = {};
    other.temporalFilterPipeline_ = {};
    other.isHistoryValid_ = false;
    other.giFrameIndex_ = 0;

    // Milestone 11H: Motion Vectors, TAA, and Sharpening
    other.velocityTarget_ = {};
    other.hdrRawTarget_ = {};
    other.velocityFragmentShader_ = {};
    other.meshVelocityVertexShader_ = {};
    other.meshVelocityFragmentShader_ = {};
    other.taaFragmentShader_ = {};
    other.sharpenFragmentShader_ = {};
    other.velocitySetLayout_ = {};
    other.taaSetLayout_ = {};
    other.sharpenSetLayout_ = {};
    other.taaDescriptorPool_ = {};
    other.velocityDescriptorSet_ = {};
    other.taaDescriptorSet_[0] = {};
    other.taaDescriptorSet_[1] = {};
    other.sharpenDescriptorSet_[0] = {};
    other.sharpenDescriptorSet_[1] = {};
    other.velocityPipelineLayout_ = {};
    other.meshVelocityPipelineLayout_ = {};
    other.taaPipelineLayout_ = {};
    other.sharpenPipelineLayout_ = {};
    other.velocityPipeline_ = {};
    other.meshVelocityPipeline_ = {};
    other.taaPipeline_ = {};
    other.sharpenPipeline_ = {};
    other.isTaaHistoryValid_ = false;
    other.taaJitterPhase_ = 0;
    other.prevEntityTransforms_.clear();

    other.width_ = 0;
    other.height_ = 0;
    other.shadowResolution_ = 2048;
}

SceneRenderer& SceneRenderer::operator=(SceneRenderer&& other) noexcept {
    if (this != &other) {
        shutdown();
        device_ = other.device_;
        colorTarget_ = other.colorTarget_;
        depthTarget_ = other.depthTarget_;
        vertexShader_ = other.vertexShader_;
        fragmentShader_ = other.fragmentShader_;
        fragmentShaderOpaque_ = other.fragmentShaderOpaque_;
        materialDescriptorSetLayout_ = other.materialDescriptorSetLayout_;
        sceneDescriptorSetLayout_ = other.sceneDescriptorSetLayout_;
        sceneDescriptorPool_ = other.sceneDescriptorPool_;
        sceneDescriptorSet_ = other.sceneDescriptorSet_;
        lightingUniformBuffer_ = other.lightingUniformBuffer_;
        shadowDepthTarget_ = other.shadowDepthTarget_;
        staticShadowTarget_ = other.staticShadowTarget_;
        staticShadowDirty_ = other.staticShadowDirty_;
        shadowSampler_ = other.shadowSampler_;
        shadowVertexShader_ = other.shadowVertexShader_;
        shadowPipelineLayout_ = other.shadowPipelineLayout_;
        shadowPipeline_ = other.shadowPipeline_;
        depthPrepassPipeline_ = other.depthPrepassPipeline_;
        pipelineLayout_ = other.pipelineLayout_;
        pipeline_ = other.pipeline_;
        pipelineOpaque_ = other.pipelineOpaque_;
        pipelineDepthPrepass_ = other.pipelineDepthPrepass_;
        pipelineDepthPrepassOpaque_ = other.pipelineDepthPrepassOpaque_;
        hdrPipelineDepthPrepass_ = other.hdrPipelineDepthPrepass_;
        hdrPipelineDepthPrepassOpaque_ = other.hdrPipelineDepthPrepassOpaque_;

        hdrTarget_ = other.hdrTarget_;
        hdrFormat_ = other.hdrFormat_;
        postProcessSettings_ = other.postProcessSettings_;
        for (size_t i = 0; i < kBloomPyramidLevels; ++i) {
            bloomDownTarget_[i] = other.bloomDownTarget_[i];
            bloomDownSets_[i] = other.bloomDownSets_[i];
            other.bloomDownTarget_[i] = {};
            other.bloomDownSets_[i] = {};
        }
        for (size_t i = 0; i < kBloomPyramidLevels - 1; ++i) {
            bloomUpTarget_[i] = other.bloomUpTarget_[i];
            bloomUpSets_[i] = other.bloomUpSets_[i];
            other.bloomUpTarget_[i] = {};
            other.bloomUpSets_[i] = {};
        }
        postProcessTempTarget_[0] = other.postProcessTempTarget_[0];
        postProcessTempTarget_[1] = other.postProcessTempTarget_[1];
        dofFragmentShader_ = other.dofFragmentShader_;
        motionBlurFragmentShader_ = other.motionBlurFragmentShader_;
        dofSet_ = other.dofSet_;
        motionBlurSet_ = other.motionBlurSet_;
        dofPipelineLayout_ = other.dofPipelineLayout_;
        motionBlurPipelineLayout_ = other.motionBlurPipelineLayout_;
        dofPipeline_ = other.dofPipeline_;
        motionBlurPipeline_ = other.motionBlurPipeline_;
        defaultLutTexture_ = std::move(other.defaultLutTexture_);
        customLutTexture_ = std::move(other.customLutTexture_);
        loadedLutPath_ = std::move(other.loadedLutPath_);
        adaptedLuminance_ = other.adaptedLuminance_;
        adaptedExposure_ = other.adaptedExposure_;
        other.postProcessTempTarget_[0] = {};
        other.postProcessTempTarget_[1] = {};
        other.dofFragmentShader_ = {};
        other.motionBlurFragmentShader_ = {};
        other.dofSet_ = {};
        other.motionBlurSet_ = {};
        other.dofPipelineLayout_ = {};
        other.motionBlurPipelineLayout_ = {};
        other.dofPipeline_ = {};
        other.motionBlurPipeline_ = {};

        postProcessSampler_ = other.postProcessSampler_;
        fullscreenVertexShader_ = other.fullscreenVertexShader_;
        bloomDownFragmentShader_ = other.bloomDownFragmentShader_;
        bloomUpFragmentShader_ = other.bloomUpFragmentShader_;
        toneMappingFragmentShader_ = other.toneMappingFragmentShader_;
        singleTextureSetLayout_ = other.singleTextureSetLayout_;
        dualTextureSetLayout_ = other.dualTextureSetLayout_;
        postProcessDescriptorPool_ = other.postProcessDescriptorPool_;
        toneMappingSet_ = other.toneMappingSet_;
        bloomDownPipelineLayout_ = other.bloomDownPipelineLayout_;
        bloomUpPipelineLayout_ = other.bloomUpPipelineLayout_;
        toneMappingPipelineLayout_ = other.toneMappingPipelineLayout_;
        bloomDownPipeline_ = other.bloomDownPipeline_;
        bloomUpPipeline_ = other.bloomUpPipeline_;
        toneMappingPipeline_ = other.toneMappingPipeline_;
        hdrPipeline_ = other.hdrPipeline_;
        hdrPipelineOpaque_ = other.hdrPipelineOpaque_;
        pipelineDoubleSided_ = other.pipelineDoubleSided_;
        hdrPipelineDoubleSided_ = other.hdrPipelineDoubleSided_;
        pipelineTransparent_ = other.pipelineTransparent_;
        pipelineTransparentDoubleSided_ = other.pipelineTransparentDoubleSided_;
        hdrPipelineTransparent_ = other.hdrPipelineTransparent_;
        hdrPipelineTransparentDoubleSided_ = other.hdrPipelineTransparentDoubleSided_;
        settings_ = other.settings_;
        wireframePipeline_ = other.wireframePipeline_;
        wireframeLdrPipeline_ = other.wireframeLdrPipeline_;
        renderGraph_ = std::move(other.renderGraph_);
        postProcessGraph_ = std::move(other.postProcessGraph_);

        gtaoRawTarget_ = other.gtaoRawTarget_;
        gtaoFilteredTarget_ = other.gtaoFilteredTarget_;
        ssgiRawTarget_ = other.ssgiRawTarget_;
        ssgiFilteredTarget_ = other.ssgiFilteredTarget_;
        ssgiHistoryTarget_[0] = other.ssgiHistoryTarget_[0];
        ssgiHistoryTarget_[1] = other.ssgiHistoryTarget_[1];
        ssrRawTarget_ = other.ssrRawTarget_;
        ssrFilteredTarget_ = other.ssrFilteredTarget_;
        ssrHistoryTarget_[0] = other.ssrHistoryTarget_[0];
        ssrHistoryTarget_[1] = other.ssrHistoryTarget_[1];
        hdrHistoryTarget_[0] = other.hdrHistoryTarget_[0];
        hdrHistoryTarget_[1] = other.hdrHistoryTarget_[1];
        defaultAoTexture_ = std::move(other.defaultAoTexture_);
        defaultIndirectTexture_ = std::move(other.defaultIndirectTexture_);
        defaultSsrTexture_ = std::move(other.defaultSsrTexture_);
        linearClampSampler_ = other.linearClampSampler_;
        pointClampSampler_ = other.pointClampSampler_;
        gtaoFragmentShader_ = other.gtaoFragmentShader_;
        bilateralBlurFragmentShader_ = other.bilateralBlurFragmentShader_;
        ssgiFragmentShader_ = other.ssgiFragmentShader_;
        ssrFragmentShader_ = other.ssrFragmentShader_;
        temporalFilterFragmentShader_ = other.temporalFilterFragmentShader_;
        tripleTextureSetLayout_ = other.tripleTextureSetLayout_;
        giDescriptorPool_ = other.giDescriptorPool_;
        gtaoDescriptorSet_ = other.gtaoDescriptorSet_;
        gtaoBlurDescriptorSet_ = other.gtaoBlurDescriptorSet_;
        ssgiDescriptorSet_ = other.ssgiDescriptorSet_;
        ssrDescriptorSet_ = other.ssrDescriptorSet_;
        temporalFilterDescriptorSet_ = other.temporalFilterDescriptorSet_;
        ssrTemporalFilterDescriptorSet_ = other.ssrTemporalFilterDescriptorSet_;
        gtaoPipelineLayout_ = other.gtaoPipelineLayout_;
        gtaoBlurPipelineLayout_ = other.gtaoBlurPipelineLayout_;
        ssgiPipelineLayout_ = other.ssgiPipelineLayout_;
        ssrPipelineLayout_ = other.ssrPipelineLayout_;
        temporalFilterPipelineLayout_ = other.temporalFilterPipelineLayout_;
        gtaoPipeline_ = other.gtaoPipeline_;
        gtaoBlurPipeline_ = other.gtaoBlurPipeline_;
        ssgiPipeline_ = other.ssgiPipeline_;
        ssrPipeline_ = other.ssrPipeline_;
        temporalFilterPipeline_ = other.temporalFilterPipeline_;
        prevViewProj_ = other.prevViewProj_;
        isHistoryValid_ = other.isHistoryValid_;
        giFrameIndex_ = other.giFrameIndex_;

        // Milestone 11H: Motion Vectors, TAA, and Sharpening
        velocityTarget_ = other.velocityTarget_;
        hdrRawTarget_ = other.hdrRawTarget_;
        velocityFragmentShader_ = other.velocityFragmentShader_;
        meshVelocityVertexShader_ = other.meshVelocityVertexShader_;
        meshVelocityFragmentShader_ = other.meshVelocityFragmentShader_;
        taaFragmentShader_ = other.taaFragmentShader_;
        sharpenFragmentShader_ = other.sharpenFragmentShader_;
        velocitySetLayout_ = other.velocitySetLayout_;
        taaSetLayout_ = other.taaSetLayout_;
        sharpenSetLayout_ = other.sharpenSetLayout_;
        taaDescriptorPool_ = other.taaDescriptorPool_;
        velocityDescriptorSet_ = other.velocityDescriptorSet_;
        taaDescriptorSet_[0] = other.taaDescriptorSet_[0];
        taaDescriptorSet_[1] = other.taaDescriptorSet_[1];
        sharpenDescriptorSet_[0] = other.sharpenDescriptorSet_[0];
        sharpenDescriptorSet_[1] = other.sharpenDescriptorSet_[1];
        velocityPipelineLayout_ = other.velocityPipelineLayout_;
        meshVelocityPipelineLayout_ = other.meshVelocityPipelineLayout_;
        taaPipelineLayout_ = other.taaPipelineLayout_;
        sharpenPipelineLayout_ = other.sharpenPipelineLayout_;
        velocityPipeline_ = other.velocityPipeline_;
        meshVelocityPipeline_ = other.meshVelocityPipeline_;
        taaPipeline_ = other.taaPipeline_;
        sharpenPipeline_ = other.sharpenPipeline_;
        unjitteredViewProj_ = other.unjitteredViewProj_;
        unjitteredPrevViewProj_ = other.unjitteredPrevViewProj_;
        taaJitterPhase_ = other.taaJitterPhase_;
        isTaaHistoryValid_ = other.isTaaHistoryValid_;
        prevEntityTransforms_ = std::move(other.prevEntityTransforms_);

        width_ = other.width_;
        height_ = other.height_;
        shadowResolution_ = other.shadowResolution_;
        colorFormat_ = other.colorFormat_;
        depthFormat_ = other.depthFormat_;

        other.device_ = nullptr;
        other.colorTarget_ = {};
        other.depthTarget_ = {};
        other.vertexShader_ = {};
        other.fragmentShader_ = {};
        other.fragmentShaderOpaque_ = {};
        other.materialDescriptorSetLayout_ = {};
        other.sceneDescriptorSetLayout_ = {};
        other.sceneDescriptorPool_ = {};
        other.sceneDescriptorSet_ = {};
        other.lightingUniformBuffer_ = {};
        other.shadowDepthTarget_ = {};
        other.staticShadowTarget_ = {};
        other.shadowSampler_ = {};
        other.shadowVertexShader_ = {};
        other.shadowPipelineLayout_ = {};
        other.shadowPipeline_ = {};
        other.depthPrepassPipeline_ = {};
        other.pipelineLayout_ = {};
        other.pipeline_ = {};
        other.pipelineOpaque_ = {};
        other.pipelineDepthPrepass_ = {};
        other.pipelineDepthPrepassOpaque_ = {};
        other.hdrPipelineDepthPrepass_ = {};
        other.hdrPipelineDepthPrepassOpaque_ = {};
        other.hdrTarget_ = {};
        other.postProcessSampler_ = {};
        other.fullscreenVertexShader_ = {};
        other.bloomDownFragmentShader_ = {};
        other.bloomUpFragmentShader_ = {};
        other.toneMappingFragmentShader_ = {};
        other.singleTextureSetLayout_ = {};
        other.dualTextureSetLayout_ = {};
        other.postProcessDescriptorPool_ = {};
        other.toneMappingSet_ = {};
        other.bloomDownPipelineLayout_ = {};
        other.bloomUpPipelineLayout_ = {};
        other.toneMappingPipelineLayout_ = {};
        other.bloomDownPipeline_ = {};
        other.bloomUpPipeline_ = {};
        other.toneMappingPipeline_ = {};
        other.hdrPipeline_ = {};
        other.hdrPipelineOpaque_ = {};
        other.pipelineDoubleSided_ = {};
        other.hdrPipelineDoubleSided_ = {};
        other.pipelineTransparent_ = {};
        other.pipelineTransparentDoubleSided_ = {};
        other.hdrPipelineTransparent_ = {};
        other.hdrPipelineTransparentDoubleSided_ = {};
        other.wireframePipeline_ = {};
        other.wireframeLdrPipeline_ = {};
        other.gtaoRawTarget_ = {};
        other.gtaoFilteredTarget_ = {};
        other.ssgiRawTarget_ = {};
        other.ssgiFilteredTarget_ = {};
        other.ssgiHistoryTarget_[0] = {};
        other.ssgiHistoryTarget_[1] = {};
        other.ssrRawTarget_ = {};
        other.ssrFilteredTarget_ = {};
        other.ssrHistoryTarget_[0] = {};
        other.ssrHistoryTarget_[1] = {};
        other.hdrHistoryTarget_[0] = {};
        other.hdrHistoryTarget_[1] = {};
        other.linearClampSampler_ = {};
        other.pointClampSampler_ = {};
        other.gtaoFragmentShader_ = {};
        other.bilateralBlurFragmentShader_ = {};
        other.ssgiFragmentShader_ = {};
        other.ssrFragmentShader_ = {};
        other.temporalFilterFragmentShader_ = {};
        other.tripleTextureSetLayout_ = {};
        other.giDescriptorPool_ = {};
        other.gtaoDescriptorSet_ = {};
        other.gtaoBlurDescriptorSet_ = {};
        other.ssgiDescriptorSet_ = {};
        other.ssrDescriptorSet_ = {};
        other.temporalFilterDescriptorSet_ = {};
        other.ssrTemporalFilterDescriptorSet_ = {};
        other.gtaoPipelineLayout_ = {};
        other.gtaoBlurPipelineLayout_ = {};
        other.ssgiPipelineLayout_ = {};
        other.ssrPipelineLayout_ = {};
        other.temporalFilterPipelineLayout_ = {};
        other.gtaoPipeline_ = {};
        other.gtaoBlurPipeline_ = {};
        other.ssgiPipeline_ = {};
        other.ssrPipeline_ = {};
        other.temporalFilterPipeline_ = {};
        other.isHistoryValid_ = false;
        other.giFrameIndex_ = 0;

        // Milestone 11H: Motion Vectors, TAA, and Sharpening
        other.velocityTarget_ = {};
        other.hdrRawTarget_ = {};
        other.velocityFragmentShader_ = {};
        other.meshVelocityVertexShader_ = {};
        other.meshVelocityFragmentShader_ = {};
        other.taaFragmentShader_ = {};
        other.sharpenFragmentShader_ = {};
        other.velocitySetLayout_ = {};
        other.taaSetLayout_ = {};
        other.sharpenSetLayout_ = {};
        other.taaDescriptorPool_ = {};
        other.velocityDescriptorSet_ = {};
        other.taaDescriptorSet_[0] = {};
        other.taaDescriptorSet_[1] = {};
        other.sharpenDescriptorSet_[0] = {};
        other.sharpenDescriptorSet_[1] = {};
        other.velocityPipelineLayout_ = {};
        other.meshVelocityPipelineLayout_ = {};
        other.taaPipelineLayout_ = {};
        other.sharpenPipelineLayout_ = {};
        other.velocityPipeline_ = {};
        other.meshVelocityPipeline_ = {};
        other.taaPipeline_ = {};
        other.sharpenPipeline_ = {};
        other.isTaaHistoryValid_ = false;
        other.taaJitterPhase_ = 0;
        other.prevEntityTransforms_.clear();

        other.width_ = 0;
        other.height_ = 0;
        other.shadowResolution_ = 2048;
    }
    return *this;
}

Result<void> SceneRenderer::init(rhi::RhiDevice& device, uint32_t width, uint32_t height,
                                 rhi::TextureFormat colorFormat, rhi::TextureFormat depthFormat) {
    if (width == 0 || height == 0) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "SceneRenderer width and height must be non-zero"};
    }

    shutdown();

    device_ = &device;
    width_ = width;
    height_ = height;
    colorFormat_ = colorFormat;
    depthFormat_ = depthFormat;
    hdrFormat_ = selectHdrFormat(device);

    // 1. Allocate offscreen color attachment texture
    rhi::TextureDesc colorDesc{
        .type = rhi::TextureType::Texture2D,
        .format = colorFormat_,
        .width = width_,
        .height = height_,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = rhi::TextureUsageFlags::ColorAttachment | rhi::TextureUsageFlags::Sampled |
                 rhi::TextureUsageFlags::TransferSrc,
        .debugName = "SceneRendererColorTarget",
    };
    auto colorRes = device_->createTexture(colorDesc);
    if (!colorRes.hasValue()) {
        shutdown();
        return colorRes.error();
    }
    colorTarget_ = colorRes.value();

    // 2. Allocate offscreen depth attachment texture
    rhi::TextureDesc depthDesc{
        .type = rhi::TextureType::Texture2D,
        .format = depthFormat_,
        .width = width_,
        .height = height_,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = rhi::TextureUsageFlags::DepthStencilAttachment | rhi::TextureUsageFlags::Sampled |
                 rhi::TextureUsageFlags::TransferSrc,
        .debugName = "SceneRendererDepthTarget",
    };
    auto depthRes = device_->createTexture(depthDesc);
    if (!depthRes.hasValue()) {
        shutdown();
        return depthRes.error();
    }
    depthTarget_ = depthRes.value();

    // 2b. Allocate directional shadow map depth texture
    rhi::TextureDesc shadowDesc{
        .type = rhi::TextureType::Texture2D,
        .format = rhi::TextureFormat::D32_FLOAT,
        .width = shadowResolution_,
        .height = shadowResolution_,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = rhi::TextureUsageFlags::DepthStencilAttachment | rhi::TextureUsageFlags::Sampled,
        .debugName = "DirectionalShadowMap",
    };
    auto shadowRes = device_->createTexture(shadowDesc);
    if (!shadowRes.hasValue()) {
        shutdown();
        return shadowRes.error();
    }
    shadowDepthTarget_ = shadowRes.value();

    shadowDesc.debugName = "StaticShadowMapCache";
    auto staticShadowRes = device_->createTexture(shadowDesc);
    if (!staticShadowRes.hasValue()) {
        shutdown();
        return staticShadowRes.error();
    }
    staticShadowTarget_ = staticShadowRes.value();
    staticShadowDirty_ = true;

    // 2c. Allocate shadow comparison sampler
    rhi::SamplerDesc shadowSampDesc{
        .minFilter = rhi::Filter::Linear,
        .magFilter = rhi::Filter::Linear,
        .mipmapMode = rhi::Filter::Linear,
        .addressU = rhi::SamplerAddressMode::ClampToEdge,
        .addressV = rhi::SamplerAddressMode::ClampToEdge,
        .addressW = rhi::SamplerAddressMode::ClampToEdge,
        .compareEnable = true,
        .compareOp = rhi::CompareOp::LessOrEqual,
        .debugName = "ShadowComparisonSampler",
    };
    auto sampRes = device_->createSampler(shadowSampDesc);
    if (!sampRes.hasValue()) {
        shutdown();
        return sampRes.error();
    }
    shadowSampler_ = sampRes.value();

    // 2d. Allocate lighting uniform buffer
    rhi::BufferDesc uboDesc{
        .sizeInBytes = sizeof(GpuLightingData),
        .usage = rhi::BufferUsageFlags::Uniform,
        .memoryUsage = rhi::MemoryUsage::CpuToGpu,
        .debugName = "SceneLightingUniformBuffer",
    };
    auto uboRes = device_->createBuffer(uboDesc);
    if (!uboRes.hasValue()) {
        shutdown();
        return uboRes.error();
    }
    lightingUniformBuffer_ = uboRes.value();

    GpuLightingData initialLighting{};
    std::ignore =
        device_->writeBuffer(lightingUniformBuffer_, 0, &initialLighting, sizeof(GpuLightingData));

    // 3. Create material descriptor set layout (Set 0)
    auto matLayoutDesc = createMaterialDescriptorSetLayoutDesc();
    auto matLayoutRes = device_->createDescriptorSetLayout(matLayoutDesc);
    if (!matLayoutRes.hasValue()) {
        shutdown();
        return matLayoutRes.error();
    }
    materialDescriptorSetLayout_ = matLayoutRes.value();

    // 3b. Create scene lighting descriptor set layout (Set 1)
    auto sceneLayoutDesc = createSceneDescriptorSetLayoutDesc();
    auto sceneLayoutRes = device_->createDescriptorSetLayout(sceneLayoutDesc);
    if (!sceneLayoutRes.hasValue()) {
        shutdown();
        return sceneLayoutRes.error();
    }
    sceneDescriptorSetLayout_ = sceneLayoutRes.value();

    // 3c. Create descriptor pool & allocate Set 1
    rhi::DescriptorPoolDesc poolDesc{
        .maxSets = 1,
        .poolSizes =
            {
                {.type = rhi::DescriptorType::UniformBuffer, .count = 1},
                {.type = rhi::DescriptorType::CombinedImageSampler, .count = 8},
            },
        .debugName = "SceneLightingDescriptorPool",
    };
    auto poolRes = device_->createDescriptorPool(poolDesc);
    if (!poolRes.hasValue()) {
        shutdown();
        return poolRes.error();
    }
    sceneDescriptorPool_ = poolRes.value();

    auto setRes = device_->allocateDescriptorSet(sceneDescriptorPool_, sceneDescriptorSetLayout_);
    if (!setRes.hasValue()) {
        shutdown();
        return setRes.error();
    }
    sceneDescriptorSet_ = setRes.value();

    // Initialize default procedural environment map
    auto envRes = GpuEnvironmentMap::createDefault(*device_);
    if (envRes.hasValue()) {
        envMap_ = std::move(envRes.value());
    } else {
        ENG_LOG_WARN("SceneRenderer", "Failed to create default procedural sky environment map");
    }

    // Initialize fallback 1x1 AO (white) and indirect GI (black) textures
    ImageData whiteImg{};
    whiteImg.width = 1;
    whiteImg.height = 1;
    whiteImg.channels = 4;
    whiteImg.format = rhi::TextureFormat::RGBA8_UNORM;
    whiteImg.pixels = {255, 255, 255, 255};
    auto aoTexRes = GpuTexture::create(*device_, whiteImg, false);
    if (aoTexRes.hasValue()) {
        defaultAoTexture_ = std::move(aoTexRes.value());
    }

    ImageData blackImg{};
    blackImg.width = 1;
    blackImg.height = 1;
    blackImg.channels = 4;
    blackImg.format = rhi::TextureFormat::RGBA8_UNORM;
    blackImg.pixels = {0, 0, 0, 255};
    auto indTexRes = GpuTexture::create(*device_, blackImg, false);
    if (indTexRes.hasValue()) {
        defaultIndirectTexture_ = std::move(indTexRes.value());
    }

    ImageData transparentImg{};
    transparentImg.width = 1;
    transparentImg.height = 1;
    transparentImg.channels = 4;
    transparentImg.format = rhi::TextureFormat::RGBA8_UNORM;
    transparentImg.pixels = {0, 0, 0, 0};
    auto ssrTexRes = GpuTexture::create(*device_, transparentImg, false);
    if (ssrTexRes.hasValue()) {
        defaultSsrTexture_ = std::move(ssrTexRes.value());
    }

    const rhi::TextureHandle irrTex =
        envMap_.irradianceMap ? envMap_.irradianceMap->getTexture() : rhi::TextureHandle{};
    const rhi::SamplerHandle irrSamp =
        envMap_.irradianceMap ? envMap_.irradianceMap->getSampler() : rhi::SamplerHandle{};
    const rhi::TextureHandle prefTex =
        envMap_.prefilteredMap ? envMap_.prefilteredMap->getTexture() : rhi::TextureHandle{};
    const rhi::SamplerHandle prefSamp =
        envMap_.prefilteredMap ? envMap_.prefilteredMap->getSampler() : rhi::SamplerHandle{};
    const rhi::TextureHandle lutTex =
        envMap_.brdfLut ? envMap_.brdfLut->getTexture() : rhi::TextureHandle{};
    const rhi::SamplerHandle lutSamp =
        envMap_.brdfLut ? envMap_.brdfLut->getSampler() : rhi::SamplerHandle{};
    const rhi::TextureHandle defAoTex =
        defaultAoTexture_ ? defaultAoTexture_->getTexture() : rhi::TextureHandle{};
    const rhi::SamplerHandle defAoSamp =
        defaultAoTexture_ ? defaultAoTexture_->getSampler() : rhi::SamplerHandle{};
    const rhi::TextureHandle defIndTex =
        defaultIndirectTexture_ ? defaultIndirectTexture_->getTexture() : rhi::TextureHandle{};
    const rhi::SamplerHandle defIndSamp =
        defaultIndirectTexture_ ? defaultIndirectTexture_->getSampler() : rhi::SamplerHandle{};
    const rhi::TextureHandle defSsrTex =
        defaultSsrTexture_ ? defaultSsrTexture_->getTexture() : rhi::TextureHandle{};
    const rhi::SamplerHandle defSsrSamp =
        defaultSsrTexture_ ? defaultSsrTexture_->getSampler() : rhi::SamplerHandle{};

    std::array<rhi::DescriptorWrite, 8> sceneWrites{
        rhi::DescriptorWrite::makeBuffer(0, rhi::DescriptorType::UniformBuffer,
                                         lightingUniformBuffer_, 0, sizeof(GpuLightingData)),
        rhi::DescriptorWrite::makeCombinedImageSampler(1, shadowDepthTarget_, shadowSampler_),
        rhi::DescriptorWrite::makeCombinedImageSampler(2, irrTex, irrSamp),
        rhi::DescriptorWrite::makeCombinedImageSampler(3, prefTex, prefSamp),
        rhi::DescriptorWrite::makeCombinedImageSampler(4, lutTex, lutSamp),
        rhi::DescriptorWrite::makeCombinedImageSampler(5, defAoTex, defAoSamp),
        rhi::DescriptorWrite::makeCombinedImageSampler(6, defIndTex, defIndSamp),
        rhi::DescriptorWrite::makeCombinedImageSampler(7, defSsrTex, defSsrSamp),
    };
    std::ignore = device_->updateDescriptorSet(sceneDescriptorSet_, sceneWrites);

    // 4. Compile vertex and fragment shaders from precompiled SPIR-V bytecodes
    rhi::ShaderDesc vertDesc{
        .stage = rhi::ShaderStageFlags::Vertex,
        .spirvCode = kPbrMeshVertexShaderSpirv,
        .entryPoint = "main",
        .debugName = "PbrMeshVertexShader",
    };
    auto vertRes = device_->createShader(vertDesc);
    if (!vertRes.hasValue()) {
        shutdown();
        return vertRes.error();
    }
    vertexShader_ = vertRes.value();

    rhi::ShaderDesc fragDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kGltfMaterialFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "PbrMaterialFragmentShader",
    };
    auto fragRes = device_->createShader(fragDesc);
    if (!fragRes.hasValue()) {
        shutdown();
        return fragRes.error();
    }
    fragmentShader_ = fragRes.value();

    rhi::ShaderDesc fragOpaqueDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kGltfMaterialOpaqueFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "PbrMaterialOpaqueFragmentShader",
    };
    auto fragOpaqueRes = device_->createShader(fragOpaqueDesc);
    if (!fragOpaqueRes.hasValue()) {
        shutdown();
        return fragOpaqueRes.error();
    }
    fragmentShaderOpaque_ = fragOpaqueRes.value();

    // 5. Create pipeline layout specifying 128 bytes of push constants for vertex stage
    rhi::PipelineLayoutDesc layoutDesc{
        .pushConstants = {{
            .stageFlags = rhi::ShaderStageFlags::Vertex,
            .offset = 0,
            .size = 128,
        }},
        .descriptorSetLayoutHandles = {materialDescriptorSetLayout_, sceneDescriptorSetLayout_},
        .descriptorSetLayouts = {},
        .debugName = "MeshPipelineLayout",
    };
    auto layoutRes = device_->createPipelineLayout(layoutDesc);
    if (!layoutRes.hasValue()) {
        shutdown();
        return layoutRes.error();
    }
    pipelineLayout_ = layoutRes.value();

    // 6. Build Graphics Pipeline for offscreen dynamic rendering
    rhi::GraphicsPipelineDesc pipeDesc{
        .vertexShader = vertexShader_,
        .fragmentShader = fragmentShader_,
        .vertexInput =
            {
                .bindings = {{
                    .binding = 0,
                    .stride = sizeof(Vertex),
                    .inputRate = rhi::VertexInputRate::Vertex,
                }},
                .attributes =
                    {
                        {
                            .location = 0,
                            .binding = 0,
                            .format = rhi::VertexFormat::Float3,
                            .offset = static_cast<uint32_t>(offsetof(Vertex, position)),
                        },
                        {
                            .location = 1,
                            .binding = 0,
                            .format = rhi::VertexFormat::Float3,
                            .offset = static_cast<uint32_t>(offsetof(Vertex, normal)),
                        },
                        {
                            .location = 2,
                            .binding = 0,
                            .format = rhi::VertexFormat::Float2,
                            .offset = static_cast<uint32_t>(offsetof(Vertex, uv)),
                        },
                        {
                            .location = 3,
                            .binding = 0,
                            .format = rhi::VertexFormat::Float4,
                            .offset = static_cast<uint32_t>(offsetof(Vertex, color)),
                        },
                        {
                            .location = 4,
                            .binding = 0,
                            .format = rhi::VertexFormat::Float4,
                            .offset = static_cast<uint32_t>(offsetof(Vertex, tangent)),
                        },
                    },
            },
        .topology = rhi::PrimitiveTopology::TriangleList,
        .rasterization =
            {
                .polygonMode = rhi::PolygonMode::Fill,
                .cullMode = rhi::CullMode::Back,
                .frontFace = rhi::FrontFace::CounterClockwise,
            },
        .depthStencil =
            {
                .depthTestEnable = true,
                .depthWriteEnable = true,
                .depthCompareOp = rhi::CompareOp::LessOrEqual,
            },
        .colorBlendAttachments = {{
            .blendEnable = false,
            .colorWriteMask = rhi::ColorComponentFlags::All,
        }},
        .layout = pipelineLayout_,
        .colorAttachmentFormats = {colorFormat_},
        .depthAttachmentFormat = depthFormat_,
        .debugName = "MeshGraphicsPipeline",
    };
    auto pipeRes = device_->createGraphicsPipeline(pipeDesc);
    if (!pipeRes.hasValue()) {
        shutdown();
        return pipeRes.error();
    }
    pipeline_ = pipeRes.value();

    if (hdrFormat_ != colorFormat_) {
        pipeDesc.colorAttachmentFormats = {hdrFormat_};
        pipeDesc.debugName = "MeshHdrGraphicsPipeline";
        auto hdrPipeRes = device_->createGraphicsPipeline(pipeDesc);
        if (!hdrPipeRes.hasValue()) {
            shutdown();
            return hdrPipeRes.error();
        }
        hdrPipeline_ = hdrPipeRes.value();
    } else {
        hdrPipeline_ = pipeline_;
    }

    // 6b. Build Opaque Pipelines (using pbr_mesh_opaque.frag with early_fragment_tests)
    auto pipeOpaqueDesc = pipeDesc;
    pipeOpaqueDesc.fragmentShader = fragmentShaderOpaque_;
    pipeOpaqueDesc.colorAttachmentFormats = {colorFormat_};
    pipeOpaqueDesc.debugName = "MeshGraphicsPipelineOpaque";
    auto pipeOpaqueRes = device_->createGraphicsPipeline(pipeOpaqueDesc);
    if (!pipeOpaqueRes.hasValue()) {
        shutdown();
        return pipeOpaqueRes.error();
    }
    pipelineOpaque_ = pipeOpaqueRes.value();

    if (hdrFormat_ != colorFormat_) {
        pipeOpaqueDesc.colorAttachmentFormats = {hdrFormat_};
        pipeOpaqueDesc.debugName = "MeshHdrGraphicsPipelineOpaque";
        auto hdrPipeOpaqueRes = device_->createGraphicsPipeline(pipeOpaqueDesc);
        if (!hdrPipeOpaqueRes.hasValue()) {
            shutdown();
            return hdrPipeOpaqueRes.error();
        }
        hdrPipelineOpaque_ = hdrPipeOpaqueRes.value();
    } else {
        hdrPipelineOpaque_ = pipelineOpaque_;
    }

    // 6c. Build Double-Sided Pipelines (CullMode::None for leaves, vegetation, foliage, cards)
    auto pipeDoubleDesc = pipeDesc;
    pipeDoubleDesc.rasterization.cullMode = rhi::CullMode::None;
    pipeDoubleDesc.colorAttachmentFormats = {colorFormat_};
    pipeDoubleDesc.debugName = "MeshGraphicsPipelineDoubleSided";
    auto pipeDoubleRes = device_->createGraphicsPipeline(pipeDoubleDesc);
    if (!pipeDoubleRes.hasValue()) {
        shutdown();
        return pipeDoubleRes.error();
    }
    pipelineDoubleSided_ = pipeDoubleRes.value();

    if (hdrFormat_ != colorFormat_) {
        pipeDoubleDesc.colorAttachmentFormats = {hdrFormat_};
        pipeDoubleDesc.debugName = "MeshHdrGraphicsPipelineDoubleSided";
        auto hdrPipeDoubleRes = device_->createGraphicsPipeline(pipeDoubleDesc);
        if (!hdrPipeDoubleRes.hasValue()) {
            shutdown();
            return hdrPipeDoubleRes.error();
        }
        hdrPipelineDoubleSided_ = hdrPipeDoubleRes.value();
    } else {
        hdrPipelineDoubleSided_ = pipelineDoubleSided_;
    }

    // 6d. Build Transparent Pipelines (BlendEnable = true, DepthWriteEnable = false, CompareOp =
    // LessOrEqual)
    auto pipeTransDesc = pipeDesc;
    pipeTransDesc.depthStencil.depthTestEnable = true;
    pipeTransDesc.depthStencil.depthWriteEnable = false;
    pipeTransDesc.depthStencil.depthCompareOp = rhi::CompareOp::LessOrEqual;
    pipeTransDesc.colorBlendAttachments = {{
        .blendEnable = true,
        .srcColorBlendFactor = rhi::BlendFactor::SrcAlpha,
        .dstColorBlendFactor = rhi::BlendFactor::OneMinusSrcAlpha,
        .colorBlendOp = rhi::BlendOp::Add,
        .srcAlphaBlendFactor = rhi::BlendFactor::One,
        .dstAlphaBlendFactor = rhi::BlendFactor::OneMinusSrcAlpha,
        .alphaBlendOp = rhi::BlendOp::Add,
        .colorWriteMask = rhi::ColorComponentFlags::All,
    }};
    pipeTransDesc.colorAttachmentFormats = {colorFormat_};
    pipeTransDesc.debugName = "MeshGraphicsPipelineTransparent";
    auto pipeTransRes = device_->createGraphicsPipeline(pipeTransDesc);
    if (!pipeTransRes.hasValue()) {
        shutdown();
        return pipeTransRes.error();
    }
    pipelineTransparent_ = pipeTransRes.value();

    if (hdrFormat_ != colorFormat_) {
        pipeTransDesc.colorAttachmentFormats = {hdrFormat_};
        pipeTransDesc.debugName = "MeshHdrGraphicsPipelineTransparent";
        auto hdrPipeTransRes = device_->createGraphicsPipeline(pipeTransDesc);
        if (!hdrPipeTransRes.hasValue()) {
            shutdown();
            return hdrPipeTransRes.error();
        }
        hdrPipelineTransparent_ = hdrPipeTransRes.value();
    } else {
        hdrPipelineTransparent_ = pipelineTransparent_;
    }

    // 6e. Build Transparent Double-Sided Pipelines
    auto pipeTransDoubleDesc = pipeTransDesc;
    pipeTransDoubleDesc.rasterization.cullMode = rhi::CullMode::None;
    pipeTransDoubleDesc.colorAttachmentFormats = {colorFormat_};
    pipeTransDoubleDesc.debugName = "MeshGraphicsPipelineTransparentDoubleSided";
    auto pipeTransDoubleRes = device_->createGraphicsPipeline(pipeTransDoubleDesc);
    if (!pipeTransDoubleRes.hasValue()) {
        shutdown();
        return pipeTransDoubleRes.error();
    }
    pipelineTransparentDoubleSided_ = pipeTransDoubleRes.value();

    if (hdrFormat_ != colorFormat_) {
        pipeTransDoubleDesc.colorAttachmentFormats = {hdrFormat_};
        pipeTransDoubleDesc.debugName = "MeshHdrGraphicsPipelineTransparentDoubleSided";
        auto hdrPipeTransDoubleRes = device_->createGraphicsPipeline(pipeTransDoubleDesc);
        if (!hdrPipeTransDoubleRes.hasValue()) {
            shutdown();
            return hdrPipeTransDoubleRes.error();
        }
        hdrPipelineTransparentDoubleSided_ = hdrPipeTransDoubleRes.value();
    } else {
        hdrPipelineTransparentDoubleSided_ = pipelineTransparentDoubleSided_;
    }

    // Optional wireframe rasterization pipelines (requires fillModeNonSolid GPU feature)
    if (device_->getCaps().features.fillModeNonSolid) {
        auto wireDesc = pipeDesc;
        wireDesc.rasterization.polygonMode = rhi::PolygonMode::Line;
        wireDesc.colorAttachmentFormats = {hdrFormat_};
        wireDesc.debugName = "MeshHdrWireframePipeline";
        auto wireRes = device_->createGraphicsPipeline(wireDesc);
        if (wireRes.hasValue()) {
            wireframePipeline_ = wireRes.value();
        }
        if (hdrFormat_ != colorFormat_) {
            wireDesc.colorAttachmentFormats = {colorFormat_};
            wireDesc.debugName = "MeshLdrWireframePipeline";
            auto wireLdrRes = device_->createGraphicsPipeline(wireDesc);
            if (wireLdrRes.hasValue()) {
                wireframeLdrPipeline_ = wireLdrRes.value();
            }
        }
    }

    // 7. Compile Shadow Depth Vertex Shader and create Shadow Pipeline
    rhi::ShaderDesc shadowVertDesc{
        .stage = rhi::ShaderStageFlags::Vertex,
        .spirvCode = kShadowDepthVertexShaderSpirv,
        .entryPoint = "main",
        .debugName = "ShadowDepthVertexShader",
    };
    auto shadowVertRes = device_->createShader(shadowVertDesc);
    if (!shadowVertRes.hasValue()) {
        shutdown();
        return shadowVertRes.error();
    }
    shadowVertexShader_ = shadowVertRes.value();

    rhi::PipelineLayoutDesc shadowLayoutDesc{
        .pushConstants = {{
            .stageFlags = rhi::ShaderStageFlags::Vertex,
            .offset = 0,
            .size = sizeof(Mat4),
        }},
        .descriptorSetLayoutHandles = {},
        .descriptorSetLayouts = {},
        .debugName = "ShadowPipelineLayout",
    };
    auto shadowLayoutRes = device_->createPipelineLayout(shadowLayoutDesc);
    if (!shadowLayoutRes.hasValue()) {
        shutdown();
        return shadowLayoutRes.error();
    }
    shadowPipelineLayout_ = shadowLayoutRes.value();

    rhi::GraphicsPipelineDesc shadowPipeDesc{
        .vertexShader = shadowVertexShader_,
        .fragmentShader = {},
        .vertexInput =
            {
                .bindings = {{
                    .binding = 0,
                    .stride = sizeof(Vertex),
                    .inputRate = rhi::VertexInputRate::Vertex,
                }},
                .attributes =
                    {
                        {
                            .location = 0,
                            .binding = 0,
                            .format = rhi::VertexFormat::Float3,
                            .offset = static_cast<uint32_t>(offsetof(Vertex, position)),
                        },
                    },
            },
        .topology = rhi::PrimitiveTopology::TriangleList,
        .rasterization =
            {
                .polygonMode = rhi::PolygonMode::Fill,
                .cullMode = rhi::CullMode::Back,
                .frontFace = rhi::FrontFace::CounterClockwise,
            },
        .depthStencil =
            {
                .depthTestEnable = true,
                .depthWriteEnable = true,
                .depthCompareOp = rhi::CompareOp::LessOrEqual,
            },
        .colorBlendAttachments = {},
        .layout = shadowPipelineLayout_,
        .colorAttachmentFormats = {},
        .depthAttachmentFormat = rhi::TextureFormat::D32_FLOAT,
        .debugName = "ShadowGraphicsPipeline",
    };
    auto shadowPipeRes = device_->createGraphicsPipeline(shadowPipeDesc);
    if (!shadowPipeRes.hasValue()) {
        shutdown();
        return shadowPipeRes.error();
    }
    shadowPipeline_ = shadowPipeRes.value();

    // 7b. Create Depth Prepass Pipeline (vertex-only writing to scene depthTarget_)
    rhi::GraphicsPipelineDesc depthPrepassDesc = shadowPipeDesc;
    depthPrepassDesc.depthAttachmentFormat = depthFormat_;
    depthPrepassDesc.rasterization.cullMode = rhi::CullMode::None;
    depthPrepassDesc.debugName = "DepthPrepassPipeline";
    auto depthPrepassRes = device_->createGraphicsPipeline(depthPrepassDesc);
    if (depthPrepassRes.hasValue()) {
        depthPrepassPipeline_ = depthPrepassRes.value();
    }

    // 7c. Main PBR Pipelines for Depth Prepass (depth test Equal/LessOrEqual with depth write
    // disabled)
    rhi::GraphicsPipelineDesc pipeDepthPrepassDesc = pipeDesc;
    pipeDepthPrepassDesc.depthStencil.depthTestEnable = true;
    pipeDepthPrepassDesc.depthStencil.depthWriteEnable = false;
    pipeDepthPrepassDesc.depthStencil.depthCompareOp = rhi::CompareOp::LessOrEqual;
    pipeDepthPrepassDesc.debugName = "MeshGraphicsPipeline_DepthPrepass";
    auto pipeDpRes = device_->createGraphicsPipeline(pipeDepthPrepassDesc);
    if (pipeDpRes.hasValue()) {
        pipelineDepthPrepass_ = pipeDpRes.value();
    }

    if (hdrFormat_ != colorFormat_) {
        pipeDepthPrepassDesc.colorAttachmentFormats = {hdrFormat_};
        pipeDepthPrepassDesc.debugName = "MeshHdrGraphicsPipeline_DepthPrepass";
        auto hdrDpRes = device_->createGraphicsPipeline(pipeDepthPrepassDesc);
        if (hdrDpRes.hasValue()) {
            hdrPipelineDepthPrepass_ = hdrDpRes.value();
        }
    } else {
        hdrPipelineDepthPrepass_ = pipelineDepthPrepass_;
    }

    // 7d. Opaque Main PBR Pipelines for Depth Prepass (with early_fragment_tests)
    auto pipeDepthPrepassOpaqueDesc = pipeDepthPrepassDesc;
    pipeDepthPrepassOpaqueDesc.fragmentShader = fragmentShaderOpaque_;
    pipeDepthPrepassOpaqueDesc.colorAttachmentFormats = {colorFormat_};
    pipeDepthPrepassOpaqueDesc.debugName = "MeshGraphicsPipeline_DepthPrepassOpaque";
    auto pipeDpOpaqueRes = device_->createGraphicsPipeline(pipeDepthPrepassOpaqueDesc);
    if (pipeDpOpaqueRes.hasValue()) {
        pipelineDepthPrepassOpaque_ = pipeDpOpaqueRes.value();
    }

    if (hdrFormat_ != colorFormat_) {
        pipeDepthPrepassOpaqueDesc.colorAttachmentFormats = {hdrFormat_};
        pipeDepthPrepassOpaqueDesc.debugName = "MeshHdrGraphicsPipeline_DepthPrepassOpaque";
        auto hdrDpOpaqueRes = device_->createGraphicsPipeline(pipeDepthPrepassOpaqueDesc);
        if (hdrDpOpaqueRes.hasValue()) {
            hdrPipelineDepthPrepassOpaque_ = hdrDpOpaqueRes.value();
        }
    } else {
        hdrPipelineDepthPrepassOpaque_ = pipelineDepthPrepassOpaque_;
    }

    // 8. Initialize Post-Processing & HDR Resources
    auto ppRes = initPostProcessResources();
    if (!ppRes.hasValue()) {
        shutdown();
        return ppRes.error();
    }

    // 9. Initialize Global Illumination (Milestone 11D) Resources
    auto giRes = initGiResources();
    if (!giRes.hasValue()) {
        shutdown();
        return giRes.error();
    }

    auto giTargetsRes = createGiTargets(width_, height_);
    if (!giTargetsRes.hasValue()) {
        shutdown();
        return giTargetsRes.error();
    }

    // 10. Initialize Temporal Anti-Aliasing (Milestone 11H) Resources
    auto taaRes = initTaaResources();
    if (!taaRes.hasValue()) {
        shutdown();
        return taaRes.error();
    }

    auto taaTargetsRes = createTaaTargets(width_, height_);
    if (!taaTargetsRes.hasValue()) {
        shutdown();
        return taaTargetsRes.error();
    }

    auto ppTargetsRes = createPostProcessTargets(width_, height_);
    if (!ppTargetsRes.hasValue()) {
        shutdown();
        return ppTargetsRes.error();
    }

    settings_ = GraphicsSettings::createPreset(GraphicsQualityPreset::High);
    settings_ = settings_.validateAgainstCaps(device_->getCaps());

    buildRenderGraph();
    buildPostProcessGraph();

    return Result<void>::success();
}

void SceneRenderer::shutdown() {
    if (device_ != nullptr) {
        envMap_ = {};
        destroyTaaTargets();
        destroyTaaResources();
        destroyPostProcessTargets();
        destroyGiResources();

        if (toneMappingPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(toneMappingPipeline_);
            toneMappingPipeline_ = {};
        }
        if (motionBlurPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(motionBlurPipeline_);
            motionBlurPipeline_ = {};
        }
        if (dofPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(dofPipeline_);
            dofPipeline_ = {};
        }
        if (bloomUpPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(bloomUpPipeline_);
            bloomUpPipeline_ = {};
        }
        if (bloomDownPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(bloomDownPipeline_);
            bloomDownPipeline_ = {};
        }
        if (toneMappingPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(toneMappingPipelineLayout_);
            toneMappingPipelineLayout_ = {};
        }
        if (motionBlurPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(motionBlurPipelineLayout_);
            motionBlurPipelineLayout_ = {};
        }
        if (dofPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(dofPipelineLayout_);
            dofPipelineLayout_ = {};
        }
        if (bloomUpPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(bloomUpPipelineLayout_);
            bloomUpPipelineLayout_ = {};
        }
        if (bloomDownPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(bloomDownPipelineLayout_);
            bloomDownPipelineLayout_ = {};
        }
        if (postProcessDescriptorPool_.isValid()) {
            device_->destroyDescriptorPool(postProcessDescriptorPool_);
            postProcessDescriptorPool_ = {};
            for (auto& s : bloomDownSets_) {
                s = {};
            }
            for (auto& s : bloomUpSets_) {
                s = {};
            }
            dofSet_ = {};
            motionBlurSet_ = {};
            toneMappingSet_ = {};
        }
        if (dualTextureSetLayout_.isValid()) {
            device_->destroyDescriptorSetLayout(dualTextureSetLayout_);
            dualTextureSetLayout_ = {};
        }
        if (singleTextureSetLayout_.isValid()) {
            device_->destroyDescriptorSetLayout(singleTextureSetLayout_);
            singleTextureSetLayout_ = {};
        }
        if (toneMappingFragmentShader_.isValid()) {
            device_->destroyShader(toneMappingFragmentShader_);
            toneMappingFragmentShader_ = {};
        }
        if (motionBlurFragmentShader_.isValid()) {
            device_->destroyShader(motionBlurFragmentShader_);
            motionBlurFragmentShader_ = {};
        }
        if (dofFragmentShader_.isValid()) {
            device_->destroyShader(dofFragmentShader_);
            dofFragmentShader_ = {};
        }
        if (bloomUpFragmentShader_.isValid()) {
            device_->destroyShader(bloomUpFragmentShader_);
            bloomUpFragmentShader_ = {};
        }
        if (bloomDownFragmentShader_.isValid()) {
            device_->destroyShader(bloomDownFragmentShader_);
            bloomDownFragmentShader_ = {};
        }
        if (fullscreenVertexShader_.isValid()) {
            device_->destroyShader(fullscreenVertexShader_);
            fullscreenVertexShader_ = {};
        }
        if (postProcessSampler_.isValid()) {
            device_->destroySampler(postProcessSampler_);
            postProcessSampler_ = {};
        }
        customLutTexture_.reset();
        defaultLutTexture_.reset();
        loadedLutPath_.clear();
        if (hdrPipeline_.isValid() && hdrPipeline_ != pipeline_) {
            device_->destroyGraphicsPipeline(hdrPipeline_);
        }
        hdrPipeline_ = {};

        if (hdrPipelineOpaque_.isValid() && hdrPipelineOpaque_ != pipelineOpaque_) {
            device_->destroyGraphicsPipeline(hdrPipelineOpaque_);
        }
        hdrPipelineOpaque_ = {};

        if (pipelineOpaque_.isValid()) {
            device_->destroyGraphicsPipeline(pipelineOpaque_);
            pipelineOpaque_ = {};
        }

        if (hdrPipelineDoubleSided_.isValid() && hdrPipelineDoubleSided_ != pipelineDoubleSided_) {
            device_->destroyGraphicsPipeline(hdrPipelineDoubleSided_);
        }
        hdrPipelineDoubleSided_ = {};

        if (pipelineDoubleSided_.isValid()) {
            device_->destroyGraphicsPipeline(pipelineDoubleSided_);
            pipelineDoubleSided_ = {};
        }

        if (hdrPipelineTransparentDoubleSided_.isValid() &&
            hdrPipelineTransparentDoubleSided_ != pipelineTransparentDoubleSided_) {
            device_->destroyGraphicsPipeline(hdrPipelineTransparentDoubleSided_);
        }
        hdrPipelineTransparentDoubleSided_ = {};

        if (pipelineTransparentDoubleSided_.isValid()) {
            device_->destroyGraphicsPipeline(pipelineTransparentDoubleSided_);
            pipelineTransparentDoubleSided_ = {};
        }

        if (hdrPipelineTransparent_.isValid() && hdrPipelineTransparent_ != pipelineTransparent_) {
            device_->destroyGraphicsPipeline(hdrPipelineTransparent_);
        }
        hdrPipelineTransparent_ = {};

        if (pipelineTransparent_.isValid()) {
            device_->destroyGraphicsPipeline(pipelineTransparent_);
            pipelineTransparent_ = {};
        }

        if (wireframePipeline_.isValid()) {
            device_->destroyGraphicsPipeline(wireframePipeline_);
            wireframePipeline_ = {};
        }
        if (wireframeLdrPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(wireframeLdrPipeline_);
            wireframeLdrPipeline_ = {};
        }

        if (depthPrepassPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(depthPrepassPipeline_);
            depthPrepassPipeline_ = {};
        }
        if (pipelineDepthPrepass_.isValid()) {
            device_->destroyGraphicsPipeline(pipelineDepthPrepass_);
            pipelineDepthPrepass_ = {};
        }
        if (pipelineDepthPrepassOpaque_.isValid() &&
            pipelineDepthPrepassOpaque_ != pipelineDepthPrepass_) {
            device_->destroyGraphicsPipeline(pipelineDepthPrepassOpaque_);
            pipelineDepthPrepassOpaque_ = {};
        }
        if (hdrPipelineDepthPrepass_.isValid() &&
            hdrPipelineDepthPrepass_ != pipelineDepthPrepass_) {
            device_->destroyGraphicsPipeline(hdrPipelineDepthPrepass_);
            hdrPipelineDepthPrepass_ = {};
        }
        if (hdrPipelineDepthPrepassOpaque_.isValid() &&
            hdrPipelineDepthPrepassOpaque_ != pipelineDepthPrepassOpaque_ &&
            hdrPipelineDepthPrepassOpaque_ != hdrPipelineDepthPrepass_) {
            device_->destroyGraphicsPipeline(hdrPipelineDepthPrepassOpaque_);
            hdrPipelineDepthPrepassOpaque_ = {};
        }

        if (shadowPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(shadowPipeline_);
            shadowPipeline_ = {};
        }
        if (shadowPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(shadowPipelineLayout_);
            shadowPipelineLayout_ = {};
        }
        if (shadowVertexShader_.isValid()) {
            device_->destroyShader(shadowVertexShader_);
            shadowVertexShader_ = {};
        }
        if (shadowSampler_.isValid()) {
            device_->destroySampler(shadowSampler_);
            shadowSampler_ = {};
        }
        if (shadowDepthTarget_.isValid()) {
            device_->destroyTexture(shadowDepthTarget_);
            shadowDepthTarget_ = {};
        }
        if (staticShadowTarget_.isValid()) {
            device_->destroyTexture(staticShadowTarget_);
            staticShadowTarget_ = {};
        }
        if (sceneDescriptorPool_.isValid()) {
            device_->destroyDescriptorPool(sceneDescriptorPool_);
            sceneDescriptorPool_ = {};
            sceneDescriptorSet_ = {};
        }
        if (sceneDescriptorSetLayout_.isValid()) {
            device_->destroyDescriptorSetLayout(sceneDescriptorSetLayout_);
            sceneDescriptorSetLayout_ = {};
        }
        if (lightingUniformBuffer_.isValid()) {
            device_->destroyBuffer(lightingUniformBuffer_);
            lightingUniformBuffer_ = {};
        }
        if (pipeline_.isValid()) {
            device_->destroyGraphicsPipeline(pipeline_);
            pipeline_ = {};
        }
        if (pipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(pipelineLayout_);
            pipelineLayout_ = {};
        }
        if (materialDescriptorSetLayout_.isValid()) {
            device_->destroyDescriptorSetLayout(materialDescriptorSetLayout_);
            materialDescriptorSetLayout_ = {};
        }
        if (fragmentShader_.isValid()) {
            device_->destroyShader(fragmentShader_);
            fragmentShader_ = {};
        }
        if (fragmentShaderOpaque_.isValid()) {
            device_->destroyShader(fragmentShaderOpaque_);
            fragmentShaderOpaque_ = {};
        }
        if (vertexShader_.isValid()) {
            device_->destroyShader(vertexShader_);
            vertexShader_ = {};
        }
        if (depthTarget_.isValid()) {
            device_->destroyTexture(depthTarget_);
            depthTarget_ = {};
        }
        if (colorTarget_.isValid()) {
            device_->destroyTexture(colorTarget_);
            colorTarget_ = {};
        }
        device_ = nullptr;
    }
    renderGraph_.reset();
    postProcessGraph_.reset();
    currentWorld_ = nullptr;
    currentAssetManager_ = nullptr;
    currentDevice_ = nullptr;
    width_ = 0;
    height_ = 0;
}

Result<void> SceneRenderer::resize(uint32_t width, uint32_t height) {
    if (device_ == nullptr) {
        return Error{.code = ErrorCode::InvalidState,
                     .message = "SceneRenderer not initialized for resize"};
    }
    if (width == 0 || height == 0) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Resize dimensions must be non-zero"};
    }
    if (width == width_ && height == height_) {
        return Result<void>::success();
    }

    // Synchronize GPU to prevent destroying textures in use by pending command buffers (Finding
    // F-04)
    device_->waitIdle();

    if (colorTarget_.isValid()) {
        device_->destroyTexture(colorTarget_);
        colorTarget_ = {};
    }
    if (depthTarget_.isValid()) {
        device_->destroyTexture(depthTarget_);
        depthTarget_ = {};
    }

    destroyPostProcessTargets();
    destroyGiTargets();
    destroyTaaTargets();

    width_ = width;
    height_ = height;
    shadowDirty_ = true;

    rhi::TextureDesc colorDesc{
        .type = rhi::TextureType::Texture2D,
        .format = colorFormat_,
        .width = width_,
        .height = height_,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = rhi::TextureUsageFlags::ColorAttachment | rhi::TextureUsageFlags::Sampled |
                 rhi::TextureUsageFlags::TransferSrc,
        .debugName = "SceneRendererColorTarget",
    };
    auto colorRes = device_->createTexture(colorDesc);
    if (!colorRes.hasValue()) {
        return colorRes.error();
    }
    colorTarget_ = colorRes.value();

    rhi::TextureDesc depthDesc{
        .type = rhi::TextureType::Texture2D,
        .format = depthFormat_,
        .width = width_,
        .height = height_,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = rhi::TextureUsageFlags::DepthStencilAttachment | rhi::TextureUsageFlags::Sampled |
                 rhi::TextureUsageFlags::TransferSrc,
        .debugName = "SceneRendererDepthTarget",
    };
    auto depthRes = device_->createTexture(depthDesc);
    if (!depthRes.hasValue()) {
        return depthRes.error();
    }
    depthTarget_ = depthRes.value();

    auto giTargetsRes = createGiTargets(width_, height_);
    if (!giTargetsRes.hasValue()) {
        return giTargetsRes.error();
    }
    auto taaTargetsRes = createTaaTargets(width_, height_);
    if (!taaTargetsRes.hasValue()) {
        return taaTargetsRes.error();
    }
    auto ppRes = createPostProcessTargets(width_, height_);
    if (!ppRes.hasValue()) {
        return ppRes.error();
    }
    isHistoryValid_ = false;
    invalidateTaaHistory();

    buildRenderGraph();
    buildPostProcessGraph();

    return Result<void>::success();
}

Result<void> SceneRenderer::applyGraphicsSettings(const GraphicsSettings& newSettings) {
    if (device_ == nullptr) {
        settings_ = newSettings;
        return Result<void>::success();
    }

    // 1. Validate & clamp against hardware capabilities
    GraphicsSettings validated = newSettings.validateAgainstCaps(device_->getCaps());

    // 2. Classify change severity
    [[maybe_unused]] auto severity = settings_.computeChangeSeverity(validated);

    // 3. Shadow resolution changes require recreating shadow depth target and updating descriptors
    if (validated.shadows.resolution != shadowResolution_) {
        device_->waitIdle();
        shadowResolution_ = validated.shadows.resolution;

        if (shadowDepthTarget_.isValid()) {
            device_->destroyTexture(shadowDepthTarget_);
            shadowDepthTarget_ = {};
        }
        if (staticShadowTarget_.isValid()) {
            device_->destroyTexture(staticShadowTarget_);
            staticShadowTarget_ = {};
        }

        rhi::TextureDesc shadowDesc{
            .type = rhi::TextureType::Texture2D,
            .format = rhi::TextureFormat::D32_FLOAT,
            .width = shadowResolution_,
            .height = shadowResolution_,
            .depth = 1,
            .mipLevels = 1,
            .arrayLayers = 1,
            .usage =
                rhi::TextureUsageFlags::DepthStencilAttachment | rhi::TextureUsageFlags::Sampled,
            .debugName = "ShadowDepthTarget",
        };
        auto shadowRes = device_->createTexture(shadowDesc);
        if (!shadowRes.hasValue()) {
            return shadowRes.error();
        }
        shadowDepthTarget_ = shadowRes.value();

        shadowDesc.debugName = "StaticShadowMapCache";
        auto staticShadowRes = device_->createTexture(shadowDesc);
        if (!staticShadowRes.hasValue()) {
            return staticShadowRes.error();
        }
        staticShadowTarget_ = staticShadowRes.value();
        staticShadowDirty_ = true;

        if (sceneDescriptorSet_.isValid() && shadowSampler_.isValid()) {
            std::array<rhi::DescriptorWrite, 1> shadowWrite{
                rhi::DescriptorWrite::makeCombinedImageSampler(1, shadowDepthTarget_,
                                                               shadowSampler_),
            };
            std::ignore = device_->updateDescriptorSet(sceneDescriptorSet_, shadowWrite);
        }
    }

    // 3b. Material texture quality and anisotropic filtering propagation
    if (settings_.quality.anisotropicFiltering != validated.quality.anisotropicFiltering ||
        settings_.quality.textureQuality != validated.quality.textureQuality) {
        if (device_ != nullptr && currentAssetManager_ != nullptr) {
            device_->waitIdle();
            std::ignore = currentAssetManager_->updateSamplerQuality(validated.quality, *device_);
        }
    }

    // 4. Synchronize post-processing runtime controls
    setPostProcessSettings(validated.postProcess);

    if (settings_.lighting.timeOfDay != validated.lighting.timeOfDay) {
        switch (validated.lighting.timeOfDay) {
        case EnvironmentTimeOfDay::Daylight:
            std::ignore = applyAtmosphere(AtmosphereParameters::createDaylight());
            break;
        case EnvironmentTimeOfDay::Sunset:
            std::ignore = applyAtmosphere(AtmosphereParameters::createSunset());
            break;
        case EnvironmentTimeOfDay::Night:
            std::ignore = applyAtmosphere(AtmosphereParameters::createNight());
            break;
        case EnvironmentTimeOfDay::Custom:
            break;
        }
    }

    // 5. Hardware VSync toggle
    device_->setVsync(validated.performance.vsync);

    if (!(settings_.shadows == validated.shadows)) {
        shadowDirty_ = true;
        staticShadowDirty_ = true;
        staticCastersListDirty_ = true;
    }

    if (settings_.quality.antiAliasing != validated.quality.antiAliasing ||
        !(settings_.quality.taa == validated.quality.taa)) {
        invalidateTaaHistory();
    }

    settings_ = validated;
    buildRenderGraph();
    buildPostProcessGraph();
    return Result<void>::success();
}

void SceneRenderer::setEnvironmentMap(GpuEnvironmentMap&& envMap) {
    if (!envMap.isValid()) {
        return;
    }
    envMap_ = std::move(envMap);
    if (device_ != nullptr && sceneDescriptorSet_.isValid() && envMap_.isValid()) {
        std::array<rhi::DescriptorWrite, 3> iblWrites{
            rhi::DescriptorWrite::makeCombinedImageSampler(2, envMap_.irradianceMap->getTexture(),
                                                           envMap_.irradianceMap->getSampler()),
            rhi::DescriptorWrite::makeCombinedImageSampler(3, envMap_.prefilteredMap->getTexture(),
                                                           envMap_.prefilteredMap->getSampler()),
            rhi::DescriptorWrite::makeCombinedImageSampler(4, envMap_.brdfLut->getTexture(),
                                                           envMap_.brdfLut->getSampler()),
        };
        std::ignore = device_->updateDescriptorSet(sceneDescriptorSet_, iblWrites);
    }
}

Result<void> SceneRenderer::loadEnvironmentMap(const HdrImage& hdr) {
    if (device_ == nullptr) {
        return Error{.code = ErrorCode::InvalidState, .message = "Device not initialized"};
    }
    auto envRes = GpuEnvironmentMap::createFromHdr(*device_, hdr);
    if (!envRes.hasValue()) {
        return envRes.error();
    }
    setEnvironmentMap(std::move(envRes.value()));
    return Result<void>::success();
}

Result<void> SceneRenderer::loadEnvironmentMapFromFile(std::string_view filePath) {
    auto hdrRes = IblProcessor::loadHdrFromFile(filePath);
    if (!hdrRes.hasValue()) {
        return hdrRes.error();
    }
    return loadEnvironmentMap(hdrRes.value());
}

Result<void> SceneRenderer::applyAtmosphere(const AtmosphereParameters& params) {
    atmosphereParams_ = params;
    postProcessSettings_.whiteBalance = params.whiteBalance;
    postProcessSettings_.exposure = params.exposure;
    postProcessSettings_.contrast = params.contrast;
    postProcessSettings_.saturation = params.saturation;

    if (device_ != nullptr && settings_.lighting.iblEnabled) {
        auto envRes = GpuEnvironmentMap::createAtmosphere(*device_, params, 64, 16, 32, 4);
        if (!envRes.hasValue()) {
            return envRes.error();
        }
        setEnvironmentMap(std::move(envRes.value()));
    }
    return Result<void>::success();
}

Result<void> SceneRenderer::applyAtmosphere(world::World& world,
                                            const AtmosphereParameters& params) {
    auto res = applyAtmosphere(params);
    if (!res.hasValue()) {
        return res;
    }

    auto& ecs = world.getRawWorld();
    bool found = false;
    auto dirQuery = ecs.query<DirectionalLightComponent>();
    dirQuery.each([&](flecs::entity e, DirectionalLightComponent& comp) {
        static_cast<void>(e);
        comp.direction = params.sunDirection.normalized();
        comp.color = params.sunColor;
        comp.intensity = params.sunIntensity;
        comp.castShadows = true;
        comp.enabled = true;
        found = true;
    });

    if (!found) {
        auto e = ecs.entity("SunMoonLight");
        e.set(DirectionalLightComponent{
            .direction = params.sunDirection.normalized(),
            .color = params.sunColor,
            .intensity = params.sunIntensity,
            .castShadows = true,
            .shadowBias = 0.005F,
            .enabled = true,
        });
    }

    return Result<void>::success();
}

void SceneRenderer::buildRenderGraph() {
    renderGraph_.reset();

    const bool useTaa = isTaaEnabled() && hdrRawTarget_.isValid() && velocityTarget_.isValid() &&
                        taaPipeline_.isValid() && sharpenPipeline_.isValid();
    const bool useHdr = postProcessSettings_.enabled && hdrTarget_.isValid();
    const bool useMotionBlur =
        useHdr && postProcessSettings_.motionBlurEnabled && velocityTarget_.isValid();
    const bool needsVelocity = (useTaa || useMotionBlur) && velocityPipeline_.isValid();
    const rhi::TextureHandle sceneColorTarget =
        useTaa ? hdrRawTarget_ : (useHdr ? hdrTarget_ : colorTarget_);

    GraphTextureId colorId{};
    const bool hasColorTarget = sceneColorTarget.isValid();
    if (hasColorTarget) {
        colorId = renderGraph_.importTexture("SceneColorTarget", sceneColorTarget);
    }
    GraphTextureId depthId{};
    const bool hasDepthTarget = depthTarget_.isValid();
    if (hasDepthTarget) {
        depthId = renderGraph_.importTexture("DepthTarget", depthTarget_);
    }

    GraphTextureId velocityId{};
    bool hasVelocityTarget = false;
    GraphTextureId currHistoryId{};
    bool hasCurrHistory = false;
    GraphTextureId prevHistoryId{};
    bool hasPrevHistory = false;
    GraphTextureId finalHdrId{};
    bool hasFinalHdr = false;
    if (needsVelocity && velocityTarget_.isValid()) {
        velocityId = renderGraph_.importTexture("VelocityTarget", velocityTarget_);
        hasVelocityTarget = true;
    }
    if (useTaa) {
        const uint32_t currSlot = frameSlotIndex_ % 2;
        const uint32_t prevSlot = (frameSlotIndex_ + 1) % 2;
        if (hdrHistoryTarget_[currSlot].isValid()) {
            currHistoryId =
                renderGraph_.importTexture("TaaCurrentHistoryTarget", hdrHistoryTarget_[currSlot]);
            hasCurrHistory = true;
        }
        if (hdrHistoryTarget_[prevSlot].isValid()) {
            prevHistoryId =
                renderGraph_.importTexture("TaaPreviousHistoryTarget", hdrHistoryTarget_[prevSlot]);
            hasPrevHistory = true;
        }
        if (hdrTarget_.isValid()) {
            finalHdrId = renderGraph_.importTexture("HdrAntiAliasedTarget", hdrTarget_);
            hasFinalHdr = true;
        }
    }

    const bool hasShadowPass =
        settings_.shadows.enabled && shadowDepthTarget_.isValid() && shadowPipeline_.isValid();
    GraphTextureId shadowId{};
    const bool hasShadowTarget = hasShadowPass;
    if (hasShadowPass) {
        shadowId = renderGraph_.importTexture("ShadowDepthTarget", shadowDepthTarget_);
    }

    GraphBufferId lightBufId{};
    const bool hasLightBuf = lightingUniformBuffer_.isValid();
    if (hasLightBuf) {
        lightBufId = renderGraph_.importBuffer("LightingBuffer", lightingUniformBuffer_);
    }

    const bool hasGtao = (settings_.lighting.gtaoEnabled || settings_.shadows.contactShadows) &&
                         gtaoPipeline_.isValid() && gtaoBlurPipeline_.isValid() &&
                         gtaoRawTarget_.isValid() && gtaoFilteredTarget_.isValid();
    const bool hasSsgi = settings_.lighting.ssgiEnabled && ssgiPipeline_.isValid() &&
                         ssgiRawTarget_.isValid() && ssgiFilteredTarget_.isValid();
    const bool hasSsr = settings_.lighting.ssrEnabled && ssrPipeline_.isValid() &&
                        ssrRawTarget_.isValid() && ssrFilteredTarget_.isValid();

    GraphTextureId gtaoRawId{};
    GraphTextureId gtaoFilteredId{};
    if (hasGtao) {
        gtaoRawId = renderGraph_.importTexture("GtaoRawTarget", gtaoRawTarget_);
        gtaoFilteredId = renderGraph_.importTexture("GtaoFilteredTarget", gtaoFilteredTarget_);
    }

    GraphTextureId ssgiRawId{};
    GraphTextureId ssgiFilteredId{};
    if (hasSsgi) {
        ssgiRawId = renderGraph_.importTexture("SsgiRawTarget", ssgiRawTarget_);
        ssgiFilteredId = renderGraph_.importTexture("SsgiFilteredTarget", ssgiFilteredTarget_);
    }

    GraphTextureId ssrRawId{};
    GraphTextureId ssrFilteredId{};
    if (hasSsr) {
        ssrRawId = renderGraph_.importTexture("SsrRawTarget", ssrRawTarget_);
        ssrFilteredId = renderGraph_.importTexture("SsrFilteredTarget", ssrFilteredTarget_);
    }

    // 1. Directional Shadow Pass
    if (hasShadowPass) {
        renderGraph_.addPass(
            "DirectionalShadowPass",
            [shadowId](RenderPassBuilder& builder) {
                builder.writeTexture(shadowId, rhi::TextureUsageFlags::DepthStencilAttachment);
            },
            [this](rhi::RhiCommandList& cmdList) { recordShadowPass(cmdList); });
    }

    // 2. Early Depth Prepass (required for depth prepass setting OR GTAO/SSGI/SSR)
    const bool hasDepthPrepass =
        (settings_.performance.depthPrepass || hasGtao || hasSsgi || hasSsr) &&
        depthPrepassPipeline_.isValid() && depthTarget_.isValid();
    if (hasDepthPrepass) {
        renderGraph_.addPass(
            "DepthPrepass",
            [depthId](RenderPassBuilder& builder) {
                if (depthId.isValid()) {
                    builder.writeTexture(depthId, rhi::TextureUsageFlags::DepthStencilAttachment);
                }
            },
            [this](rhi::RhiCommandList& cmdList) { recordDepthPrepass(cmdList); });
    }

    // 3. Ground-Truth Ambient Occlusion (GTAO) Pass
    if (hasGtao && depthId.isValid() && gtaoRawId.isValid() && gtaoFilteredId.isValid()) {
        renderGraph_.addPass(
            "GtaoPass",
            [depthId, gtaoRawId](RenderPassBuilder& builder) {
                builder.readTexture(depthId, rhi::TextureUsageFlags::Sampled);
                builder.writeTexture(gtaoRawId, rhi::TextureUsageFlags::ColorAttachment);
            },
            [this](rhi::RhiCommandList& cmdList) { recordGtaoPass(cmdList); });

        renderGraph_.addPass(
            "GtaoBilateralBlurPass",
            [gtaoRawId, depthId, gtaoFilteredId](RenderPassBuilder& builder) {
                builder.readTexture(gtaoRawId, rhi::TextureUsageFlags::Sampled);
                builder.readTexture(depthId, rhi::TextureUsageFlags::Sampled);
                builder.writeTexture(gtaoFilteredId, rhi::TextureUsageFlags::ColorAttachment);
            },
            [this](rhi::RhiCommandList& cmdList) { recordGtaoBlurPass(cmdList); });
    }

    // 4. Screen-Space Global Illumination (SSGI) & Temporal Denoising Pass
    if (hasSsgi && depthId.isValid() && ssgiRawId.isValid() && ssgiFilteredId.isValid()) {
        renderGraph_.addPass(
            "SsgiPass",
            [depthId, colorId, ssgiRawId](RenderPassBuilder& builder) {
                builder.readTexture(depthId, rhi::TextureUsageFlags::Sampled);
                if (colorId.isValid()) {
                    builder.readTexture(colorId, rhi::TextureUsageFlags::Sampled);
                }
                builder.writeTexture(ssgiRawId, rhi::TextureUsageFlags::ColorAttachment);
            },
            [this](rhi::RhiCommandList& cmdList) { recordSsgiPass(cmdList); });

        renderGraph_.addPass(
            "TemporalFilterPass",
            [ssgiRawId, depthId, ssgiFilteredId](RenderPassBuilder& builder) {
                builder.readTexture(ssgiRawId, rhi::TextureUsageFlags::Sampled);
                builder.readTexture(depthId, rhi::TextureUsageFlags::Sampled);
                builder.writeTexture(ssgiFilteredId, rhi::TextureUsageFlags::ColorAttachment);
            },
            [this](rhi::RhiCommandList& cmdList) { recordTemporalFilterPass(cmdList); });
    }

    // 4b. Screen-Space Reflections (SSR) & Temporal Denoising Pass
    if (hasSsr && depthId.isValid() && ssrRawId.isValid() && ssrFilteredId.isValid()) {
        renderGraph_.addPass(
            "SsrPass",
            [depthId, colorId, ssrRawId](RenderPassBuilder& builder) {
                builder.readTexture(depthId, rhi::TextureUsageFlags::Sampled);
                if (colorId.isValid()) {
                    builder.readTexture(colorId, rhi::TextureUsageFlags::Sampled);
                }
                builder.writeTexture(ssrRawId, rhi::TextureUsageFlags::ColorAttachment);
            },
            [this](rhi::RhiCommandList& cmdList) { recordSsrPass(cmdList); });

        renderGraph_.addPass(
            "SsrTemporalFilterPass",
            [ssrRawId, depthId, ssrFilteredId](RenderPassBuilder& builder) {
                builder.readTexture(ssrRawId, rhi::TextureUsageFlags::Sampled);
                builder.readTexture(depthId, rhi::TextureUsageFlags::Sampled);
                builder.writeTexture(ssrFilteredId, rhi::TextureUsageFlags::ColorAttachment);
            },
            [this](rhi::RhiCommandList& cmdList) { recordSsrTemporalFilterPass(cmdList); });
    }

    // 5. Main PBR Scene Pass
    renderGraph_.addPass(
        "MainPbrPass",
        [hasShadowTarget, shadowId, hasLightBuf, lightBufId, hasColorTarget, colorId,
         hasDepthTarget, depthId, hasDepthPrepass, hasGtao, gtaoFilteredId, hasSsgi, ssgiFilteredId,
         hasSsr, ssrFilteredId](RenderPassBuilder& builder) {
            if (hasShadowTarget) {
                builder.readTexture(shadowId, rhi::TextureUsageFlags::Sampled);
            }
            if (hasLightBuf) {
                builder.readBuffer(lightBufId, rhi::BufferUsageFlags::Uniform);
            }
            if (hasGtao && gtaoFilteredId.isValid()) {
                builder.readTexture(gtaoFilteredId, rhi::TextureUsageFlags::Sampled);
            }
            if (hasSsgi && ssgiFilteredId.isValid()) {
                builder.readTexture(ssgiFilteredId, rhi::TextureUsageFlags::Sampled);
            }
            if (hasSsr && ssrFilteredId.isValid()) {
                builder.readTexture(ssrFilteredId, rhi::TextureUsageFlags::Sampled);
            }
            if (hasColorTarget) {
                builder.writeTexture(colorId, rhi::TextureUsageFlags::ColorAttachment);
            }
            if (hasDepthTarget) {
                if (hasDepthPrepass) {
                    builder.readTexture(depthId, rhi::TextureUsageFlags::DepthStencilAttachment);
                } else {
                    builder.writeTexture(depthId, rhi::TextureUsageFlags::DepthStencilAttachment);
                }
            }
        },
        [this](rhi::RhiCommandList& cmdList) { recordMainPbrPass(cmdList); });

    // 6. Velocity / Motion Vector Pass (Milestone 11H / 11I Motion Blur)
    if (needsVelocity && hasVelocityTarget && hasDepthTarget) {
        renderGraph_.addPass(
            "VelocityPass",
            [depthId, velocityId](RenderPassBuilder& builder) {
                builder.readTexture(depthId, rhi::TextureUsageFlags::Sampled);
                builder.writeTexture(velocityId, rhi::TextureUsageFlags::ColorAttachment);
            },
            [this](rhi::RhiCommandList& cmdList) { recordVelocityPass(cmdList); });
    }

    // 7. Temporal Anti-Aliasing (TAA) Accumulation Pass (Milestone 11H)
    if (useTaa && hasColorTarget && hasPrevHistory && hasVelocityTarget && hasDepthTarget &&
        hasCurrHistory) {
        renderGraph_.addPass(
            "TaaPass",
            [colorId, prevHistoryId, velocityId, depthId,
             currHistoryId](RenderPassBuilder& builder) {
                builder.readTexture(colorId, rhi::TextureUsageFlags::Sampled);
                builder.readTexture(prevHistoryId, rhi::TextureUsageFlags::Sampled);
                builder.readTexture(velocityId, rhi::TextureUsageFlags::Sampled);
                builder.readTexture(depthId, rhi::TextureUsageFlags::Sampled);
                builder.writeTexture(currHistoryId, rhi::TextureUsageFlags::ColorAttachment);
            },
            [this](rhi::RhiCommandList& cmdList) { recordTaaPass(cmdList); });
    }

    // 8. Contrast-Adaptive Sharpening (RCAS) Pass (Milestone 11H)
    if (useTaa && hasCurrHistory && hasFinalHdr) {
        renderGraph_.addPass(
            "SharpenPass",
            [currHistoryId, finalHdrId](RenderPassBuilder& builder) {
                builder.readTexture(currHistoryId, rhi::TextureUsageFlags::Sampled);
                builder.writeTexture(finalHdrId, rhi::TextureUsageFlags::ColorAttachment);
            },
            [this](rhi::RhiCommandList& cmdList) { recordSharpenPass(cmdList); });
    }

    auto compileRes = renderGraph_.compile();
    if (!compileRes.hasValue()) {
        ENG_LOG_ERROR("SceneRenderer", "Failed to compile SceneRenderer RenderGraph");
    }
}

void SceneRenderer::recordShadowPass(rhi::RhiCommandList& cmdList) {
    const Timestamp tShadowCpuStart = Timestamp::now();
    if (currentWorld_ == nullptr || currentAssetManager_ == nullptr ||
        !shadowDepthTarget_.isValid() || !shadowPipeline_.isValid()) {
        cpuPassTimings_.shadowRecordMs = 0.0F;
        return;
    }

    if (!shadowDirty_) {
        if (currentHasTimestamps_) {
            cmdList.writeTimestamp(currentBaseQuery_ + 2, rhi::GpuTimestampStage::TopOfPipe);
            cmdList.writeTimestamp(currentBaseQuery_ + 3, rhi::GpuTimestampStage::TopOfPipe);
        }
        cpuPassTimings_.shadowRecordMs =
            static_cast<float>((Timestamp::now() - tShadowCpuStart).asMicroseconds()) * 0.001F;
        return;
    }

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 2, rhi::GpuTimestampStage::TopOfPipe);
    }

    if (staticCastersListDirty_ || lastDynamicCasterCount_ > 0) {
        if (staticCastersListDirty_) {
            staticShadowCasters_.clear();
        }
        dynamicShadowCasters_.clear();
        auto& ecs = currentWorld_->getRawWorld();
        ecs.each<const MeshComponent>([&](flecs::entity e, const MeshComponent& meshComp) {
            if (!meshComp.visible || !meshComp.castShadows) {
                lastRenderStats_.culledPrimitives++;
                return;
            }
            const bool isDynamic = (!meshComp.isStatic) ||
                                   e.has<physics::PhysicsTransformComponent>() ||
                                   e.has<physics::RigidBodyComponent>();
            if (!isDynamic && !staticCastersListDirty_) {
                return;
            }
            AssetId meshId = meshComp.meshAsset;
            if ((!meshId.isValid() || !currentAssetManager_->isLoaded(meshId)) &&
                !meshComp.assetPath.empty()) {
                meshId = currentAssetManager_->findAssetByPath(meshComp.assetPath);
            }
            if (!meshId.isValid() || !currentAssetManager_->isLoaded(meshId)) {
                lastRenderStats_.culledPrimitives++;
                return;
            }
            GpuMesh* gpuMesh = currentAssetManager_->getGpuMesh(meshId);
            if (gpuMesh == nullptr && currentDevice_ != nullptr) {
                auto uploadRes = currentAssetManager_->uploadMesh(meshId, *currentDevice_);
                if (uploadRes.hasValue()) {
                    gpuMesh = uploadRes.value();
                }
            }
            if (gpuMesh == nullptr || !gpuMesh->isValid()) {
                lastRenderStats_.culledPrimitives++;
                return;
            }

            const auto& shadowSubmeshes = gpuMesh->getSubmeshes();
            const uint32_t shadowMatIdx = (meshComp.submeshIndex < shadowSubmeshes.size())
                                              ? shadowSubmeshes[meshComp.submeshIndex].materialIndex
                                              : 0;
            GpuMaterial* shadowMat = currentAssetManager_->getGpuMaterial(meshId, shadowMatIdx);
            if (shadowMat != nullptr) {
                const auto& u = shadowMat->getUniformData();
                if (u.alphaMode == 2 || u.transmissionFactor > 0.5F) {
                    lastRenderStats_.culledPrimitives++;
                    return; // Transparent / transmissive glass does not cast opaque shadows
                }
            }

            Mat4 model = Mat4::identity();
            if (const auto* wt = e.get<world::WorldTransformComponent>()) {
                model = wt->worldMatrix;
            } else if (const auto* tc = e.get<world::TransformComponent>()) {
                model = tc->transform.toMatrix();
            } else if (const auto* ptc = e.get<physics::PhysicsTransformComponent>()) {
                model = ptc->transform.toMatrix();
            }

            const AABB localBounds = (meshComp.submeshIndex < gpuMesh->getSubmeshes().size())
                                         ? gpuMesh->getSubmeshes()[meshComp.submeshIndex].bounds
                                         : gpuMesh->getBounds();
            const AABB worldBounds = localBounds.transform(model);

            const Vec3 center = (worldBounds.minBound + worldBounds.maxBound) * 0.5F;
            const float distToLight = Vec3::dot(center, currentLightDir_);

            ShadowCasterItem item{
                .gpuMesh = gpuMesh,
                .model = model,
                .submeshIndex = meshComp.submeshIndex,
                .worldBounds = worldBounds,
                .distanceToLight = distToLight,
                .isDynamic = isDynamic,
            };

            if (isDynamic) {
                dynamicShadowCasters_.push_back(item);
            } else {
                staticShadowCasters_.push_back(item);
            }
        });
    } else {
        dynamicShadowCasters_.clear();
    }

    auto sortFrontToBack = [](const ShadowCasterItem& a, const ShadowCasterItem& b) {
        return a.distanceToLight > b.distanceToLight;
    };
    if (staticCastersListDirty_) {
        std::sort(staticShadowCasters_.begin(), staticShadowCasters_.end(), sortFrontToBack);
        staticCastersListDirty_ = false;
    }
    std::sort(dynamicShadowCasters_.begin(), dynamicShadowCasters_.end(), sortFrontToBack);

    if (staticShadowTarget_.isValid()) {
        if (staticShadowDirty_) {
            rhi::RenderPassBeginInfo staticPassInfo{
                .colorTarget = {},
                .depthTarget = staticShadowTarget_,
                .clearColor = {0.0F, 0.0F, 0.0F, 1.0F},
                .clearDepth = 1.0F,
                .clearTarget = false,
                .clearDepthTarget = true,
                .renderWidth = shadowResolution_,
                .renderHeight = shadowResolution_,
            };
            cmdList.beginRenderPass(staticPassInfo);
            cmdList.bindPipeline(shadowPipeline_);
            renderCasters(cmdList, staticShadowCasters_);
            cmdList.endRenderPass();
            staticShadowDirty_ = false;
        }

        // Copy static shadow cache to composite shadow depth target
        cmdList.copyTexture(staticShadowTarget_, shadowDepthTarget_);

        // Render dynamic casters directly on top of static depth without clearing
        if (!dynamicShadowCasters_.empty()) {
            rhi::RenderPassBeginInfo dynamicPassInfo{
                .colorTarget = {},
                .depthTarget = shadowDepthTarget_,
                .clearColor = {0.0F, 0.0F, 0.0F, 1.0F},
                .clearDepth = 1.0F,
                .clearTarget = false,
                .clearDepthTarget = false,
                .renderWidth = shadowResolution_,
                .renderHeight = shadowResolution_,
            };
            cmdList.beginRenderPass(dynamicPassInfo);
            cmdList.bindPipeline(shadowPipeline_);
            renderCasters(cmdList, dynamicShadowCasters_);
            cmdList.endRenderPass();
        }
    } else {
        // Fallback when static shadow cache target is unallocated
        rhi::RenderPassBeginInfo shadowPassInfo{
            .colorTarget = {},
            .depthTarget = shadowDepthTarget_,
            .clearColor = {0.0F, 0.0F, 0.0F, 1.0F},
            .clearDepth = 1.0F,
            .clearTarget = false,
            .clearDepthTarget = true,
            .renderWidth = shadowResolution_,
            .renderHeight = shadowResolution_,
        };
        cmdList.beginRenderPass(shadowPassInfo);
        cmdList.bindPipeline(shadowPipeline_);
        renderCasters(cmdList, staticShadowCasters_);
        renderCasters(cmdList, dynamicShadowCasters_);
        cmdList.endRenderPass();
        staticShadowDirty_ = false;
    }

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 3, rhi::GpuTimestampStage::BottomOfPipe);
    }
    shadowDirty_ = false;
    cpuPassTimings_.shadowRecordMs =
        static_cast<float>((Timestamp::now() - tShadowCpuStart).asMicroseconds()) * 0.001F;
}

void SceneRenderer::renderCasters(rhi::RhiCommandList& cmdList,
                                  const std::vector<ShadowCasterItem>& casters) {
    if (casters.empty() || !shadowPipeline_.isValid()) {
        return;
    }
    const bool applyLightCulling = settings_.performance.frustumCulling;
    const size_t numCascades = shadowCascades_.empty() ? 1 : shadowCascades_.size();

    for (size_t cIdx = 0; cIdx < numCascades; ++cIdx) {
        float vpX = 0.0F;
        float vpY = 0.0F;
        float vpW = static_cast<float>(shadowResolution_);
        float vpH = static_cast<float>(shadowResolution_);
        Mat4 cascadeVp = currentLightSpaceMatrix_;
        Frustum lightFrustum = Frustum::fromViewProjection(cascadeVp);

        if (!shadowCascades_.empty()) {
            const auto& cascade = shadowCascades_[cIdx];
            vpX = cascade.atlasTileUV.x * static_cast<float>(shadowResolution_);
            vpY = cascade.atlasTileUV.y * static_cast<float>(shadowResolution_);
            vpW = cascade.atlasTileUV.z * static_cast<float>(shadowResolution_);
            vpH = cascade.atlasTileUV.w * static_cast<float>(shadowResolution_);
            cascadeVp = cascade.viewProjMatrix;
            lightFrustum = cascade.lightFrustum;
        }

        // In far cascades (cIdx >= 1), skip sub-texel shadow casters whose world AABB
        // diagonal is smaller than 2 shadow texels, avoiding thousands of invisible draw calls.
        float minCasterSizeSq = 0.0F;
        if (cIdx >= 1 && vpW > 0.0F) {
            const float scaleX =
                std::sqrt(cascadeVp.m[0] * cascadeVp.m[0] + cascadeVp.m[1] * cascadeVp.m[1] +
                          cascadeVp.m[2] * cascadeVp.m[2]);
            if (scaleX > 1e-6F) {
                const float worldSpan = 2.0F / scaleX;
                const float texelWorldSize = worldSpan / vpW;
                const float minDiag = texelWorldSize * 2.0F;
                minCasterSizeSq = minDiag * minDiag;
            }
        }

        cmdList.setViewport(vpX, vpY, vpW, vpH);
        cmdList.setScissor(static_cast<int32_t>(vpX), static_cast<int32_t>(vpY),
                           static_cast<uint32_t>(vpW), static_cast<uint32_t>(vpH));

        rhi::BufferHandle lastBoundVb{};
        rhi::BufferHandle lastBoundIb{};
        bool hasPushedMvp = false;
        Mat4 lastMvp{};

        for (const auto& caster : casters) {
            if (minCasterSizeSq > 0.0F) {
                const Vec3 ext = caster.worldBounds.maxBound - caster.worldBounds.minBound;
                if (ext.lengthSq() < minCasterSizeSq) {
                    lastRenderStats_.culledPrimitives++;
                    continue;
                }
            }
            if (applyLightCulling) {
                if (!lightFrustum.intersectsAABB(caster.worldBounds)) {
                    lastRenderStats_.culledPrimitives++;
                    continue;
                }
            }

            if (shadowPipelineLayout_.isValid()) {
                const Mat4 mvp = cascadeVp * caster.model;
                if (!hasPushedMvp || std::memcmp(lastMvp.m, mvp.m, sizeof(mvp.m)) != 0) {
                    alignas(16) float pushData[16];
                    std::memcpy(pushData, mvp.m, sizeof(pushData));
                    cmdList.pushConstants(shadowPipelineLayout_, rhi::ShaderStageFlags::Vertex, 0,
                                          sizeof(pushData), pushData);
                    lastMvp = mvp;
                    hasPushedMvp = true;
                }
            }

            if (lastBoundVb != caster.gpuMesh->getVertexBuffer()) {
                cmdList.bindVertexBuffer(0, caster.gpuMesh->getVertexBuffer(), 0);
                lastBoundVb = caster.gpuMesh->getVertexBuffer();
            }
            if (caster.gpuMesh->getIndexBuffer().isValid() && caster.gpuMesh->getIndexCount() > 0) {
                if (lastBoundIb != caster.gpuMesh->getIndexBuffer()) {
                    cmdList.bindIndexBuffer(caster.gpuMesh->getIndexBuffer(), 0, true);
                    lastBoundIb = caster.gpuMesh->getIndexBuffer();
                }
                const auto& submeshes = caster.gpuMesh->getSubmeshes();
                if (caster.submeshIndex < submeshes.size()) {
                    const auto& sub = submeshes[caster.submeshIndex];
                    cmdList.drawIndexed(sub.indexCount, 1, sub.firstIndex, sub.vertexOffset, 0);
                    lastRenderStats_.shadowDrawCalls++;
                    lastRenderStats_.shadowTriangles += sub.indexCount / 3;
                } else if (submeshes.empty()) {
                    cmdList.drawIndexed(caster.gpuMesh->getIndexCount(), 1, 0, 0, 0);
                    lastRenderStats_.shadowDrawCalls++;
                    lastRenderStats_.shadowTriangles += caster.gpuMesh->getIndexCount() / 3;
                } else {
                    for (const auto& sub : submeshes) {
                        cmdList.drawIndexed(sub.indexCount, 1, sub.firstIndex, sub.vertexOffset, 0);
                        lastRenderStats_.shadowDrawCalls++;
                        lastRenderStats_.shadowTriangles += sub.indexCount / 3;
                    }
                }
            } else if (caster.gpuMesh->getVertexCount() > 0) {
                cmdList.draw(caster.gpuMesh->getVertexCount(), 1, 0, 0);
                lastRenderStats_.shadowDrawCalls++;
                lastRenderStats_.shadowTriangles += caster.gpuMesh->getVertexCount() / 3;
            }
        }
    }
}

void SceneRenderer::recordDepthPrepass(rhi::RhiCommandList& cmdList) {
    const Timestamp tPrepassCpuStart = Timestamp::now();
    if (currentWorld_ == nullptr || currentAssetManager_ == nullptr ||
        !depthPrepassPipeline_.isValid() || !depthTarget_.isValid()) {
        cpuPassTimings_.depthPrepassRecordMs = 0.0F;
        return;
    }

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 12, rhi::GpuTimestampStage::TopOfPipe);
    }

    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = {},
        .depthTarget = depthTarget_,
        .clearColor = {0.0F, 0.0F, 0.0F, 1.0F},
        .clearDepth = 1.0F,
        .clearTarget = false,
        .clearDepthTarget = true,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_));
    cmdList.setScissor(0, 0, width_, height_);
    cmdList.bindPipeline(depthPrepassPipeline_);

    const bool applyCameraCulling =
        settings_.performance.frustumCulling && !currentViewProj_.isNearlyEqual(Mat4::identity());
    const Frustum cameraFrustum = Frustum::fromViewProjection(currentViewProj_);

    collectVisibleMeshes();

    rhi::BufferHandle lastBoundVb{};
    rhi::BufferHandle lastBoundIb{};
    bool hasPushedModel = false;
    Mat4 lastModel{};

    for (const auto& item : visibleItems_) {
        if (item.gpuMesh == nullptr || !item.gpuMesh->isValid()) {
            continue;
        }
        if (item.isAlphaMasked || item.isTransparent) {
            continue; // Skip alpha masked and transparent geometry in depth prepass
        }

        if (shadowPipelineLayout_.isValid()) {
            if (!hasPushedModel ||
                std::memcmp(lastModel.m, item.model.m, sizeof(lastModel.m)) != 0) {
                alignas(16) float pushData[16];
                const Mat4 mvp = currentViewProj_ * item.model;
                std::memcpy(pushData, mvp.m, sizeof(pushData));
                cmdList.pushConstants(shadowPipelineLayout_, rhi::ShaderStageFlags::Vertex, 0,
                                      sizeof(pushData), pushData);
                lastModel = item.model;
                hasPushedModel = true;
            }
        }

        if (lastBoundVb != item.gpuMesh->getVertexBuffer()) {
            cmdList.bindVertexBuffer(0, item.gpuMesh->getVertexBuffer(), 0);
            lastBoundVb = item.gpuMesh->getVertexBuffer();
        }
        if (item.gpuMesh->getIndexBuffer().isValid() && item.gpuMesh->getIndexCount() > 0) {
            if (lastBoundIb != item.gpuMesh->getIndexBuffer()) {
                cmdList.bindIndexBuffer(item.gpuMesh->getIndexBuffer(), 0, true);
                lastBoundIb = item.gpuMesh->getIndexBuffer();
            }
            const auto& submeshes = item.gpuMesh->getSubmeshes();
            if (item.submeshIndex < submeshes.size()) {
                const auto& sub = submeshes[item.submeshIndex];
                cmdList.drawIndexed(sub.indexCount, 1, sub.firstIndex, sub.vertexOffset, 0);
                lastRenderStats_.depthPrepassDrawCalls++;
            } else {
                cmdList.drawIndexed(item.gpuMesh->getIndexCount(), 1, 0, 0, 0);
                lastRenderStats_.depthPrepassDrawCalls++;
            }
        } else if (item.gpuMesh->getVertexCount() > 0) {
            cmdList.draw(item.gpuMesh->getVertexCount(), 1, 0, 0);
            lastRenderStats_.depthPrepassDrawCalls++;
        }
    }
    (void)applyCameraCulling;
    (void)cameraFrustum;

    cmdList.endRenderPass();
    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 13, rhi::GpuTimestampStage::BottomOfPipe);
    }
    cpuPassTimings_.depthPrepassRecordMs =
        static_cast<float>((Timestamp::now() - tPrepassCpuStart).asMicroseconds()) * 0.001F;
}

void SceneRenderer::prewarmSceneAssets(world::World& world, AssetManager& assetManager,
                                       rhi::RhiDevice& device) {
    auto& ecs = world.getRawWorld();
    ecs.each<const MeshComponent>([&](flecs::entity /*e*/, const MeshComponent& meshComp) {
        if (!meshComp.visible) {
            return;
        }
        AssetId meshId = meshComp.meshAsset;
        if ((!meshId.isValid() || !assetManager.isLoaded(meshId)) && !meshComp.assetPath.empty()) {
            meshId = assetManager.findAssetByPath(meshComp.assetPath);
            if (!meshId.isValid() || !assetManager.isLoaded(meshId)) {
                auto loadRes = assetManager.loadMesh(meshComp.assetPath);
                if (loadRes.hasValue()) {
                    meshId = loadRes.value();
                }
            }
        }
        if (!meshId.isValid() || !assetManager.isLoaded(meshId)) {
            return;
        }
        GpuMesh* gpuMesh = assetManager.getGpuMesh(meshId);
        if (gpuMesh == nullptr) {
            auto uploadRes = assetManager.uploadMesh(meshId, device);
            if (uploadRes.hasValue()) {
                gpuMesh = uploadRes.value();
            }
        }
        if (gpuMesh == nullptr || !gpuMesh->isValid()) {
            return;
        }
        if (pipelineLayout_.isValid() && materialDescriptorSetLayout_.isValid()) {
            const auto& submeshes = gpuMesh->getSubmeshes();
            if (meshComp.submeshIndex < submeshes.size()) {
                const uint32_t matIdx = submeshes[meshComp.submeshIndex].materialIndex;
                if (assetManager.getGpuMaterial(meshId, matIdx) == nullptr) {
                    (void)assetManager.uploadMaterial(meshId, matIdx, device,
                                                      materialDescriptorSetLayout_);
                }
            } else if (submeshes.empty()) {
                if (assetManager.getGpuMaterial(meshId, 0) == nullptr) {
                    (void)assetManager.uploadMaterial(meshId, 0, device,
                                                      materialDescriptorSetLayout_);
                }
            } else {
                for (const auto& sub : submeshes) {
                    if (assetManager.getGpuMaterial(meshId, sub.materialIndex) == nullptr) {
                        (void)assetManager.uploadMaterial(meshId, sub.materialIndex, device,
                                                          materialDescriptorSetLayout_);
                    }
                }
            }
        }
    });
}

void SceneRenderer::collectVisibleMeshes() {
    if (visibleItemsCollected_ || currentWorld_ == nullptr || currentAssetManager_ == nullptr ||
        currentDevice_ == nullptr) {
        return;
    }
    visibleItemsCollected_ = true;

    const bool applyCameraCulling =
        settings_.performance.frustumCulling && !currentViewProj_.isNearlyEqual(Mat4::identity());
    const Frustum cameraFrustum = Frustum::fromViewProjection(currentViewProj_);

    auto& ecs = currentWorld_->getRawWorld();
    const uint64_t currentTransformVersion = currentWorld_->getTransformVersion();
    const uint64_t currentWorldInstanceId = currentWorld_->getInstanceId();
    const uint64_t currentWorldGen = currentWorld_->getSceneGeneration();
    const int32_t currentMeshCount = ecs.count<MeshComponent>();

    // Fast contiguous scan over MeshComponent to detect visibility/meshAsset/submesh edits
    // without touching transforms, AssetManager maps, or AABB transforms.
    uint64_t currentStateHash = 14695981039346656037ULL;
    ecs.each<const MeshComponent>([&](flecs::entity e, const MeshComponent& mc) {
        uint64_t h = static_cast<uint64_t>(e.id());
        h ^= (static_cast<uint64_t>(mc.visible ? 1U : 0U) << 56);
        h ^= mc.meshAsset.value() * 11400714819323198485ULL;
        h ^= static_cast<uint64_t>(mc.submeshIndex) * 1099511628211ULL;
        currentStateHash ^= h * 16777619ULL;
    });

    const bool cacheHit = cachedSceneValid_ && cachedSceneWorld_ == currentWorld_ &&
                          cachedSceneWorldInstanceId_ == currentWorldInstanceId &&
                          cachedSceneWorldGen_ == currentWorldGen &&
                          cachedSceneAssetManager_ == currentAssetManager_ &&
                          cachedSceneTransformVersion_ == currentTransformVersion &&
                          cachedSceneMeshCount_ == currentMeshCount &&
                          cachedSceneStateHash_ == currentStateHash &&
                          prevEntityTransforms_.empty();

    visibleItems_.clear();

    if (!cacheHit) {
        cachedSceneItems_.clear();
        bool allAssetsReady = true;
        uint32_t resolvedMeshes = 0;

        ecs.each<const MeshComponent>([&](flecs::entity e, const MeshComponent& meshComp) {
            if (!meshComp.visible) {
                return;
            }

            AssetId meshId = meshComp.meshAsset;
            if ((!meshId.isValid() || !currentAssetManager_->isLoaded(meshId)) &&
                !meshComp.assetPath.empty()) {
                meshId = currentAssetManager_->findAssetByPath(meshComp.assetPath);
                if (!meshId.isValid() || !currentAssetManager_->isLoaded(meshId)) {
                    auto loadRes = currentAssetManager_->loadMesh(meshComp.assetPath);
                    if (loadRes.hasValue()) {
                        meshId = loadRes.value();
                    }
                }
            }

            if (!meshId.isValid() || !currentAssetManager_->isLoaded(meshId)) {
                allAssetsReady = false;
                return;
            }

            GpuMesh* gpuMesh = currentAssetManager_->getGpuMesh(meshId);
            if (gpuMesh == nullptr) {
                auto uploadRes = currentAssetManager_->uploadMesh(meshId, *currentDevice_);
                if (uploadRes.hasValue()) {
                    gpuMesh = uploadRes.value();
                }
            }

            if (gpuMesh == nullptr || !gpuMesh->isValid()) {
                allAssetsReady = false;
                return;
            }

            Mat4 model = Mat4::identity();
            if (const auto* wt = e.get<world::WorldTransformComponent>()) {
                model = wt->worldMatrix;
            } else if (const auto* tc = e.get<world::TransformComponent>()) {
                model = tc->transform.toMatrix();
            } else if (const auto* ptc = e.get<physics::PhysicsTransformComponent>()) {
                model = ptc->transform.toMatrix();
            }

            const AABB localBounds = (meshComp.submeshIndex < gpuMesh->getSubmeshes().size())
                                         ? gpuMesh->getSubmeshes()[meshComp.submeshIndex].bounds
                                         : gpuMesh->getBounds();
            const AABB worldBounds = localBounds.transform(model);

            auto resolveMaterial =
                [&](uint32_t matIndex) -> std::tuple<rhi::DescriptorSetHandle, bool, bool, bool> {
                if (pipelineLayout_.isValid() && materialDescriptorSetLayout_.isValid()) {
                    GpuMaterial* mat = currentAssetManager_->getGpuMaterial(meshId, matIndex);
                    if (mat == nullptr) {
                        auto upMat = currentAssetManager_->uploadMaterial(
                            meshId, matIndex, *currentDevice_, materialDescriptorSetLayout_);
                        if (upMat.hasValue()) {
                            mat = upMat.value();
                        } else {
                            auto defMat = currentAssetManager_->getOrCreateDefaultMaterial(
                                *currentDevice_, materialDescriptorSetLayout_);
                            if (defMat.hasValue()) {
                                mat = defMat.value();
                            }
                        }
                    }
                    if (mat != nullptr && mat->isValid()) {
                        const auto& u = mat->getUniformData();
                        const bool isMasked = (u.alphaMode == 1);
                        const bool isDoubleSided = (u.doubleSided != 0);
                        const bool isTransparent =
                            (u.alphaMode == 2) || (u.transmissionFactor > 0.0F);
                        return {mat->getDescriptorSet(), isMasked, isDoubleSided, isTransparent};
                    }
                }
                allAssetsReady = false;
                return {{}, false, false, false};
            };

            Mat4 prevModel = model;
            bool hasValidPrev = false;
            if (!prevEntityTransforms_.empty()) {
                const uint64_t entityIdVal = static_cast<uint64_t>(e.id());
                for (const auto& cache : prevEntityTransforms_) {
                    if (cache.entityId == entityIdVal) {
                        prevModel = cache.model;
                        hasValidPrev = true;
                        break;
                    }
                }
            }

            const auto& submeshes = gpuMesh->getSubmeshes();
            if (meshComp.submeshIndex < submeshes.size()) {
                const auto [matSet, isMasked, isDoubleSided, isTransparent] =
                    resolveMaterial(submeshes[meshComp.submeshIndex].materialIndex);
                cachedSceneItems_.push_back({
                    .item =
                        {
                            .gpuMesh = gpuMesh,
                            .materialSet = matSet,
                            .model = model,
                            .prevModel = prevModel,
                            .submeshIndex = meshComp.submeshIndex,
                            .depthSq = 0.0F,
                            .isAlphaMasked = isMasked,
                            .isDoubleSided = isDoubleSided,
                            .isTransparent = isTransparent,
                            .hasValidPrevTransform = hasValidPrev && isTaaHistoryValid_,
                        },
                    .worldBounds = worldBounds,
                });
            } else if (submeshes.empty()) {
                const auto [matSet, isMasked, isDoubleSided, isTransparent] = resolveMaterial(0);
                cachedSceneItems_.push_back({
                    .item =
                        {
                            .gpuMesh = gpuMesh,
                            .materialSet = matSet,
                            .model = model,
                            .prevModel = prevModel,
                            .submeshIndex = 0,
                            .depthSq = 0.0F,
                            .isAlphaMasked = isMasked,
                            .isDoubleSided = isDoubleSided,
                            .isTransparent = isTransparent,
                            .hasValidPrevTransform = hasValidPrev && isTaaHistoryValid_,
                        },
                    .worldBounds = worldBounds,
                });
            } else {
                for (uint32_t sIdx = 0; sIdx < submeshes.size(); ++sIdx) {
                    const auto [matSet, isMasked, isDoubleSided, isTransparent] =
                        resolveMaterial(submeshes[sIdx].materialIndex);
                    cachedSceneItems_.push_back({
                        .item =
                            {
                                .gpuMesh = gpuMesh,
                                .materialSet = matSet,
                                .model = model,
                                .prevModel = prevModel,
                                .submeshIndex = sIdx,
                                .depthSq = 0.0F,
                                .isAlphaMasked = isMasked,
                                .isDoubleSided = isDoubleSided,
                                .isTransparent = isTransparent,
                                .hasValidPrevTransform = hasValidPrev && isTaaHistoryValid_,
                            },
                        .worldBounds = worldBounds,
                        .isFirstSubmeshOfEntity = (sIdx == 0),
                    });
                }
            }
            ++resolvedMeshes;
        });

        cachedSceneWorld_ = currentWorld_;
        cachedSceneWorldInstanceId_ = currentWorldInstanceId;
        cachedSceneWorldGen_ = currentWorldGen;
        cachedSceneAssetManager_ = currentAssetManager_;
        cachedSceneTransformVersion_ = currentTransformVersion;
        cachedSceneMeshCount_ = currentMeshCount;
        cachedSceneStateHash_ = currentStateHash;
        cachedSceneValid_ = allAssetsReady && prevEntityTransforms_.empty() && resolvedMeshes > 0;
    }

    for (const auto& cached : cachedSceneItems_) {
        if (applyCameraCulling) {
            if (!cameraFrustum.intersectsAABB(cached.worldBounds)) {
                if (cached.isFirstSubmeshOfEntity) {
                    lastRenderStats_.culledPrimitives++;
                }
                continue;
            }
        }

        const float minX = std::min(cached.worldBounds.minBound.x, cached.worldBounds.maxBound.x);
        const float maxX = std::max(cached.worldBounds.minBound.x, cached.worldBounds.maxBound.x);
        const float minY = std::min(cached.worldBounds.minBound.y, cached.worldBounds.maxBound.y);
        const float maxY = std::max(cached.worldBounds.minBound.y, cached.worldBounds.maxBound.y);
        const float minZ = std::min(cached.worldBounds.minBound.z, cached.worldBounds.maxBound.z);
        const float maxZ = std::max(cached.worldBounds.minBound.z, cached.worldBounds.maxBound.z);

        const Vec3 closestPoint{std::clamp(currentCameraPos_.x, minX, maxX),
                                std::clamp(currentCameraPos_.y, minY, maxY),
                                std::clamp(currentCameraPos_.z, minZ, maxZ)};
        VisibleMeshItem item = cached.item;
        item.depthSq = (closestPoint - currentCameraPos_).lengthSq();
        visibleItems_.push_back(item);
        if (cached.isFirstSubmeshOfEntity) {
            lastRenderStats_.meshesRendered++;
        }
    }

    // Sort visible submeshes:
    // 1. Opaque & Alpha-masked first (front-to-back for Early-Z rejection)
    // 2. Transparent & Transmissive last (back-to-front for proper alpha blending)
    std::sort(visibleItems_.begin(), visibleItems_.end(),
              [](const VisibleMeshItem& a, const VisibleMeshItem& b) {
                  if (a.isTransparent != b.isTransparent) {
                      return !a.isTransparent; // Opaque / Masked first, Transparent last
                  }
                  if (a.isTransparent) {
                      return a.depthSq > b.depthSq; // Transparent back-to-front
                  }
                  if (a.isAlphaMasked != b.isAlphaMasked) {
                      return !a.isAlphaMasked; // Opaque first, Masked second
                  }
                  return a.depthSq < b.depthSq; // Front-to-back for Early-Z
              });
}

void SceneRenderer::recordMainPbrPass(rhi::RhiCommandList& cmdList) {
    const Timestamp tMainCpuStart = Timestamp::now();
    if (currentWorld_ == nullptr || currentAssetManager_ == nullptr) {
        cpuPassTimings_.mainPbrRecordMs = 0.0F;
        return;
    }
    const bool managePass = colorTarget_.isValid();

    const bool useTaa = isTaaEnabled() && hdrRawTarget_.isValid();
    const bool useHdr = (postProcessSettings_.enabled && hdrTarget_.isValid()) || useTaa;
    const bool useDepthPrepass =
        (settings_.performance.depthPrepass || settings_.lighting.gtaoEnabled ||
         settings_.lighting.ssgiEnabled) &&
        depthPrepassPipeline_.isValid();
    const rhi::TextureHandle sceneColorTarget =
        useTaa ? hdrRawTarget_ : (useHdr ? hdrTarget_ : colorTarget_);
    rhi::PipelineHandle scenePipeline =
        (useHdr && hdrPipeline_.isValid()) ? hdrPipeline_ : pipeline_;

    if (useDepthPrepass) {
        if (useHdr && hdrPipelineDepthPrepass_.isValid()) {
            scenePipeline = hdrPipelineDepthPrepass_;
        } else if (!useHdr && pipelineDepthPrepass_.isValid()) {
            scenePipeline = pipelineDepthPrepass_;
        }
    }

    if (settings_.debug.viewMode == DebugViewMode::Wireframe) {
        if (useHdr && wireframePipeline_.isValid()) {
            scenePipeline = wireframePipeline_;
        } else if (!useHdr && wireframeLdrPipeline_.isValid()) {
            scenePipeline = wireframeLdrPipeline_;
        } else if (wireframePipeline_.isValid()) {
            scenePipeline = wireframePipeline_;
        }
    }

    if (managePass) {
        if (currentHasTimestamps_) {
            cmdList.writeTimestamp(currentBaseQuery_ + 4, rhi::GpuTimestampStage::TopOfPipe);
        }
        rhi::RenderPassBeginInfo passInfo{
            .colorTarget = sceneColorTarget,
            .depthTarget = depthTarget_,
            .clearColor = {0.08F, 0.12F, 0.20F, 1.0F},
            .clearDepth = 1.0F,
            .clearTarget = true,
            .clearDepthTarget = !useDepthPrepass,
        };
        cmdList.beginRenderPass(passInfo);
        cmdList.setViewport(0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_));
        cmdList.setScissor(0, 0, width_, height_);
    }

    if (scenePipeline.isValid()) {
        cmdList.bindPipeline(scenePipeline);
    }

    // Bind Set 1: Scene Lighting & Shadow Map once for the main pass
    if (pipelineLayout_.isValid() && sceneDescriptorSet_.isValid()) {
        const std::array<rhi::DescriptorSetHandle, 1> sceneSets = {sceneDescriptorSet_};
        cmdList.bindDescriptorSets(pipelineLayout_, 1, sceneSets);
    }

    collectVisibleMeshes();

    rhi::PipelineHandle lastBoundPipeline = scenePipeline;
    rhi::BufferHandle lastBoundVb{};
    rhi::BufferHandle lastBoundIb{};
    rhi::DescriptorSetHandle lastBoundMatSet{};
    bool hasPushedModel = false;
    Mat4 lastModel{};

    const bool isWireframe = (settings_.debug.viewMode == DebugViewMode::Wireframe);

    for (const auto& item : visibleItems_) {
        // Bind pipeline based on opaque vs alpha-masked vs transparent
        rhi::PipelineHandle targetPipeline = scenePipeline;
        if (!isWireframe) {
            if (item.isTransparent) {
                if (item.isDoubleSided) {
                    targetPipeline = useHdr ? hdrPipelineTransparentDoubleSided_
                                            : pipelineTransparentDoubleSided_;
                } else {
                    targetPipeline = useHdr ? hdrPipelineTransparent_ : pipelineTransparent_;
                }
            } else if (item.isDoubleSided) {
                targetPipeline = useHdr ? hdrPipelineDoubleSided_ : pipelineDoubleSided_;
            } else if (useDepthPrepass) {
                targetPipeline =
                    item.isAlphaMasked
                        ? (useHdr ? hdrPipelineDepthPrepass_ : pipelineDepthPrepass_)
                        : (useHdr ? hdrPipelineDepthPrepassOpaque_ : pipelineDepthPrepassOpaque_);
            } else {
                targetPipeline = item.isAlphaMasked
                                     ? (useHdr ? hdrPipeline_ : pipeline_)
                                     : (useHdr ? hdrPipelineOpaque_ : pipelineOpaque_);
            }
        }
        if (targetPipeline.isValid() && targetPipeline != lastBoundPipeline) {
            cmdList.bindPipeline(targetPipeline);
            lastBoundPipeline = targetPipeline;
        }

        // Pack 128-byte push constants (64B mvp + 64B model)
        if (pipelineLayout_.isValid()) {
            if (!hasPushedModel ||
                std::memcmp(lastModel.m, item.model.m, sizeof(lastModel.m)) != 0) {
                const Mat4 mvp = currentViewProj_ * item.model;
                alignas(16) float pushData[32];
                std::memcpy(&pushData[0], mvp.m, 64);
                std::memcpy(&pushData[16], item.model.m, 64);
                cmdList.pushConstants(pipelineLayout_, rhi::ShaderStageFlags::Vertex, 0,
                                      sizeof(pushData), pushData);
                lastModel = item.model;
                hasPushedModel = true;
            }
        }

        // Bind vertex buffer
        if (lastBoundVb != item.gpuMesh->getVertexBuffer()) {
            cmdList.bindVertexBuffer(0, item.gpuMesh->getVertexBuffer(), 0);
            lastBoundVb = item.gpuMesh->getVertexBuffer();
        }

        // Bind material descriptor set
        if (item.materialSet.isValid() && lastBoundMatSet != item.materialSet) {
            const std::array<rhi::DescriptorSetHandle, 1> sets = {item.materialSet};
            cmdList.bindDescriptorSets(pipelineLayout_, 0, sets);
            lastBoundMatSet = item.materialSet;
        }

        // Bind index buffer if present
        if (item.gpuMesh->getIndexBuffer().isValid() && item.gpuMesh->getIndexCount() > 0) {
            if (lastBoundIb != item.gpuMesh->getIndexBuffer()) {
                cmdList.bindIndexBuffer(item.gpuMesh->getIndexBuffer(), 0, true);
                lastBoundIb = item.gpuMesh->getIndexBuffer();
            }

            const auto& submeshes = item.gpuMesh->getSubmeshes();
            if (item.submeshIndex < submeshes.size()) {
                const auto& sub = submeshes[item.submeshIndex];
                cmdList.drawIndexed(sub.indexCount, 1, sub.firstIndex, sub.vertexOffset, 0);
                lastRenderStats_.drawCalls++;
                lastRenderStats_.triangles += sub.indexCount / 3;
                lastRenderStats_.vertices += sub.indexCount;
            } else {
                cmdList.drawIndexed(item.gpuMesh->getIndexCount(), 1, 0, 0, 0);
                lastRenderStats_.drawCalls++;
                lastRenderStats_.triangles += item.gpuMesh->getIndexCount() / 3;
                lastRenderStats_.vertices += item.gpuMesh->getVertexCount();
            }
        } else if (item.gpuMesh->getVertexCount() > 0) {
            cmdList.draw(item.gpuMesh->getVertexCount(), 1, 0, 0);
            lastRenderStats_.drawCalls++;
            lastRenderStats_.triangles += item.gpuMesh->getVertexCount() / 3;
            lastRenderStats_.vertices += item.gpuMesh->getVertexCount();
        }
    }

    if (managePass) {
        cmdList.endRenderPass();
        if (currentHasTimestamps_) {
            cmdList.writeTimestamp(currentBaseQuery_ + 5, rhi::GpuTimestampStage::BottomOfPipe);
            if (!postProcessSettings_.enabled) {
                cmdList.writeTimestamp(currentBaseQuery_ + 1, rhi::GpuTimestampStage::BottomOfPipe);
            }
        }
    }
    cpuPassTimings_.mainPbrRecordMs =
        static_cast<float>((Timestamp::now() - tMainCpuStart).asMicroseconds()) * 0.001F;
}

RenderStats SceneRenderer::renderScene(world::World& world, AssetManager& assetManager,
                                       rhi::RhiDevice& device, rhi::RhiCommandList& cmdList,
                                       const Mat4& viewProjection) {
    const Timestamp tRenderSceneStart = Timestamp::now();
    cpuPassTimings_ = CpuPassTimings{};
    visibleItemsCollected_ = false;
    auto& ecs = world.getRawWorld();

    const uint64_t currentSceneGen = world.getSceneGeneration();
    const int32_t currentPrewarmMeshCount = static_cast<int32_t>(ecs.count<MeshComponent>());
    if (&world != lastWorld_ || currentSceneGen != lastPrewarmSceneGen_ ||
        currentPrewarmMeshCount != lastPrewarmMeshCount_) {
        prewarmSceneAssets(world, assetManager, device);
        lastPrewarmSceneGen_ = currentSceneGen;
        lastPrewarmMeshCount_ = currentPrewarmMeshCount;
    }

    currentHasTimestamps_ = device.getCaps().features.timestampQueries;
    currentBaseQuery_ = (frameSlotIndex_ % 2) * 32;
    if (currentHasTimestamps_) {
        cmdList.resetQueryPool(currentBaseQuery_, 32);
        cmdList.writeTimestamp(currentBaseQuery_ + 0, rhi::GpuTimestampStage::TopOfPipe);
    }

    // 1. Gather Light Sources & Build GpuLightingData
    GpuLightingData lightingData{};
    lightingData.cameraPosAndAmbientIntensity = Vec4(0.0F, 0.0F, 5.0F, 0.15F);
    lightingData.ambientColorAndShadowBias = Vec4(1.0F, 1.0F, 1.0F, 0.005F);

    // Camera extraction from viewProjection
    auto invVPOpt = viewProjection.inverse();
    if (invVPOpt.has_value()) {
        const auto& ivp = invVPOpt.value();
        float x = -ivp.m[8];
        float y = -ivp.m[9];
        float z = -ivp.m[10];
        float w = -ivp.m[11];
        if (std::abs(w) > 1e-6F) {
            lightingData.cameraPosAndAmbientIntensity.x = x / w;
            lightingData.cameraPosAndAmbientIntensity.y = y / w;
            lightingData.cameraPosAndAmbientIntensity.z = z / w;
        }
    }

    bool hasDirLight = false;
    if (settings_.lighting.directLighting) {
        auto dirQuery = ecs.query<const DirectionalLightComponent>();
        dirQuery.each([&](flecs::entity e, const DirectionalLightComponent& dirComp) {
            static_cast<void>(e);
            if (!dirComp.enabled || hasDirLight) {
                return;
            }
            hasDirLight = true;
            Vec3 dir = dirComp.direction;
            if (dir.lengthSq() > 1e-6F) {
                dir = dir.normalized();
            } else {
                dir = Vec3(0.5F, 0.8F, 0.5F).normalized();
            }
            lightingData.dirLight.directionAndCastShadow =
                Vec4(dir.x, dir.y, dir.z, dirComp.castShadows ? 1.0F : 0.0F);
            lightingData.dirLight.colorAndIntensity =
                Vec4(dirComp.color.x, dirComp.color.y, dirComp.color.z, dirComp.intensity);
            lightingData.ambientColorAndShadowBias.w = dirComp.shadowBias;
            lightingData.dirLightActive = 1;
        });

        if (!hasDirLight) {
            Vec3 dir = Vec3(0.5F, 0.8F, 0.5F).normalized();
            lightingData.dirLight.directionAndCastShadow = Vec4(dir.x, dir.y, dir.z, 0.0F);
            lightingData.dirLight.colorAndIntensity = Vec4(1.0F, 1.0F, 1.0F, 2.5F);
            lightingData.dirLightActive = 1;
        }
    } else {
        lightingData.dirLightActive = 0;
        lightingData.dirLight.directionAndCastShadow = Vec4(0.0F, 1.0F, 0.0F, 0.0F);
        lightingData.dirLight.colorAndIntensity = Vec4(0.0F, 0.0F, 0.0F, 0.0F);
    }

    if (!settings_.shadows.enabled) {
        lightingData.dirLight.directionAndCastShadow.w = 0.0F;
    } else {
        lightingData.ambientColorAndShadowBias.w =
            lightingData.ambientColorAndShadowBias.w * (settings_.shadows.depthBias / 0.002F);
    }

    // Point lights
    uint32_t pointCount = 0;
    auto pointQuery = ecs.query<const PointLightComponent>();
    pointQuery.each([&](flecs::entity e, const PointLightComponent& pComp) {
        if (!pComp.enabled || pointCount >= kMaxPointLights) {
            return;
        }
        Vec3 pos = pComp.position;
        if (e.has<world::WorldTransformComponent>()) {
            const auto* wt = e.get<world::WorldTransformComponent>();
            if (wt != nullptr) {
                pos = Vec3(wt->worldMatrix.m[12], wt->worldMatrix.m[13], wt->worldMatrix.m[14]);
            }
        } else if (e.has<world::TransformComponent>()) {
            const auto* tc = e.get<world::TransformComponent>();
            if (tc != nullptr) {
                pos = tc->transform.position;
            }
        }
        lightingData.pointLights[pointCount].positionAndRange =
            Vec4(pos.x, pos.y, pos.z, std::max(0.1F, pComp.range));
        lightingData.pointLights[pointCount].colorAndIntensity =
            Vec4(pComp.color.x, pComp.color.y, pComp.color.z, pComp.intensity);
        ++pointCount;
    });
    lightingData.numPointLights = pointCount;

    // Spot lights
    uint32_t spotCount = 0;
    auto spotQuery = ecs.query<const SpotLightComponent>();
    spotQuery.each([&](flecs::entity e, const SpotLightComponent& sComp) {
        if (!sComp.enabled || spotCount >= kMaxSpotLights) {
            return;
        }
        Vec3 pos = sComp.position;
        Vec3 dir = sComp.direction;
        if (e.has<world::WorldTransformComponent>()) {
            const auto* wt = e.get<world::WorldTransformComponent>();
            if (wt != nullptr) {
                pos = Vec3(wt->worldMatrix.m[12], wt->worldMatrix.m[13], wt->worldMatrix.m[14]);
                dir = wt->worldMatrix.transformVector(dir).normalized();
            }
        } else if (e.has<world::TransformComponent>()) {
            const auto* tc = e.get<world::TransformComponent>();
            if (tc != nullptr) {
                pos = tc->transform.position;
                dir = tc->transform.rotation.rotate(dir).normalized();
            }
        }
        if (dir.lengthSq() > 1e-6F) {
            dir = dir.normalized();
        } else {
            dir = Vec3(0.0F, -1.0F, 0.0F);
        }
        lightingData.spotLights[spotCount].positionAndRange =
            Vec4(pos.x, pos.y, pos.z, std::max(0.1F, sComp.range));
        lightingData.spotLights[spotCount].directionAndIntensity =
            Vec4(dir.x, dir.y, dir.z, sComp.intensity);
        lightingData.spotLights[spotCount].colorAndCastShadow =
            Vec4(sComp.color.x, sComp.color.y, sComp.color.z, sComp.castShadows ? 1.0F : 0.0F);
        lightingData.spotLights[spotCount].coneAngles =
            Vec4(std::cos(sComp.innerConeAngle), std::cos(sComp.outerConeAngle), 0.0F, 0.0F);
        ++spotCount;
    });
    lightingData.numSpotLights = spotCount;
    const uint32_t pcfQuality = static_cast<uint32_t>(settings_.shadows.pcfQuality);
    lightingData.shadowPackedParams = (shadowResolution_ & 0xFFFFU) | ((pcfQuality & 0xFFU) << 16U);

    const bool isIblActive = settings_.lighting.iblEnabled && envMap_.isValid();
    lightingData.iblParams =
        Vec4(isIblActive ? 1.0F : 0.0F, settings_.lighting.environmentIntensity,
             envMap_.maxPrefilterMipLevel, 0.0F);

    // Compute Directional Cascaded Shadow Maps (Milestone 11E)
    Vec3 dir = Vec3(lightingData.dirLight.directionAndCastShadow.x,
                    lightingData.dirLight.directionAndCastShadow.y,
                    lightingData.dirLight.directionAndCastShadow.z);
    if (dir.lengthSq() < 1e-6F) {
        dir = Vec3(0.5F, 0.8F, 0.5F).normalized();
    }

    if (settings_.shadows.enabled) {
        CascadeConfig cascadeConfig{
            .cascadeCount = std::clamp(settings_.shadows.cascadeCount, 1U, 4U),
            .maxDistance = settings_.shadows.distance,
            .splitLambda = settings_.shadows.splitLambda,
            .stabilizeCascades = settings_.shadows.stabilizeCascades,
            .atlasResolution = shadowResolution_,
        };
        const Timestamp tCascadeStart = Timestamp::now();
        computeShadowCascades(Mat4::identity(), viewProjection, 0.1F, settings_.shadows.distance,
                              dir, cascadeConfig, shadowCascades_);
        cpuPassTimings_.shadowCascadeCalcMs =
            static_cast<float>((Timestamp::now() - tCascadeStart).asMicroseconds()) * 0.001F;
        for (size_t i = 0; i < shadowCascades_.size() && i < 4; ++i) {
            lightingData.cascadeLightSpaceMatrices[i] = shadowCascades_[i].atlasMatrix;
        }
        lightingData.cascadeSplits = Vec4(
            shadowCascades_.size() > 0 ? shadowCascades_[0].splitFar : settings_.shadows.distance,
            shadowCascades_.size() > 1 ? shadowCascades_[1].splitFar : settings_.shadows.distance,
            shadowCascades_.size() > 2 ? shadowCascades_[2].splitFar : settings_.shadows.distance,
            shadowCascades_.size() > 3 ? shadowCascades_[3].splitFar : settings_.shadows.distance);
        lightingData.cascadeParams =
            Vec4(static_cast<float>(shadowCascades_.size()), settings_.shadows.cascadeBlendRange,
                 settings_.shadows.contactShadows ? 1.0F : 0.0F,
                 settings_.shadows.softShadows ? 1.0F : 0.0F);
    } else {
        shadowCascades_.clear();
        lightingData.cascadeParams = Vec4(0.0F, 0.0F, 0.0F, 0.0F);
    }

    currentWorld_ = &world;
    currentAssetManager_ = &assetManager;
    currentDevice_ = &device;
    if (assetManager.getQualitySettings() != settings_.quality) {
        assetManager.setQualitySettings(settings_.quality);
    }
    unjitteredViewProj_ = viewProjection;
    if (isTaaEnabled()) {
        uint32_t seqLen = 16;
        if (settings_.quality.taa.jitterSequence == TaaJitterSequence::Halton8) {
            seqLen = 8;
        } else if (settings_.quality.taa.jitterSequence == TaaJitterSequence::Halton32) {
            seqLen = 32;
        }
        const Vec2 jitterPixels = computeHaltonJitter(taaJitterPhase_, seqLen);
        currentViewProj_ =
            viewProjection.withJitter(jitterPixels.x, jitterPixels.y, static_cast<float>(width_),
                                      static_cast<float>(height_));
        taaJitterPhase_ = (taaJitterPhase_ + 1) % seqLen;
    } else {
        currentViewProj_ = viewProjection;
        taaJitterPhase_ = 0;
    }
    currentCameraPos_ = Vec3(lightingData.cameraPosAndAmbientIntensity.x,
                             lightingData.cameraPosAndAmbientIntensity.y,
                             lightingData.cameraPosAndAmbientIntensity.z);
    currentLightSpaceMatrix_ =
        shadowCascades_.empty() ? Mat4::identity() : shadowCascades_[0].viewProjMatrix;
    currentLightDir_ = dir;

    // Global Illumination (Milestone 11D)
    lightingData.giParams =
        Vec4(settings_.lighting.ssgiEnabled ? 1.0F : 0.0F, settings_.lighting.giIntensity,
             settings_.lighting.ssgiRadius,
             settings_.lighting.gtaoEnabled ? settings_.lighting.gtaoIntensity : 0.0F);
    lightingData.prevViewProj = prevViewProj_;
    lightingData.invViewProj = invVPOpt.value_or(Mat4::identity());

    // Blended Irradiance Probes
    if (settings_.lighting.probesEnabled) {
        SphericalHarmonicsL2 blendedSh{};
        float totalProbeWeight = 0.0F;
        auto probeQuery = ecs.query<const IrradianceProbeComponent>();
        probeQuery.each([&](flecs::entity e, const IrradianceProbeComponent& probe) {
            if (!probe.enabled) {
                return;
            }
            Vec3 probePos = probe.position;
            if (e.has<world::WorldTransformComponent>()) {
                const auto* wt = e.get<world::WorldTransformComponent>();
                if (wt != nullptr) {
                    probePos =
                        Vec3(wt->worldMatrix.m[12], wt->worldMatrix.m[13], wt->worldMatrix.m[14]);
                }
            } else if (e.has<world::TransformComponent>()) {
                const auto* tc = e.get<world::TransformComponent>();
                if (tc != nullptr) {
                    probePos = tc->transform.position;
                }
            }
            IrradianceProbeComponent probeCopy = probe;
            probeCopy.position = probePos;
            const float weight = probeCopy.computeWeight(currentCameraPos_);
            if (weight > 1e-4F) {
                SphericalHarmonicsL2 probeSh = probe.sh;
                probeSh *= weight;
                blendedSh += probeSh;
                totalProbeWeight += weight;
            }
        });

        if (totalProbeWeight <= 1e-4F) {
            float minDistanceSq = std::numeric_limits<float>::max();
            SphericalHarmonicsL2 nearestSh{};
            bool foundNearest = false;
            probeQuery.each([&](flecs::entity e, const IrradianceProbeComponent& probe) {
                if (!probe.enabled) {
                    return;
                }
                Vec3 probePos = probe.position;
                if (e.has<world::WorldTransformComponent>()) {
                    const auto* wt = e.get<world::WorldTransformComponent>();
                    if (wt != nullptr) {
                        probePos = Vec3(wt->worldMatrix.m[12], wt->worldMatrix.m[13],
                                        wt->worldMatrix.m[14]);
                    }
                }
                float dSq = (probePos - currentCameraPos_).lengthSq();
                if (dSq < minDistanceSq) {
                    minDistanceSq = dSq;
                    nearestSh = probe.sh;
                    foundNearest = true;
                }
            });
            if (foundNearest) {
                blendedSh = nearestSh;
                totalProbeWeight = 1.0F;
            }
        }

        if (totalProbeWeight > 1e-4F) {
            blendedSh *= (1.0F / totalProbeWeight);
            for (size_t i = 0; i < 9; ++i) {
                lightingData.probeSh[i] =
                    Vec4(blendedSh.coefficients[i].x, blendedSh.coefficients[i].y,
                         blendedSh.coefficients[i].z, 0.0F);
            }
            lightingData.probeParams =
                Vec4(1.0F, settings_.lighting.giIntensity, totalProbeWeight, 0.0F);
        } else {
            lightingData.probeParams = Vec4(0.0F, 0.0F, 0.0F, 0.0F);
        }

        // Milestone 11G: Specular Reflection Probes with Box-Projection & Spatial Blending
        Vec3 dominantProbePos{0.0F, 0.0F, 0.0F};
        Vec3 dominantBoxMin{-10.0F, -10.0F, -10.0F};
        Vec3 dominantBoxMax{10.0F, 10.0F, 10.0F};
        float maxProbeWeight = 0.0F;
        float totalReflWeight = 0.0F;
        bool dominantBoxProj = false;

        auto reflQuery = ecs.query<const ReflectionProbeComponent>();
        reflQuery.each([&](flecs::entity e, const ReflectionProbeComponent& probe) {
            if (!probe.enabled) {
                return;
            }
            Vec3 probePos = probe.position;
            Vec3 boxMin = probe.boxMin;
            Vec3 boxMax = probe.boxMax;
            if (e.has<world::WorldTransformComponent>()) {
                const auto* wt = e.get<world::WorldTransformComponent>();
                if (wt != nullptr) {
                    probePos =
                        Vec3(wt->worldMatrix.m[12], wt->worldMatrix.m[13], wt->worldMatrix.m[14]);
                    boxMin = boxMin + probePos - probe.position;
                    boxMax = boxMax + probePos - probe.position;
                }
            } else if (e.has<world::TransformComponent>()) {
                const auto* tc = e.get<world::TransformComponent>();
                if (tc != nullptr) {
                    probePos = tc->transform.position;
                    boxMin = boxMin + probePos - probe.position;
                    boxMax = boxMax + probePos - probe.position;
                }
            }
            ReflectionProbeComponent probeCopy = probe;
            probeCopy.position = probePos;
            probeCopy.boxMin = boxMin;
            probeCopy.boxMax = boxMax;
            const float weight = probeCopy.computeWeight(currentCameraPos_);
            totalReflWeight += weight;
            if (weight > maxProbeWeight) {
                maxProbeWeight = weight;
                dominantProbePos = probePos;
                dominantBoxMin = boxMin;
                dominantBoxMax = boxMax;
                dominantBoxProj = probe.boxProjection;
            }
        });

        if (maxProbeWeight > 1e-4F) {
            float blendWeight = std::clamp(totalReflWeight, 0.0F, 1.0F);
            lightingData.probeBoxMin = Vec4(dominantBoxMin.x, dominantBoxMin.y, dominantBoxMin.z,
                                            dominantBoxProj ? 1.0F : 0.0F);
            lightingData.probeBoxMax =
                Vec4(dominantBoxMax.x, dominantBoxMax.y, dominantBoxMax.z, 0.0F);
            lightingData.probePosAndWeight =
                Vec4(dominantProbePos.x, dominantProbePos.y, dominantProbePos.z, blendWeight);
        } else {
            lightingData.probeBoxMin = Vec4(-10.0F, -10.0F, -10.0F, 0.0F);
            lightingData.probeBoxMax = Vec4(10.0F, 10.0F, 10.0F, 0.0F);
            lightingData.probePosAndWeight = Vec4(0.0F, 0.0F, 0.0F, 0.0F);
        }
    } else {
        lightingData.probeParams = Vec4(0.0F, 0.0F, 0.0F, 0.0F);
        lightingData.probeBoxMin = Vec4(-10.0F, -10.0F, -10.0F, 0.0F);
        lightingData.probeBoxMax = Vec4(10.0F, 10.0F, 10.0F, 0.0F);
        lightingData.probePosAndWeight = Vec4(0.0F, 0.0F, 0.0F, 0.0F);
    }

    // Milestone 11G: Screen-Space Reflections (SSR) parameters
    lightingData.ssrParams =
        Vec4(settings_.lighting.ssrEnabled ? 1.0F : 0.0F, settings_.lighting.ssrIntensity,
             settings_.lighting.ssrRoughnessCutoff, settings_.lighting.ssrMaxDistance);

    // Milestone 11I: Temporal luminance adaptation (Auto-Exposure)
    if (postProcessSettings_.autoExposureEnabled) {
        const auto lumaVec = [](const Vec4& c) -> float {
            return (0.2126F * c.x + 0.7152F * c.y + 0.0722F * c.z) * std::max(0.0F, c.w);
        };
        float estSceneLuminance = lumaVec(Vec4(
            lightingData.ambientColorAndShadowBias.x, lightingData.ambientColorAndShadowBias.y,
            lightingData.ambientColorAndShadowBias.z, lightingData.cameraPosAndAmbientIntensity.w));
        if (lightingData.dirLightActive != 0) {
            estSceneLuminance += 0.25F * lumaVec(lightingData.dirLight.colorAndIntensity);
        }
        if (lightingData.iblParams.x > 0.0F) {
            estSceneLuminance += 0.18F * std::max(0.0F, lightingData.iblParams.y);
        }
        for (uint32_t i = 0; i < lightingData.numPointLights && i < kMaxPointLights; ++i) {
            estSceneLuminance += 0.05F * lumaVec(lightingData.pointLights[i].colorAndIntensity);
        }
        updateExposureAdaptation(std::max(1e-4F, estSceneLuminance), 1.0F / 60.0F);
    }

    // Upload lighting data to GPU uniform buffer
    if (lightingUniformBuffer_.isValid()) {
        std::ignore =
            device.writeBuffer(lightingUniformBuffer_, 0, &lightingData, sizeof(GpuLightingData));
    }
    uint64_t currentStaticChecksum = lastStaticCasterChecksum_;
    uint64_t currentDynamicChecksum = lastDynamicCasterChecksum_;
    uint32_t staticCasterCount = lastStaticCasterCount_;
    uint32_t dynamicCasterCount = lastDynamicCasterCount_;
    const int32_t currentMeshCount = ecs.count<MeshComponent>();
    const uint64_t currentTransformVersion = world.getTransformVersion();
    const bool needChecksumQuery =
        settings_.shadows.enabled &&
        (&world != lastWorld_ || currentTransformVersion != lastTransformVersion_ ||
         currentMeshCount != lastMeshComponentCount_ || !dir.isNearlyEqual(lastLightDir_) ||
         lastDynamicCasterCount_ > 0);
    if (needChecksumQuery) {
        currentStaticChecksum = 0;
        currentDynamicChecksum = 0;
        staticCasterCount = 0;
        dynamicCasterCount = 0;
        const Timestamp tChecksumStart = Timestamp::now();
        ecs.each<const MeshComponent>([&](flecs::entity e, const MeshComponent& mc) {
            if (!mc.visible || !mc.castShadows) {
                return;
            }
            const auto* ptc = e.get<physics::PhysicsTransformComponent>();
            const bool isDynamic =
                (!mc.isStatic) || (ptc != nullptr) || e.has<physics::RigidBodyComponent>();

            uint64_t entChecksum = static_cast<uint64_t>(e.id()) * 1099511628211ULL;
            Vec3 trans = Vec3::zero();
            if (const auto* wt = e.get<world::WorldTransformComponent>()) {
                trans = wt->worldMatrix.getTranslation();
            } else if (const auto* tc = e.get<world::TransformComponent>()) {
                trans = tc->transform.position;
            } else if (ptc != nullptr) {
                trans = ptc->transform.position;
            }

            float tx = trans.x;
            float ty = trans.y;
            float tz = trans.z;
            uint32_t bx = 0;
            uint32_t by = 0;
            uint32_t bz = 0;
            std::memcpy(&bx, &tx, sizeof(float));
            std::memcpy(&by, &ty, sizeof(float));
            std::memcpy(&bz, &tz, sizeof(float));
            entChecksum ^= (static_cast<uint64_t>(bx) | (static_cast<uint64_t>(by) << 32));
            entChecksum ^= (static_cast<uint64_t>(bz) * 16777619ULL);

            if (isDynamic) {
                dynamicCasterCount++;
                currentDynamicChecksum ^= entChecksum;
            } else {
                staticCasterCount++;
                currentStaticChecksum ^= entChecksum;
            }
        });
        cpuPassTimings_.shadowChecksumQueryMs =
            static_cast<float>((Timestamp::now() - tChecksumStart).asMicroseconds()) * 0.001F;
    }

    bool cascadesChanged = false;
    if (shadowCascades_.empty() || !settings_.shadows.shadowCaching) {
        cascadesChanged = true;
    } else {
        for (size_t i = 0; i < shadowCascades_.size(); ++i) {
            if (!shadowCascades_[i].viewProjMatrix.isNearlyEqual(lastCascadeViewProj_[i])) {
                cascadesChanged = true;
                break;
            }
        }
    }

    const bool staticCastersChanged =
        (&world != lastWorld_ || staticCasterCount != lastStaticCasterCount_ ||
         currentStaticChecksum != lastStaticCasterChecksum_ || !dir.isNearlyEqual(lastLightDir_));
    if (staticCastersChanged) {
        staticCastersListDirty_ = true;
        staticShadowDirty_ = true;
        shadowDirty_ = true;
    } else if (cascadesChanged) {
        staticShadowDirty_ = true;
        shadowDirty_ = true;
    } else if (dynamicCasterCount != lastDynamicCasterCount_ ||
               currentDynamicChecksum != lastDynamicCasterChecksum_) {
        shadowDirty_ = true;
    }

    lastWorld_ = &world;
    lastTransformVersion_ = currentTransformVersion;
    lastMeshComponentCount_ = currentMeshCount;
    lastStaticCasterCount_ = staticCasterCount;
    lastStaticCasterChecksum_ = currentStaticChecksum;
    lastDynamicCasterCount_ = dynamicCasterCount;
    lastDynamicCasterChecksum_ = currentDynamicChecksum;
    lastCasterCount_ = staticCasterCount + dynamicCasterCount;
    lastCasterChecksum_ = currentStaticChecksum ^ currentDynamicChecksum;
    lastLightDir_ = dir;
    for (size_t i = 0; i < shadowCascades_.size() && i < 4; ++i) {
        lastCascadeViewProj_[i] = shadowCascades_[i].viewProjMatrix;
    }
    lastRenderStats_ = RenderStats{};

    if (!renderGraph_.isCompiled()) {
        buildRenderGraph();
    }
    updateGiDescriptors();

    // Update Set 1 bindings 5, 6, & 7 for Main PBR Pass
    const rhi::TextureHandle aoTex =
        ((settings_.lighting.gtaoEnabled || settings_.shadows.contactShadows) &&
         gtaoFilteredTarget_.isValid())
            ? gtaoFilteredTarget_
            : (defaultAoTexture_ ? defaultAoTexture_->getTexture() : rhi::TextureHandle{});
    const rhi::SamplerHandle aoSamp =
        linearClampSampler_.isValid() ? linearClampSampler_ : shadowSampler_;
    const rhi::TextureHandle indTex =
        (settings_.lighting.ssgiEnabled && ssgiFilteredTarget_.isValid())
            ? ssgiFilteredTarget_
            : (defaultIndirectTexture_ ? defaultIndirectTexture_->getTexture()
                                       : rhi::TextureHandle{});
    const rhi::SamplerHandle indSamp =
        linearClampSampler_.isValid() ? linearClampSampler_ : shadowSampler_;
    const rhi::TextureHandle ssrTex =
        (settings_.lighting.ssrEnabled && ssrFilteredTarget_.isValid())
            ? ssrFilteredTarget_
            : (defaultSsrTexture_ ? defaultSsrTexture_->getTexture() : rhi::TextureHandle{});
    const rhi::SamplerHandle ssrSamp =
        linearClampSampler_.isValid() ? linearClampSampler_ : shadowSampler_;

    if (sceneDescriptorSet_.isValid()) {
        std::array<rhi::DescriptorWrite, 3> giWrites{
            rhi::DescriptorWrite::makeCombinedImageSampler(5, aoTex, aoSamp),
            rhi::DescriptorWrite::makeCombinedImageSampler(6, indTex, indSamp),
            rhi::DescriptorWrite::makeCombinedImageSampler(7, ssrTex, ssrSamp),
        };
        std::ignore = device.updateDescriptorSet(sceneDescriptorSet_, giWrites);
    }

    updateTaaDescriptors();
    updatePostProcessDescriptors();

    if (renderGraph_.isCompiled()) {
        const Timestamp tRgStart = Timestamp::now();
        auto execRes = renderGraph_.execute(device, cmdList);
        cpuPassTimings_.renderGraphExecMs =
            static_cast<float>((Timestamp::now() - tRgStart).asMicroseconds()) * 0.001F;
        if (!execRes.hasValue()) {
            ENG_LOG_ERROR("SceneRenderer", "RenderGraph execute failed");
        }
    }

    prevViewProj_ = viewProjection;
    isHistoryValid_ = true;
    giFrameIndex_++;

    unjitteredPrevViewProj_ = unjitteredViewProj_;
    isTaaHistoryValid_ = true;

    currentWorld_ = nullptr;
    currentAssetManager_ = nullptr;
    currentDevice_ = nullptr;
    cpuPassTimings_.totalRenderSceneMs =
        static_cast<float>((Timestamp::now() - tRenderSceneStart).asMicroseconds()) * 0.001F;
    return lastRenderStats_;
}

Result<void> SceneRenderer::initGiResources() {
    if (device_ == nullptr) {
        return Error{.code = ErrorCode::InvalidState, .message = "Device not initialized"};
    }

    // 1. Samplers
    rhi::SamplerDesc pointDesc{
        .minFilter = rhi::Filter::Nearest,
        .magFilter = rhi::Filter::Nearest,
        .mipmapMode = rhi::Filter::Nearest,
        .addressU = rhi::SamplerAddressMode::ClampToEdge,
        .addressV = rhi::SamplerAddressMode::ClampToEdge,
        .addressW = rhi::SamplerAddressMode::ClampToEdge,
        .debugName = "GiPointClampSampler",
    };
    auto ptRes = device_->createSampler(pointDesc);
    if (!ptRes.hasValue()) {
        return ptRes.error();
    }
    pointClampSampler_ = ptRes.value();

    rhi::SamplerDesc linDesc{
        .minFilter = rhi::Filter::Linear,
        .magFilter = rhi::Filter::Linear,
        .mipmapMode = rhi::Filter::Linear,
        .addressU = rhi::SamplerAddressMode::ClampToEdge,
        .addressV = rhi::SamplerAddressMode::ClampToEdge,
        .addressW = rhi::SamplerAddressMode::ClampToEdge,
        .debugName = "GiLinearClampSampler",
    };
    auto linRes = device_->createSampler(linDesc);
    if (!linRes.hasValue()) {
        return linRes.error();
    }
    linearClampSampler_ = linRes.value();

    // 2. Triple texture descriptor set layout (for temporal filter & post-process)
    if (!tripleTextureSetLayout_.isValid()) {
        rhi::DescriptorSetLayoutDesc tripleDesc{
            .bindings =
                {
                    {
                        .binding = 0,
                        .descriptorType = rhi::DescriptorType::CombinedImageSampler,
                        .descriptorCount = 1,
                        .stageFlags = rhi::ShaderStageFlags::Fragment,
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
                },
            .debugName = "GiTripleTextureLayout",
        };
        auto tripleRes = device_->createDescriptorSetLayout(tripleDesc);
        if (!tripleRes.hasValue()) {
            return tripleRes.error();
        }
        tripleTextureSetLayout_ = tripleRes.value();
    }

    // 3. GI Descriptor Pool (max 16 sets, 32 image samplers)
    rhi::DescriptorPoolDesc poolDesc{
        .maxSets = 16,
        .poolSizes =
            {
                {.type = rhi::DescriptorType::CombinedImageSampler, .count = 32},
            },
        .debugName = "GiDescriptorPool",
    };
    auto poolRes = device_->createDescriptorPool(poolDesc);
    if (!poolRes.hasValue()) {
        return poolRes.error();
    }
    giDescriptorPool_ = poolRes.value();

    auto gtaoSetRes = device_->allocateDescriptorSet(giDescriptorPool_, singleTextureSetLayout_);
    if (!gtaoSetRes.hasValue()) {
        return gtaoSetRes.error();
    }
    gtaoDescriptorSet_ = gtaoSetRes.value();

    auto gtaoBlurSetRes = device_->allocateDescriptorSet(giDescriptorPool_, dualTextureSetLayout_);
    if (!gtaoBlurSetRes.hasValue()) {
        return gtaoBlurSetRes.error();
    }
    gtaoBlurDescriptorSet_ = gtaoBlurSetRes.value();

    auto ssgiSetRes = device_->allocateDescriptorSet(giDescriptorPool_, dualTextureSetLayout_);
    if (!ssgiSetRes.hasValue()) {
        return ssgiSetRes.error();
    }
    ssgiDescriptorSet_ = ssgiSetRes.value();

    auto ssrSetRes = device_->allocateDescriptorSet(giDescriptorPool_, dualTextureSetLayout_);
    if (!ssrSetRes.hasValue()) {
        return ssrSetRes.error();
    }
    ssrDescriptorSet_ = ssrSetRes.value();

    auto tfSetRes = device_->allocateDescriptorSet(giDescriptorPool_, tripleTextureSetLayout_);
    if (!tfSetRes.hasValue()) {
        return tfSetRes.error();
    }
    temporalFilterDescriptorSet_ = tfSetRes.value();

    auto ssrTfSetRes = device_->allocateDescriptorSet(giDescriptorPool_, tripleTextureSetLayout_);
    if (!ssrTfSetRes.hasValue()) {
        return ssrTfSetRes.error();
    }
    ssrTemporalFilterDescriptorSet_ = ssrTfSetRes.value();

    // 4. Shaders
    rhi::ShaderDesc gtaoDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kGtaoFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "GtaoFragmentShader",
    };
    auto gtaoShaderRes = device_->createShader(gtaoDesc);
    if (!gtaoShaderRes.hasValue()) {
        return gtaoShaderRes.error();
    }
    gtaoFragmentShader_ = gtaoShaderRes.value();

    rhi::ShaderDesc blurDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kBilateralBlurFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "BilateralBlurFragmentShader",
    };
    auto blurShaderRes = device_->createShader(blurDesc);
    if (!blurShaderRes.hasValue()) {
        return blurShaderRes.error();
    }
    bilateralBlurFragmentShader_ = blurShaderRes.value();

    rhi::ShaderDesc ssgiDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kSsgiFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "SsgiFragmentShader",
    };
    auto ssgiShaderRes = device_->createShader(ssgiDesc);
    if (!ssgiShaderRes.hasValue()) {
        return ssgiShaderRes.error();
    }
    ssgiFragmentShader_ = ssgiShaderRes.value();

    rhi::ShaderDesc ssrDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kSsrFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "SsrFragmentShader",
    };
    auto ssrShaderRes = device_->createShader(ssrDesc);
    if (!ssrShaderRes.hasValue()) {
        return ssrShaderRes.error();
    }
    ssrFragmentShader_ = ssrShaderRes.value();

    rhi::ShaderDesc tfDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kTemporalFilterFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "TemporalFilterFragmentShader",
    };
    auto tfShaderRes = device_->createShader(tfDesc);
    if (!tfShaderRes.hasValue()) {
        return tfShaderRes.error();
    }
    temporalFilterFragmentShader_ = tfShaderRes.value();

    // 5. Pipeline Layouts
    rhi::PipelineLayoutDesc gtaoLayoutDesc{
        .pushConstants = {{
            .stageFlags = rhi::ShaderStageFlags::Fragment,
            .offset = 0,
            .size = sizeof(GtaoPushConstants),
        }},
        .descriptorSetLayoutHandles = {singleTextureSetLayout_},
        .descriptorSetLayouts = {},
        .debugName = "GtaoPipelineLayout",
    };
    auto gtaoLayRes = device_->createPipelineLayout(gtaoLayoutDesc);
    if (!gtaoLayRes.hasValue()) {
        return gtaoLayRes.error();
    }
    gtaoPipelineLayout_ = gtaoLayRes.value();

    rhi::PipelineLayoutDesc blurLayoutDesc{
        .pushConstants = {{
            .stageFlags = rhi::ShaderStageFlags::Fragment,
            .offset = 0,
            .size = sizeof(BilateralBlurPushConstants),
        }},
        .descriptorSetLayoutHandles = {dualTextureSetLayout_},
        .descriptorSetLayouts = {},
        .debugName = "GtaoBlurPipelineLayout",
    };
    auto blurLayRes = device_->createPipelineLayout(blurLayoutDesc);
    if (!blurLayRes.hasValue()) {
        return blurLayRes.error();
    }
    gtaoBlurPipelineLayout_ = blurLayRes.value();

    rhi::PipelineLayoutDesc ssgiLayoutDesc{
        .pushConstants = {{
            .stageFlags = rhi::ShaderStageFlags::Fragment,
            .offset = 0,
            .size = sizeof(SsgiPushConstants),
        }},
        .descriptorSetLayoutHandles = {dualTextureSetLayout_},
        .descriptorSetLayouts = {},
        .debugName = "SsgiPipelineLayout",
    };
    auto ssgiLayRes = device_->createPipelineLayout(ssgiLayoutDesc);
    if (!ssgiLayRes.hasValue()) {
        return ssgiLayRes.error();
    }
    ssgiPipelineLayout_ = ssgiLayRes.value();

    rhi::PipelineLayoutDesc ssrLayoutDesc{
        .pushConstants = {{
            .stageFlags = rhi::ShaderStageFlags::Fragment,
            .offset = 0,
            .size = sizeof(SsrPushConstants),
        }},
        .descriptorSetLayoutHandles = {dualTextureSetLayout_},
        .descriptorSetLayouts = {},
        .debugName = "SsrPipelineLayout",
    };
    auto ssrLayRes = device_->createPipelineLayout(ssrLayoutDesc);
    if (!ssrLayRes.hasValue()) {
        return ssrLayRes.error();
    }
    ssrPipelineLayout_ = ssrLayRes.value();

    rhi::PipelineLayoutDesc tfLayoutDesc{
        .pushConstants = {{
            .stageFlags = rhi::ShaderStageFlags::Fragment,
            .offset = 0,
            .size = sizeof(TemporalFilterPushConstants),
        }},
        .descriptorSetLayoutHandles = {tripleTextureSetLayout_},
        .descriptorSetLayouts = {},
        .debugName = "TemporalFilterPipelineLayout",
    };
    auto tfLayRes = device_->createPipelineLayout(tfLayoutDesc);
    if (!tfLayRes.hasValue()) {
        return tfLayRes.error();
    }
    temporalFilterPipelineLayout_ = tfLayRes.value();

    // 6. Graphics Pipelines
    rhi::GraphicsPipelineDesc gtaoPipeDesc{
        .vertexShader = fullscreenVertexShader_,
        .fragmentShader = gtaoFragmentShader_,
        .vertexInput = {},
        .topology = rhi::PrimitiveTopology::TriangleList,
        .rasterization =
            {
                .polygonMode = rhi::PolygonMode::Fill,
                .cullMode = rhi::CullMode::None,
                .frontFace = rhi::FrontFace::CounterClockwise,
            },
        .depthStencil =
            {
                .depthTestEnable = false,
                .depthWriteEnable = false,
                .depthCompareOp = rhi::CompareOp::Always,
            },
        .colorBlendAttachments = {{
            .blendEnable = false,
            .colorWriteMask = rhi::ColorComponentFlags::All,
        }},
        .layout = gtaoPipelineLayout_,
        .colorAttachmentFormats = {rhi::TextureFormat::RGBA8_UNORM},
        .debugName = "GtaoGraphicsPipeline",
    };
    auto gtaoPipeRes = device_->createGraphicsPipeline(gtaoPipeDesc);
    if (!gtaoPipeRes.hasValue()) {
        return gtaoPipeRes.error();
    }
    gtaoPipeline_ = gtaoPipeRes.value();

    auto blurPipeDesc = gtaoPipeDesc;
    blurPipeDesc.fragmentShader = bilateralBlurFragmentShader_;
    blurPipeDesc.layout = gtaoBlurPipelineLayout_;
    blurPipeDesc.debugName = "GtaoBlurGraphicsPipeline";
    auto blurPipeRes = device_->createGraphicsPipeline(blurPipeDesc);
    if (!blurPipeRes.hasValue()) {
        return blurPipeRes.error();
    }
    gtaoBlurPipeline_ = blurPipeRes.value();

    auto ssgiPipeDesc = gtaoPipeDesc;
    ssgiPipeDesc.fragmentShader = ssgiFragmentShader_;
    ssgiPipeDesc.layout = ssgiPipelineLayout_;
    ssgiPipeDesc.colorAttachmentFormats = {hdrFormat_};
    ssgiPipeDesc.debugName = "SsgiGraphicsPipeline";
    auto ssgiPipeRes = device_->createGraphicsPipeline(ssgiPipeDesc);
    if (!ssgiPipeRes.hasValue()) {
        return ssgiPipeRes.error();
    }
    ssgiPipeline_ = ssgiPipeRes.value();

    auto ssrPipeDesc = gtaoPipeDesc;
    ssrPipeDesc.fragmentShader = ssrFragmentShader_;
    ssrPipeDesc.layout = ssrPipelineLayout_;
    ssrPipeDesc.colorAttachmentFormats = {hdrFormat_};
    ssrPipeDesc.debugName = "SsrGraphicsPipeline";
    auto ssrPipeRes = device_->createGraphicsPipeline(ssrPipeDesc);
    if (!ssrPipeRes.hasValue()) {
        return ssrPipeRes.error();
    }
    ssrPipeline_ = ssrPipeRes.value();

    auto tfPipeDesc = gtaoPipeDesc;
    tfPipeDesc.fragmentShader = temporalFilterFragmentShader_;
    tfPipeDesc.layout = temporalFilterPipelineLayout_;
    tfPipeDesc.colorAttachmentFormats = {hdrFormat_};
    tfPipeDesc.debugName = "TemporalFilterGraphicsPipeline";
    auto tfPipeRes = device_->createGraphicsPipeline(tfPipeDesc);
    if (!tfPipeRes.hasValue()) {
        return tfPipeRes.error();
    }
    temporalFilterPipeline_ = tfPipeRes.value();

    return Result<void>::success();
}

void SceneRenderer::destroyGiResources() {
    if (device_ != nullptr) {
        destroyGiTargets();

        if (temporalFilterPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(temporalFilterPipeline_);
            temporalFilterPipeline_ = {};
        }
        if (ssrPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(ssrPipeline_);
            ssrPipeline_ = {};
        }
        if (ssgiPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(ssgiPipeline_);
            ssgiPipeline_ = {};
        }
        if (gtaoBlurPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(gtaoBlurPipeline_);
            gtaoBlurPipeline_ = {};
        }
        if (gtaoPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(gtaoPipeline_);
            gtaoPipeline_ = {};
        }

        if (temporalFilterPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(temporalFilterPipelineLayout_);
            temporalFilterPipelineLayout_ = {};
        }
        if (ssrPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(ssrPipelineLayout_);
            ssrPipelineLayout_ = {};
        }
        if (ssgiPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(ssgiPipelineLayout_);
            ssgiPipelineLayout_ = {};
        }
        if (gtaoBlurPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(gtaoBlurPipelineLayout_);
            gtaoBlurPipelineLayout_ = {};
        }
        if (gtaoPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(gtaoPipelineLayout_);
            gtaoPipelineLayout_ = {};
        }

        if (giDescriptorPool_.isValid()) {
            device_->destroyDescriptorPool(giDescriptorPool_);
            giDescriptorPool_ = {};
            gtaoDescriptorSet_ = {};
            gtaoBlurDescriptorSet_ = {};
            ssgiDescriptorSet_ = {};
            ssrDescriptorSet_ = {};
            temporalFilterDescriptorSet_ = {};
            ssrTemporalFilterDescriptorSet_ = {};
        }

        if (tripleTextureSetLayout_.isValid()) {
            device_->destroyDescriptorSetLayout(tripleTextureSetLayout_);
            tripleTextureSetLayout_ = {};
        }

        if (temporalFilterFragmentShader_.isValid()) {
            device_->destroyShader(temporalFilterFragmentShader_);
            temporalFilterFragmentShader_ = {};
        }
        if (ssrFragmentShader_.isValid()) {
            device_->destroyShader(ssrFragmentShader_);
            ssrFragmentShader_ = {};
        }
        if (ssgiFragmentShader_.isValid()) {
            device_->destroyShader(ssgiFragmentShader_);
            ssgiFragmentShader_ = {};
        }
        if (bilateralBlurFragmentShader_.isValid()) {
            device_->destroyShader(bilateralBlurFragmentShader_);
            bilateralBlurFragmentShader_ = {};
        }
        if (gtaoFragmentShader_.isValid()) {
            device_->destroyShader(gtaoFragmentShader_);
            gtaoFragmentShader_ = {};
        }

        if (linearClampSampler_.isValid()) {
            device_->destroySampler(linearClampSampler_);
            linearClampSampler_ = {};
        }
        if (pointClampSampler_.isValid()) {
            device_->destroySampler(pointClampSampler_);
            pointClampSampler_ = {};
        }

        defaultAoTexture_.reset();
        defaultIndirectTexture_.reset();
        defaultSsrTexture_.reset();
    }
}

Result<void> SceneRenderer::createGiTargets(uint32_t width, uint32_t height) {
    if (device_ == nullptr || width == 0 || height == 0) {
        return Result<void>::success();
    }

    // 1. GTAO Targets: Half-resolution Raw Target for Tier 0 performance,
    // Full-resolution Filtered Target via bilateral depth-aware upsampling
    const uint32_t halfW = std::max(1u, width / 2);
    const uint32_t halfH = std::max(1u, height / 2);

    rhi::TextureDesc gtaoRawDesc{
        .type = rhi::TextureType::Texture2D,
        .format = rhi::TextureFormat::RGBA8_UNORM,
        .width = halfW,
        .height = halfH,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = rhi::TextureUsageFlags::ColorAttachment | rhi::TextureUsageFlags::Sampled |
                 rhi::TextureUsageFlags::TransferSrc,
        .debugName = "GtaoRawTarget",
    };
    auto gtaoRawRes = device_->createTexture(gtaoRawDesc);
    if (!gtaoRawRes.hasValue()) {
        return gtaoRawRes.error();
    }
    gtaoRawTarget_ = gtaoRawRes.value();

    auto gtaoFiltDesc = gtaoRawDesc;
    gtaoFiltDesc.width = width;
    gtaoFiltDesc.height = height;
    gtaoFiltDesc.debugName = "GtaoFilteredTarget";
    auto gtaoFiltRes = device_->createTexture(gtaoFiltDesc);
    if (!gtaoFiltRes.hasValue()) {
        return gtaoFiltRes.error();
    }
    gtaoFilteredTarget_ = gtaoFiltRes.value();

    // 2. Half-resolution SSGI & Temporal Denoise Targets
    rhi::TextureDesc ssgiDesc{
        .type = rhi::TextureType::Texture2D,
        .format = hdrFormat_,
        .width = halfW,
        .height = halfH,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = rhi::TextureUsageFlags::ColorAttachment | rhi::TextureUsageFlags::Sampled |
                 rhi::TextureUsageFlags::TransferSrc,
        .debugName = "SsgiRawTarget",
    };
    auto ssgiRawRes = device_->createTexture(ssgiDesc);
    if (!ssgiRawRes.hasValue()) {
        return ssgiRawRes.error();
    }
    ssgiRawTarget_ = ssgiRawRes.value();

    auto ssgiFiltDesc = ssgiDesc;
    ssgiFiltDesc.debugName = "SsgiFilteredTarget";
    auto ssgiFiltRes = device_->createTexture(ssgiFiltDesc);
    if (!ssgiFiltRes.hasValue()) {
        return ssgiFiltRes.error();
    }
    ssgiFilteredTarget_ = ssgiFiltRes.value();

    for (size_t i = 0; i < 2; ++i) {
        auto histDesc = ssgiDesc;
        histDesc.debugName = (i == 0) ? "SsgiHistoryTarget0" : "SsgiHistoryTarget1";
        auto histRes = device_->createTexture(histDesc);
        if (!histRes.hasValue()) {
            return histRes.error();
        }
        ssgiHistoryTarget_[i] = histRes.value();
    }

    // 3. Milestone 11G: Half-resolution SSR & Temporal Denoise Targets
    rhi::TextureDesc ssrDesc{
        .type = rhi::TextureType::Texture2D,
        .format = hdrFormat_,
        .width = halfW,
        .height = halfH,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = rhi::TextureUsageFlags::ColorAttachment | rhi::TextureUsageFlags::Sampled |
                 rhi::TextureUsageFlags::TransferSrc,
        .debugName = "SsrRawTarget",
    };
    auto ssrRawRes = device_->createTexture(ssrDesc);
    if (!ssrRawRes.hasValue()) {
        return ssrRawRes.error();
    }
    ssrRawTarget_ = ssrRawRes.value();

    auto ssrFiltDesc = ssrDesc;
    ssrFiltDesc.debugName = "SsrFilteredTarget";
    auto ssrFiltRes = device_->createTexture(ssrFiltDesc);
    if (!ssrFiltRes.hasValue()) {
        return ssrFiltRes.error();
    }
    ssrFilteredTarget_ = ssrFiltRes.value();

    for (size_t i = 0; i < 2; ++i) {
        auto histDesc = ssrDesc;
        histDesc.debugName = (i == 0) ? "SsrHistoryTarget0" : "SsrHistoryTarget1";
        auto histRes = device_->createTexture(histDesc);
        if (!histRes.hasValue()) {
            return histRes.error();
        }
        ssrHistoryTarget_[i] = histRes.value();
    }

    updateGiDescriptors();

    return Result<void>::success();
}

void SceneRenderer::destroyGiTargets() {
    if (device_ != nullptr) {
        if (gtaoRawTarget_.isValid()) {
            device_->destroyTexture(gtaoRawTarget_);
            gtaoRawTarget_ = {};
        }
        if (gtaoFilteredTarget_.isValid()) {
            device_->destroyTexture(gtaoFilteredTarget_);
            gtaoFilteredTarget_ = {};
        }
        if (ssgiRawTarget_.isValid()) {
            device_->destroyTexture(ssgiRawTarget_);
            ssgiRawTarget_ = {};
        }
        if (ssgiFilteredTarget_.isValid()) {
            device_->destroyTexture(ssgiFilteredTarget_);
            ssgiFilteredTarget_ = {};
        }
        for (size_t i = 0; i < 2; ++i) {
            if (ssgiHistoryTarget_[i].isValid()) {
                device_->destroyTexture(ssgiHistoryTarget_[i]);
                ssgiHistoryTarget_[i] = {};
            }
        }
        if (ssrRawTarget_.isValid()) {
            device_->destroyTexture(ssrRawTarget_);
            ssrRawTarget_ = {};
        }
        if (ssrFilteredTarget_.isValid()) {
            device_->destroyTexture(ssrFilteredTarget_);
            ssrFilteredTarget_ = {};
        }
        for (size_t i = 0; i < 2; ++i) {
            if (ssrHistoryTarget_[i].isValid()) {
                device_->destroyTexture(ssrHistoryTarget_[i]);
                ssrHistoryTarget_[i] = {};
            }
        }
    }
}

void SceneRenderer::updateGiDescriptors() {
    if (device_ == nullptr || !pointClampSampler_.isValid() || !linearClampSampler_.isValid()) {
        return;
    }

    // 1. GTAO Descriptor Set: Binding 0 = DepthTarget with PointSampler
    if (gtaoDescriptorSet_.isValid() && depthTarget_.isValid()) {
        std::array<rhi::DescriptorWrite, 1> writes{
            rhi::DescriptorWrite::makeCombinedImageSampler(0, depthTarget_, pointClampSampler_),
        };
        std::ignore = device_->updateDescriptorSet(gtaoDescriptorSet_, writes);
    }

    // 2. GTAO Bilateral Blur Descriptor Set: Binding 0 = GtaoRawTarget, Binding 1 = DepthTarget
    if (gtaoBlurDescriptorSet_.isValid() && gtaoRawTarget_.isValid() && depthTarget_.isValid()) {
        std::array<rhi::DescriptorWrite, 2> writes{
            rhi::DescriptorWrite::makeCombinedImageSampler(0, gtaoRawTarget_, linearClampSampler_),
            rhi::DescriptorWrite::makeCombinedImageSampler(1, depthTarget_, pointClampSampler_),
        };
        std::ignore = device_->updateDescriptorSet(gtaoBlurDescriptorSet_, writes);
    }

    // 3. SSGI Descriptor Set: Binding 0 = DepthTarget, Binding 1 = HdrTarget (scene radiance)
    if (ssgiDescriptorSet_.isValid() && depthTarget_.isValid() && hdrTarget_.isValid()) {
        std::array<rhi::DescriptorWrite, 2> writes{
            rhi::DescriptorWrite::makeCombinedImageSampler(0, depthTarget_, pointClampSampler_),
            rhi::DescriptorWrite::makeCombinedImageSampler(1, hdrTarget_, linearClampSampler_),
        };
        std::ignore = device_->updateDescriptorSet(ssgiDescriptorSet_, writes);
    }

    // 4. Temporal Filter Descriptor Set: Binding 0 = SsgiRawTarget, Binding 1 = HistoryTarget,
    // Binding 2 = DepthTarget
    const uint32_t prevSlot = (giFrameIndex_ + 1) % 2;
    rhi::TextureHandle historyTex =
        ssgiHistoryTarget_[prevSlot].isValid() ? ssgiHistoryTarget_[prevSlot] : ssgiRawTarget_;
    if (temporalFilterDescriptorSet_.isValid() && ssgiRawTarget_.isValid() &&
        historyTex.isValid() && depthTarget_.isValid()) {
        std::array<rhi::DescriptorWrite, 3> writes{
            rhi::DescriptorWrite::makeCombinedImageSampler(0, ssgiRawTarget_, linearClampSampler_),
            rhi::DescriptorWrite::makeCombinedImageSampler(1, historyTex, linearClampSampler_),
            rhi::DescriptorWrite::makeCombinedImageSampler(2, depthTarget_, pointClampSampler_),
        };
        std::ignore = device_->updateDescriptorSet(temporalFilterDescriptorSet_, writes);
    }

    // 5. Milestone 11G: SSR Descriptor Set: Binding 0 = DepthTarget, Binding 1 = HdrTarget (scene
    // radiance)
    if (ssrDescriptorSet_.isValid() && depthTarget_.isValid() && hdrTarget_.isValid()) {
        std::array<rhi::DescriptorWrite, 2> writes{
            rhi::DescriptorWrite::makeCombinedImageSampler(0, depthTarget_, pointClampSampler_),
            rhi::DescriptorWrite::makeCombinedImageSampler(1, hdrTarget_, linearClampSampler_),
        };
        std::ignore = device_->updateDescriptorSet(ssrDescriptorSet_, writes);
    }

    // 6. Milestone 11G: SSR Temporal Filter Descriptor Set: Binding 0 = SsrRawTarget, Binding 1 =
    // HistoryTarget, Binding 2 = DepthTarget
    rhi::TextureHandle ssrHistTex =
        ssrHistoryTarget_[prevSlot].isValid() ? ssrHistoryTarget_[prevSlot] : ssrRawTarget_;
    if (ssrTemporalFilterDescriptorSet_.isValid() && ssrRawTarget_.isValid() &&
        ssrHistTex.isValid() && depthTarget_.isValid()) {
        std::array<rhi::DescriptorWrite, 3> writes{
            rhi::DescriptorWrite::makeCombinedImageSampler(0, ssrRawTarget_, linearClampSampler_),
            rhi::DescriptorWrite::makeCombinedImageSampler(1, ssrHistTex, linearClampSampler_),
            rhi::DescriptorWrite::makeCombinedImageSampler(2, depthTarget_, pointClampSampler_),
        };
        std::ignore = device_->updateDescriptorSet(ssrTemporalFilterDescriptorSet_, writes);
    }
}

void SceneRenderer::recordGtaoPass(rhi::RhiCommandList& cmdList) {
    if (!gtaoPipeline_.isValid() || !gtaoDescriptorSet_.isValid() || !gtaoRawTarget_.isValid()) {
        return;
    }

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 14, rhi::GpuTimestampStage::TopOfPipe);
    }

    const uint32_t halfW = std::max(1u, width_ / 2);
    const uint32_t halfH = std::max(1u, height_ / 2);

    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = gtaoRawTarget_,
        .depthTarget = {},
        .clearColor = {1.0F, 1.0F, 1.0F, 1.0F},
        .clearDepth = 1.0F,
        .clearTarget = true,
        .clearDepthTarget = false,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(halfW), static_cast<float>(halfH));
    cmdList.setScissor(0, 0, halfW, halfH);

    cmdList.bindPipeline(gtaoPipeline_);
    const std::array<rhi::DescriptorSetHandle, 1> sets{gtaoDescriptorSet_};
    cmdList.bindDescriptorSets(gtaoPipelineLayout_, 0, sets);

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_ > 0 ? height_ : 1);
    const float fovY = 1.04719755F;
    const float tanHalfFov = std::tan(fovY * 0.5F);
    const float f = (tanHalfFov > 1e-6F) ? (1.0F / tanHalfFov) : 1.0F;
    const float P00 = f / (aspect > 1e-6F ? aspect : 1.0F);
    const float P11 = f;

    GtaoPushConstants pc{};
    pc.lightDirAndContact = Vec4(currentLightDir_.x, currentLightDir_.y, currentLightDir_.z,
                                 settings_.shadows.contactShadowDistance);
    pc.cameraPosAndRadius = Vec4(currentCameraPos_.x, currentCameraPos_.y, currentCameraPos_.z,
                                 settings_.lighting.gtaoRadius);
    pc.gtaoParams = Vec4(settings_.lighting.gtaoIntensity, 0.05F, 1.0F,
                         settings_.shadows.contactShadows ? 1.0F : 0.0F);
    pc.screenAndProj = Vec4(static_cast<float>(halfW), static_cast<float>(halfH), P00, P11);
    pc.invViewProj = currentViewProj_.inverse().value_or(Mat4::identity());

    cmdList.pushConstants(gtaoPipelineLayout_, rhi::ShaderStageFlags::Fragment, 0,
                          sizeof(GtaoPushConstants), &pc);
    cmdList.draw(3, 1, 0, 0);
    cmdList.endRenderPass();
}

void SceneRenderer::recordGtaoBlurPass(rhi::RhiCommandList& cmdList) {
    if (!gtaoBlurPipeline_.isValid() || !gtaoBlurDescriptorSet_.isValid() ||
        !gtaoFilteredTarget_.isValid()) {
        return;
    }

    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = gtaoFilteredTarget_,
        .depthTarget = {},
        .clearColor = {1.0F, 1.0F, 1.0F, 1.0F},
        .clearDepth = 1.0F,
        .clearTarget = true,
        .clearDepthTarget = false,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_));
    cmdList.setScissor(0, 0, width_, height_);

    cmdList.bindPipeline(gtaoBlurPipeline_);
    const std::array<rhi::DescriptorSetHandle, 1> sets{gtaoBlurDescriptorSet_};
    cmdList.bindDescriptorSets(gtaoBlurPipelineLayout_, 0, sets);

    BilateralBlurPushConstants pc{};
    pc.blurDirection = Vec2(1.0F, 0.0F);
    pc.invResolution = Vec2(1.0F / static_cast<float>(width_), 1.0F / static_cast<float>(height_));
    pc.depthThreshold = 0.05F;
    pc.pad0 = 0.0F;

    cmdList.pushConstants(gtaoBlurPipelineLayout_, rhi::ShaderStageFlags::Fragment, 0,
                          sizeof(BilateralBlurPushConstants), &pc);
    cmdList.draw(3, 1, 0, 0);
    cmdList.endRenderPass();

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 15, rhi::GpuTimestampStage::BottomOfPipe);
    }
}

void SceneRenderer::recordSsgiPass(rhi::RhiCommandList& cmdList) {
    if (!ssgiPipeline_.isValid() || !ssgiDescriptorSet_.isValid() || !ssgiRawTarget_.isValid()) {
        return;
    }

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 16, rhi::GpuTimestampStage::TopOfPipe);
    }

    const uint32_t halfW = std::max(1u, width_ / 2);
    const uint32_t halfH = std::max(1u, height_ / 2);

    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = ssgiRawTarget_,
        .depthTarget = {},
        .clearColor = {0.0F, 0.0F, 0.0F, 0.0F},
        .clearDepth = 1.0F,
        .clearTarget = true,
        .clearDepthTarget = false,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(halfW), static_cast<float>(halfH));
    cmdList.setScissor(0, 0, halfW, halfH);

    cmdList.bindPipeline(ssgiPipeline_);
    const std::array<rhi::DescriptorSetHandle, 1> sets{ssgiDescriptorSet_};
    cmdList.bindDescriptorSets(ssgiPipelineLayout_, 0, sets);

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_ > 0 ? height_ : 1);
    const float fovY = 1.04719755F;
    const float tanHalfFov = std::tan(fovY * 0.5F);
    const float f = (tanHalfFov > 1e-6F) ? (1.0F / tanHalfFov) : 1.0F;
    const float zNear = 0.1F;
    const float zFar = 1000.0F;
    const float P00 = f / (aspect > 1e-6F ? aspect : 1.0F);
    const float P11 = f;
    const float P22 = zFar / (zNear - zFar);
    const float P23 = (zNear * zFar) / (zNear - zFar);

    SsgiPushConstants pc{};
    auto invOpt = currentViewProj_.inverse();
    pc.reprojectMat = prevViewProj_ * invOpt.value_or(Mat4::identity());
    pc.projParams = Vec4(P00, P11, P22, P23);
    pc.ssgiParams = Vec4(settings_.lighting.giIntensity, 8.0F, 0.2F, settings_.lighting.ssgiRadius);
    pc.resolution = Vec2(static_cast<float>(halfW), static_cast<float>(halfH));
    pc.invResolution = Vec2(1.0F / static_cast<float>(halfW), 1.0F / static_cast<float>(halfH));
    pc.frameIndex = giFrameIndex_;
    pc.pad0 = 0.0F;

    cmdList.pushConstants(ssgiPipelineLayout_, rhi::ShaderStageFlags::Fragment, 0,
                          sizeof(SsgiPushConstants), &pc);
    cmdList.draw(3, 1, 0, 0);
    cmdList.endRenderPass();

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 17, rhi::GpuTimestampStage::BottomOfPipe);
    }
}

void SceneRenderer::recordTemporalFilterPass(rhi::RhiCommandList& cmdList) {
    if (!temporalFilterPipeline_.isValid() || !temporalFilterDescriptorSet_.isValid() ||
        !ssgiFilteredTarget_.isValid()) {
        return;
    }

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 18, rhi::GpuTimestampStage::TopOfPipe);
    }

    const uint32_t halfW = std::max(1u, width_ / 2);
    const uint32_t halfH = std::max(1u, height_ / 2);
    const uint32_t currentSlot = giFrameIndex_ % 2;
    rhi::TextureHandle outTarget = ssgiHistoryTarget_[currentSlot].isValid()
                                       ? ssgiHistoryTarget_[currentSlot]
                                       : ssgiFilteredTarget_;

    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = outTarget,
        .depthTarget = {},
        .clearColor = {0.0F, 0.0F, 0.0F, 0.0F},
        .clearDepth = 1.0F,
        .clearTarget = true,
        .clearDepthTarget = false,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(halfW), static_cast<float>(halfH));
    cmdList.setScissor(0, 0, halfW, halfH);

    cmdList.bindPipeline(temporalFilterPipeline_);
    const std::array<rhi::DescriptorSetHandle, 1> sets{temporalFilterDescriptorSet_};
    cmdList.bindDescriptorSets(temporalFilterPipelineLayout_, 0, sets);

    TemporalFilterPushConstants pc{};
    auto invOpt = currentViewProj_.inverse();
    pc.reprojectMat = prevViewProj_ * invOpt.value_or(Mat4::identity());
    pc.filterParams = Vec4(settings_.lighting.temporalDenoise ? 0.1F : 1.0F, 0.05F, 0.0F, 0.0F);
    pc.resolution = Vec2(static_cast<float>(halfW), static_cast<float>(halfH));
    pc.invResolution = Vec2(1.0F / static_cast<float>(halfW), 1.0F / static_cast<float>(halfH));
    pc.isHistoryValid = isHistoryValid_ ? 1u : 0u;
    pc.pad0 = 0.0F;

    cmdList.pushConstants(temporalFilterPipelineLayout_, rhi::ShaderStageFlags::Fragment, 0,
                          sizeof(TemporalFilterPushConstants), &pc);
    cmdList.draw(3, 1, 0, 0);
    cmdList.endRenderPass();

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 19, rhi::GpuTimestampStage::BottomOfPipe);
    }
}

void SceneRenderer::recordSsrPass(rhi::RhiCommandList& cmdList) {
    if (!ssrPipeline_.isValid() || !ssrDescriptorSet_.isValid() || !ssrRawTarget_.isValid()) {
        return;
    }

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 20, rhi::GpuTimestampStage::TopOfPipe);
    }

    const uint32_t halfW = std::max(1u, width_ / 2);
    const uint32_t halfH = std::max(1u, height_ / 2);

    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = ssrRawTarget_,
        .depthTarget = {},
        .clearColor = {0.0F, 0.0F, 0.0F, 0.0F},
        .clearDepth = 1.0F,
        .clearTarget = true,
        .clearDepthTarget = false,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(halfW), static_cast<float>(halfH));
    cmdList.setScissor(0, 0, halfW, halfH);

    cmdList.bindPipeline(ssrPipeline_);
    const std::array<rhi::DescriptorSetHandle, 1> sets{ssrDescriptorSet_};
    cmdList.bindDescriptorSets(ssrPipelineLayout_, 0, sets);

    const float aspect = static_cast<float>(width_) / static_cast<float>(height_ > 0 ? height_ : 1);
    const float fovY = 1.04719755F;
    const float tanHalfFov = std::tan(fovY * 0.5F);
    const float f = (tanHalfFov > 1e-6F) ? (1.0F / tanHalfFov) : 1.0F;
    const float zNear = 0.1F;
    const float zFar = 1000.0F;
    const float P00 = f / (aspect > 1e-6F ? aspect : 1.0F);
    const float P11 = f;
    const float P22 = zFar / (zNear - zFar);
    const float P23 = (zNear * zFar) / (zNear - zFar);

    SsrPushConstants pc{};
    auto invOpt = currentViewProj_.inverse();
    pc.reprojectMat = prevViewProj_ * invOpt.value_or(Mat4::identity());
    pc.projParams = Vec4(P00, P11, P22, P23);
    pc.ssrParams = Vec4(settings_.lighting.ssrMaxDistance, 16.0F, 0.25F,
                        settings_.lighting.ssrRoughnessCutoff);
    pc.cameraPosAndIntensity = Vec4(currentCameraPos_.x, currentCameraPos_.y, currentCameraPos_.z,
                                    settings_.lighting.ssrIntensity);
    pc.resolution = Vec2(static_cast<float>(halfW), static_cast<float>(halfH));
    pc.invResolution = Vec2(1.0F / static_cast<float>(halfW), 1.0F / static_cast<float>(halfH));

    cmdList.pushConstants(ssrPipelineLayout_, rhi::ShaderStageFlags::Fragment, 0,
                          sizeof(SsrPushConstants), &pc);
    cmdList.draw(3, 1, 0, 0);
    cmdList.endRenderPass();

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 21, rhi::GpuTimestampStage::BottomOfPipe);
    }
}

void SceneRenderer::recordSsrTemporalFilterPass(rhi::RhiCommandList& cmdList) {
    if (!temporalFilterPipeline_.isValid() || !ssrTemporalFilterDescriptorSet_.isValid() ||
        !ssrFilteredTarget_.isValid()) {
        return;
    }

    const uint32_t halfW = std::max(1u, width_ / 2);
    const uint32_t halfH = std::max(1u, height_ / 2);
    const uint32_t currentSlot = giFrameIndex_ % 2;
    rhi::TextureHandle outTarget = ssrHistoryTarget_[currentSlot].isValid()
                                       ? ssrHistoryTarget_[currentSlot]
                                       : ssrFilteredTarget_;

    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = outTarget,
        .depthTarget = {},
        .clearColor = {0.0F, 0.0F, 0.0F, 0.0F},
        .clearDepth = 1.0F,
        .clearTarget = true,
        .clearDepthTarget = false,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(halfW), static_cast<float>(halfH));
    cmdList.setScissor(0, 0, halfW, halfH);

    cmdList.bindPipeline(temporalFilterPipeline_);
    const std::array<rhi::DescriptorSetHandle, 1> sets{ssrTemporalFilterDescriptorSet_};
    cmdList.bindDescriptorSets(temporalFilterPipelineLayout_, 0, sets);

    TemporalFilterPushConstants pc{};
    auto invOpt = currentViewProj_.inverse();
    pc.reprojectMat = prevViewProj_ * invOpt.value_or(Mat4::identity());
    pc.filterParams = Vec4(settings_.lighting.temporalDenoise ? 0.15F : 1.0F, 0.05F, 0.0F, 0.0F);
    pc.resolution = Vec2(static_cast<float>(halfW), static_cast<float>(halfH));
    pc.invResolution = Vec2(1.0F / static_cast<float>(halfW), 1.0F / static_cast<float>(halfH));
    pc.isHistoryValid = (isHistoryValid_ && settings_.lighting.temporalDenoise) ? 1u : 0u;
    pc.pad0 = 0.0F;

    cmdList.pushConstants(temporalFilterPipelineLayout_, rhi::ShaderStageFlags::Fragment, 0,
                          sizeof(TemporalFilterPushConstants), &pc);
    cmdList.draw(3, 1, 0, 0);
    cmdList.endRenderPass();
}

void SceneRenderer::recordHistoryCopyPass(rhi::RhiCommandList& /*cmdList*/) {}

Result<void> SceneRenderer::initTaaResources() {
    if (device_ == nullptr) {
        return Error{.code = ErrorCode::InvalidState, .message = "Device not initialized"};
    }

    // 1. Shaders
    rhi::ShaderDesc velFsDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kVelocityFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "VelocityFragmentShader",
    };
    auto velFsRes = device_->createShader(velFsDesc);
    if (!velFsRes.hasValue()) {
        return velFsRes.error();
    }
    velocityFragmentShader_ = velFsRes.value();

    rhi::ShaderDesc meshVelVsDesc{
        .stage = rhi::ShaderStageFlags::Vertex,
        .spirvCode = kMeshVelocityVertexShaderSpirv,
        .entryPoint = "main",
        .debugName = "MeshVelocityVertexShader",
    };
    auto meshVelVsRes = device_->createShader(meshVelVsDesc);
    if (!meshVelVsRes.hasValue()) {
        return meshVelVsRes.error();
    }
    meshVelocityVertexShader_ = meshVelVsRes.value();

    rhi::ShaderDesc meshVelFsDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kMeshVelocityFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "MeshVelocityFragmentShader",
    };
    auto meshVelFsRes = device_->createShader(meshVelFsDesc);
    if (!meshVelFsRes.hasValue()) {
        return meshVelFsRes.error();
    }
    meshVelocityFragmentShader_ = meshVelFsRes.value();

    rhi::ShaderDesc taaFsDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kTaaFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "TaaFragmentShader",
    };
    auto taaFsRes = device_->createShader(taaFsDesc);
    if (!taaFsRes.hasValue()) {
        return taaFsRes.error();
    }
    taaFragmentShader_ = taaFsRes.value();

    rhi::ShaderDesc rcasFsDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kSharpenFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "SharpenFragmentShader",
    };
    auto rcasFsRes = device_->createShader(rcasFsDesc);
    if (!rcasFsRes.hasValue()) {
        return rcasFsRes.error();
    }
    sharpenFragmentShader_ = rcasFsRes.value();

    // 2. Descriptor set layouts
    rhi::DescriptorSetLayoutDesc velLayoutDesc{
        .bindings =
            {
                {
                    .binding = 0,
                    .descriptorType = rhi::DescriptorType::CombinedImageSampler,
                    .descriptorCount = 1,
                    .stageFlags = rhi::ShaderStageFlags::Fragment,
                },
            },
        .debugName = "VelocityDescriptorSetLayout",
    };
    auto velLayoutRes = device_->createDescriptorSetLayout(velLayoutDesc);
    if (!velLayoutRes.hasValue()) {
        return velLayoutRes.error();
    }
    velocitySetLayout_ = velLayoutRes.value();

    rhi::DescriptorSetLayoutDesc taaLayoutDesc{
        .bindings =
            {
                {
                    .binding = 0,
                    .descriptorType = rhi::DescriptorType::CombinedImageSampler,
                    .descriptorCount = 1,
                    .stageFlags = rhi::ShaderStageFlags::Fragment,
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
            },
        .debugName = "TaaDescriptorSetLayout",
    };
    auto taaLayoutRes = device_->createDescriptorSetLayout(taaLayoutDesc);
    if (!taaLayoutRes.hasValue()) {
        return taaLayoutRes.error();
    }
    taaSetLayout_ = taaLayoutRes.value();

    rhi::DescriptorSetLayoutDesc sharpenLayoutDesc{
        .bindings =
            {
                {
                    .binding = 0,
                    .descriptorType = rhi::DescriptorType::CombinedImageSampler,
                    .descriptorCount = 1,
                    .stageFlags = rhi::ShaderStageFlags::Fragment,
                },
            },
        .debugName = "SharpenDescriptorSetLayout",
    };
    auto sharpenLayoutRes = device_->createDescriptorSetLayout(sharpenLayoutDesc);
    if (!sharpenLayoutRes.hasValue()) {
        return sharpenLayoutRes.error();
    }
    sharpenSetLayout_ = sharpenLayoutRes.value();

    // 3. Descriptor Pool
    rhi::DescriptorPoolDesc poolDesc{
        .maxSets = 8,
        .poolSizes =
            {
                {.type = rhi::DescriptorType::CombinedImageSampler, .count = 32},
            },
        .debugName = "TaaDescriptorPool",
    };
    auto poolRes = device_->createDescriptorPool(poolDesc);
    if (!poolRes.hasValue()) {
        return poolRes.error();
    }
    taaDescriptorPool_ = poolRes.value();

    auto velSetRes = device_->allocateDescriptorSet(taaDescriptorPool_, velocitySetLayout_);
    if (!velSetRes.hasValue()) {
        return velSetRes.error();
    }
    velocityDescriptorSet_ = velSetRes.value();

    for (size_t i = 0; i < 2; ++i) {
        auto taaSetRes = device_->allocateDescriptorSet(taaDescriptorPool_, taaSetLayout_);
        if (!taaSetRes.hasValue()) {
            return taaSetRes.error();
        }
        taaDescriptorSet_[i] = taaSetRes.value();

        auto shSetRes = device_->allocateDescriptorSet(taaDescriptorPool_, sharpenSetLayout_);
        if (!shSetRes.hasValue()) {
            return shSetRes.error();
        }
        sharpenDescriptorSet_[i] = shSetRes.value();
    }

    // 4. Pipeline Layouts
    rhi::PipelineLayoutDesc velPipeLayoutDesc{
        .pushConstants =
            {
                {
                    .stageFlags = rhi::ShaderStageFlags::Fragment,
                    .offset = 0,
                    .size = sizeof(VelocityPushConstants),
                },
            },
        .descriptorSetLayoutHandles = {velocitySetLayout_},
        .descriptorSetLayouts = {},
        .debugName = "VelocityPipelineLayout",
    };
    auto velPipeLayoutRes = device_->createPipelineLayout(velPipeLayoutDesc);
    if (!velPipeLayoutRes.hasValue()) {
        return velPipeLayoutRes.error();
    }
    velocityPipelineLayout_ = velPipeLayoutRes.value();

    rhi::PipelineLayoutDesc meshVelPipeLayoutDesc{
        .pushConstants =
            {
                {
                    .stageFlags = rhi::ShaderStageFlags::Vertex | rhi::ShaderStageFlags::Fragment,
                    .offset = 0,
                    .size = sizeof(MeshVelocityPushConstants),
                },
            },
        .descriptorSetLayoutHandles = {},
        .descriptorSetLayouts = {},
        .debugName = "MeshVelocityPipelineLayout",
    };
    auto meshVelPipeLayoutRes = device_->createPipelineLayout(meshVelPipeLayoutDesc);
    if (!meshVelPipeLayoutRes.hasValue()) {
        return meshVelPipeLayoutRes.error();
    }
    meshVelocityPipelineLayout_ = meshVelPipeLayoutRes.value();

    rhi::PipelineLayoutDesc taaPipeLayoutDesc{
        .pushConstants =
            {
                {
                    .stageFlags = rhi::ShaderStageFlags::Fragment,
                    .offset = 0,
                    .size = sizeof(TaaPushConstants),
                },
            },
        .descriptorSetLayoutHandles = {taaSetLayout_},
        .descriptorSetLayouts = {},
        .debugName = "TaaPipelineLayout",
    };
    auto taaPipeLayoutRes = device_->createPipelineLayout(taaPipeLayoutDesc);
    if (!taaPipeLayoutRes.hasValue()) {
        return taaPipeLayoutRes.error();
    }
    taaPipelineLayout_ = taaPipeLayoutRes.value();

    rhi::PipelineLayoutDesc sharpenPipeLayoutDesc{
        .pushConstants =
            {
                {
                    .stageFlags = rhi::ShaderStageFlags::Fragment,
                    .offset = 0,
                    .size = sizeof(SharpenPushConstants),
                },
            },
        .descriptorSetLayoutHandles = {sharpenSetLayout_},
        .descriptorSetLayouts = {},
        .debugName = "SharpenPipelineLayout",
    };
    auto sharpenPipeLayoutRes = device_->createPipelineLayout(sharpenPipeLayoutDesc);
    if (!sharpenPipeLayoutRes.hasValue()) {
        return sharpenPipeLayoutRes.error();
    }
    sharpenPipelineLayout_ = sharpenPipeLayoutRes.value();

    // 5. Graphics Pipelines
    rhi::GraphicsPipelineDesc velPipeDesc{
        .vertexShader = fullscreenVertexShader_,
        .fragmentShader = velocityFragmentShader_,
        .vertexInput = {},
        .topology = rhi::PrimitiveTopology::TriangleList,
        .rasterization =
            {
                .polygonMode = rhi::PolygonMode::Fill,
                .cullMode = rhi::CullMode::None,
                .frontFace = rhi::FrontFace::CounterClockwise,
            },
        .depthStencil =
            {
                .depthTestEnable = false,
                .depthWriteEnable = false,
            },
        .colorBlendAttachments = {{
            .blendEnable = false,
            .colorWriteMask = rhi::ColorComponentFlags::All,
        }},
        .layout = velocityPipelineLayout_,
        .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_FLOAT},
        .debugName = "VelocityGraphicsPipeline",
    };
    auto velPipeRes = device_->createGraphicsPipeline(velPipeDesc);
    if (!velPipeRes.hasValue()) {
        return velPipeRes.error();
    }
    velocityPipeline_ = velPipeRes.value();

    rhi::GraphicsPipelineDesc meshVelPipeDesc{
        .vertexShader = meshVelocityVertexShader_,
        .fragmentShader = meshVelocityFragmentShader_,
        .vertexInput =
            {
                .bindings =
                    {
                        {
                            .binding = 0,
                            .stride = sizeof(Vertex),
                            .inputRate = rhi::VertexInputRate::Vertex,
                        },
                    },
                .attributes =
                    {
                        {
                            .location = 0,
                            .binding = 0,
                            .format = rhi::VertexFormat::Float3,
                            .offset = static_cast<uint32_t>(offsetof(Vertex, position)),
                        },
                    },
            },
        .topology = rhi::PrimitiveTopology::TriangleList,
        .rasterization =
            {
                .polygonMode = rhi::PolygonMode::Fill,
                .cullMode = rhi::CullMode::Back,
                .frontFace = rhi::FrontFace::CounterClockwise,
            },
        .depthStencil =
            {
                .depthTestEnable = true,
                .depthWriteEnable = false,
                .depthCompareOp = rhi::CompareOp::LessOrEqual,
            },
        .colorBlendAttachments = {{
            .blendEnable = false,
            .colorWriteMask = rhi::ColorComponentFlags::All,
        }},
        .layout = meshVelocityPipelineLayout_,
        .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_FLOAT},
        .depthAttachmentFormat = depthFormat_,
        .debugName = "MeshVelocityGraphicsPipeline",
    };
    auto meshVelPipeRes = device_->createGraphicsPipeline(meshVelPipeDesc);
    if (!meshVelPipeRes.hasValue()) {
        return meshVelPipeRes.error();
    }
    meshVelocityPipeline_ = meshVelPipeRes.value();

    rhi::GraphicsPipelineDesc taaPipeDesc{
        .vertexShader = fullscreenVertexShader_,
        .fragmentShader = taaFragmentShader_,
        .vertexInput = {},
        .topology = rhi::PrimitiveTopology::TriangleList,
        .rasterization =
            {
                .polygonMode = rhi::PolygonMode::Fill,
                .cullMode = rhi::CullMode::None,
                .frontFace = rhi::FrontFace::CounterClockwise,
            },
        .depthStencil =
            {
                .depthTestEnable = false,
                .depthWriteEnable = false,
            },
        .colorBlendAttachments = {{
            .blendEnable = false,
            .colorWriteMask = rhi::ColorComponentFlags::All,
        }},
        .layout = taaPipelineLayout_,
        .colorAttachmentFormats = {hdrFormat_},
        .debugName = "TaaGraphicsPipeline",
    };
    auto taaPipeRes = device_->createGraphicsPipeline(taaPipeDesc);
    if (!taaPipeRes.hasValue()) {
        return taaPipeRes.error();
    }
    taaPipeline_ = taaPipeRes.value();

    rhi::GraphicsPipelineDesc sharpenPipeDesc{
        .vertexShader = fullscreenVertexShader_,
        .fragmentShader = sharpenFragmentShader_,
        .vertexInput = {},
        .topology = rhi::PrimitiveTopology::TriangleList,
        .rasterization =
            {
                .polygonMode = rhi::PolygonMode::Fill,
                .cullMode = rhi::CullMode::None,
                .frontFace = rhi::FrontFace::CounterClockwise,
            },
        .depthStencil =
            {
                .depthTestEnable = false,
                .depthWriteEnable = false,
            },
        .colorBlendAttachments = {{
            .blendEnable = false,
            .colorWriteMask = rhi::ColorComponentFlags::All,
        }},
        .layout = sharpenPipelineLayout_,
        .colorAttachmentFormats = {hdrFormat_},
        .debugName = "SharpenGraphicsPipeline",
    };
    auto sharpenPipeRes = device_->createGraphicsPipeline(sharpenPipeDesc);
    if (!sharpenPipeRes.hasValue()) {
        return sharpenPipeRes.error();
    }
    sharpenPipeline_ = sharpenPipeRes.value();

    return Result<void>::success();
}

void SceneRenderer::destroyTaaResources() {
    if (device_ != nullptr) {
        destroyTaaTargets();

        if (sharpenPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(sharpenPipeline_);
            sharpenPipeline_ = {};
        }
        if (taaPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(taaPipeline_);
            taaPipeline_ = {};
        }
        if (meshVelocityPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(meshVelocityPipeline_);
            meshVelocityPipeline_ = {};
        }
        if (velocityPipeline_.isValid()) {
            device_->destroyGraphicsPipeline(velocityPipeline_);
            velocityPipeline_ = {};
        }

        if (sharpenPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(sharpenPipelineLayout_);
            sharpenPipelineLayout_ = {};
        }
        if (taaPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(taaPipelineLayout_);
            taaPipelineLayout_ = {};
        }
        if (meshVelocityPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(meshVelocityPipelineLayout_);
            meshVelocityPipelineLayout_ = {};
        }
        if (velocityPipelineLayout_.isValid()) {
            device_->destroyPipelineLayout(velocityPipelineLayout_);
            velocityPipelineLayout_ = {};
        }

        if (taaDescriptorPool_.isValid()) {
            device_->destroyDescriptorPool(taaDescriptorPool_);
            taaDescriptorPool_ = {};
            velocityDescriptorSet_ = {};
            taaDescriptorSet_[0] = {};
            taaDescriptorSet_[1] = {};
            sharpenDescriptorSet_[0] = {};
            sharpenDescriptorSet_[1] = {};
        }

        if (sharpenSetLayout_.isValid()) {
            device_->destroyDescriptorSetLayout(sharpenSetLayout_);
            sharpenSetLayout_ = {};
        }
        if (taaSetLayout_.isValid()) {
            device_->destroyDescriptorSetLayout(taaSetLayout_);
            taaSetLayout_ = {};
        }
        if (velocitySetLayout_.isValid()) {
            device_->destroyDescriptorSetLayout(velocitySetLayout_);
            velocitySetLayout_ = {};
        }

        if (sharpenFragmentShader_.isValid()) {
            device_->destroyShader(sharpenFragmentShader_);
            sharpenFragmentShader_ = {};
        }
        if (taaFragmentShader_.isValid()) {
            device_->destroyShader(taaFragmentShader_);
            taaFragmentShader_ = {};
        }
        if (meshVelocityFragmentShader_.isValid()) {
            device_->destroyShader(meshVelocityFragmentShader_);
            meshVelocityFragmentShader_ = {};
        }
        if (meshVelocityVertexShader_.isValid()) {
            device_->destroyShader(meshVelocityVertexShader_);
            meshVelocityVertexShader_ = {};
        }
        if (velocityFragmentShader_.isValid()) {
            device_->destroyShader(velocityFragmentShader_);
            velocityFragmentShader_ = {};
        }
    }
}

Result<void> SceneRenderer::createTaaTargets(uint32_t width, uint32_t height) {
    if (device_ == nullptr || width == 0 || height == 0) {
        return Result<void>::success();
    }

    rhi::TextureDesc velDesc{
        .type = rhi::TextureType::Texture2D,
        .format = rhi::TextureFormat::RGBA16_FLOAT,
        .width = width,
        .height = height,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = rhi::TextureUsageFlags::ColorAttachment | rhi::TextureUsageFlags::Sampled |
                 rhi::TextureUsageFlags::TransferSrc,
        .debugName = "SceneRendererVelocityTarget",
    };
    auto velRes = device_->createTexture(velDesc);
    if (!velRes.hasValue()) {
        return velRes.error();
    }
    velocityTarget_ = velRes.value();

    rhi::TextureDesc rawHdrDesc{
        .type = rhi::TextureType::Texture2D,
        .format = hdrFormat_,
        .width = width,
        .height = height,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = rhi::TextureUsageFlags::ColorAttachment | rhi::TextureUsageFlags::Sampled |
                 rhi::TextureUsageFlags::TransferSrc,
        .debugName = "SceneRendererHdrRawTarget",
    };
    auto rawRes = device_->createTexture(rawHdrDesc);
    if (!rawRes.hasValue()) {
        return rawRes.error();
    }
    hdrRawTarget_ = rawRes.value();

    for (size_t i = 0; i < 2; ++i) {
        auto histDesc = rawHdrDesc;
        histDesc.debugName = (i == 0) ? "SceneRendererHdrHistory0" : "SceneRendererHdrHistory1";
        auto histRes = device_->createTexture(histDesc);
        if (!histRes.hasValue()) {
            return histRes.error();
        }
        hdrHistoryTarget_[i] = histRes.value();
    }

    updateTaaDescriptors();
    updatePostProcessDescriptors();
    return Result<void>::success();
}

void SceneRenderer::destroyTaaTargets() {
    if (device_ != nullptr) {
        if (velocityTarget_.isValid()) {
            device_->destroyTexture(velocityTarget_);
            velocityTarget_ = {};
        }
        if (hdrRawTarget_.isValid()) {
            device_->destroyTexture(hdrRawTarget_);
            hdrRawTarget_ = {};
        }
        for (size_t i = 0; i < 2; ++i) {
            if (hdrHistoryTarget_[i].isValid()) {
                device_->destroyTexture(hdrHistoryTarget_[i]);
                hdrHistoryTarget_[i] = {};
            }
        }
    }
}

void SceneRenderer::updateTaaDescriptors() {
    if (device_ == nullptr) {
        return;
    }

    const rhi::SamplerHandle pointSamp =
        pointClampSampler_.isValid() ? pointClampSampler_ : shadowSampler_;
    const rhi::SamplerHandle linSamp =
        linearClampSampler_.isValid() ? linearClampSampler_ : shadowSampler_;

    if (velocityDescriptorSet_.isValid() && depthTarget_.isValid() && pointSamp.isValid()) {
        std::array<rhi::DescriptorWrite, 1> writes{
            rhi::DescriptorWrite::makeCombinedImageSampler(0, depthTarget_, pointSamp),
        };
        std::ignore = device_->updateDescriptorSet(velocityDescriptorSet_, writes);
    }

    if (hdrRawTarget_.isValid() && velocityTarget_.isValid() && depthTarget_.isValid() &&
        linSamp.isValid() && pointSamp.isValid()) {
        for (size_t i = 0; i < 2; ++i) {
            if (taaDescriptorSet_[i].isValid()) {
                const uint32_t prevIdx = (i == 0) ? 1 : 0;
                rhi::TextureHandle historyTex = hdrHistoryTarget_[prevIdx].isValid()
                                                    ? hdrHistoryTarget_[prevIdx]
                                                    : hdrRawTarget_;
                std::array<rhi::DescriptorWrite, 4> writes{
                    rhi::DescriptorWrite::makeCombinedImageSampler(0, hdrRawTarget_, linSamp),
                    rhi::DescriptorWrite::makeCombinedImageSampler(1, historyTex, linSamp),
                    rhi::DescriptorWrite::makeCombinedImageSampler(2, velocityTarget_, pointSamp),
                    rhi::DescriptorWrite::makeCombinedImageSampler(3, depthTarget_, pointSamp),
                };
                std::ignore = device_->updateDescriptorSet(taaDescriptorSet_[i], writes);
            }
        }
    }

    if (linSamp.isValid()) {
        for (size_t i = 0; i < 2; ++i) {
            if (sharpenDescriptorSet_[i].isValid() && hdrHistoryTarget_[i].isValid()) {
                std::array<rhi::DescriptorWrite, 1> writes{
                    rhi::DescriptorWrite::makeCombinedImageSampler(0, hdrHistoryTarget_[i],
                                                                   linSamp),
                };
                std::ignore = device_->updateDescriptorSet(sharpenDescriptorSet_[i], writes);
            }
        }
    }
}

void SceneRenderer::recordVelocityPass(rhi::RhiCommandList& cmdList) {
    if (!velocityPipeline_.isValid() || !velocityDescriptorSet_.isValid() ||
        !velocityTarget_.isValid()) {
        return;
    }

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 24, rhi::GpuTimestampStage::TopOfPipe);
    }

    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = velocityTarget_,
        .depthTarget = {},
        .clearColor = {0.0F, 0.0F, 1.0F, 0.0F},
        .clearDepth = 1.0F,
        .clearTarget = true,
        .clearDepthTarget = false,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_));
    cmdList.setScissor(0, 0, width_, height_);

    cmdList.bindPipeline(velocityPipeline_);
    const std::array<rhi::DescriptorSetHandle, 1> sets{velocityDescriptorSet_};
    cmdList.bindDescriptorSets(velocityPipelineLayout_, 0, sets);

    Mat4 invViewProj = unjitteredViewProj_.inverse().value_or(Mat4::identity());
    VelocityPushConstants pc{
        .reprojectMatrix = unjitteredPrevViewProj_ * invViewProj,
        .resolution = Vec2(static_cast<float>(width_), static_cast<float>(height_)),
        .invResolution =
            Vec2(1.0F / static_cast<float>(width_), 1.0F / static_cast<float>(height_)),
        .hasHistory = (isTaaHistoryValid_ && isHistoryValid_) ? 1U : 0U,
        .pad0 = 0.0F,
        .pad1 = 0.0F,
        .pad2 = 0.0F,
    };
    cmdList.pushConstants(velocityPipelineLayout_, rhi::ShaderStageFlags::Fragment, 0,
                          sizeof(VelocityPushConstants), &pc);
    cmdList.draw(3, 1, 0, 0);
    cmdList.endRenderPass();

    bool hasMovingMeshes = false;
    for (const auto& item : visibleItems_) {
        if (item.hasValidPrevTransform && !item.model.isNearlyEqual(item.prevModel)) {
            hasMovingMeshes = true;
            break;
        }
    }

    if (hasMovingMeshes && meshVelocityPipeline_.isValid() && depthTarget_.isValid()) {
        rhi::RenderPassBeginInfo meshPassInfo{
            .colorTarget = velocityTarget_,
            .depthTarget = depthTarget_,
            .clearColor = {0.0F, 0.0F, 0.0F, 0.0F},
            .clearDepth = 1.0F,
            .clearTarget = false,
            .clearDepthTarget = false,
        };
        cmdList.beginRenderPass(meshPassInfo);
        cmdList.setViewport(0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_));
        cmdList.setScissor(0, 0, width_, height_);
        cmdList.bindPipeline(meshVelocityPipeline_);

        for (const auto& item : visibleItems_) {
            if (!item.hasValidPrevTransform || item.model.isNearlyEqual(item.prevModel)) {
                continue;
            }
            if (item.gpuMesh == nullptr || !item.gpuMesh->isValid()) {
                continue;
            }

            MeshVelocityPushConstants mpc{
                .curMvp = unjitteredViewProj_ * item.model,
                .prevMvp = unjitteredPrevViewProj_ * item.prevModel,
            };
            cmdList.pushConstants(meshVelocityPipelineLayout_,
                                  rhi::ShaderStageFlags::Vertex | rhi::ShaderStageFlags::Fragment,
                                  0, sizeof(MeshVelocityPushConstants), &mpc);

            cmdList.bindVertexBuffer(0, item.gpuMesh->getVertexBuffer(), 0);
            if (item.gpuMesh->getIndexBuffer().isValid() && item.gpuMesh->getIndexCount() > 0) {
                cmdList.bindIndexBuffer(item.gpuMesh->getIndexBuffer(), 0, true);
                const auto& submeshes = item.gpuMesh->getSubmeshes();
                if (item.submeshIndex < submeshes.size()) {
                    const auto& sub = submeshes[item.submeshIndex];
                    cmdList.drawIndexed(sub.indexCount, 1, sub.firstIndex, sub.vertexOffset, 0);
                } else {
                    cmdList.drawIndexed(item.gpuMesh->getIndexCount(), 1, 0, 0, 0);
                }
            } else if (item.gpuMesh->getVertexCount() > 0) {
                cmdList.draw(item.gpuMesh->getVertexCount(), 1, 0, 0);
            }
        }
        cmdList.endRenderPass();
    }

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 25, rhi::GpuTimestampStage::BottomOfPipe);
    }
}

void SceneRenderer::recordTaaPass(rhi::RhiCommandList& cmdList) {
    const uint32_t currSlot = frameSlotIndex_ % 2;
    if (!taaPipeline_.isValid() || !taaDescriptorSet_[currSlot].isValid() ||
        !hdrHistoryTarget_[currSlot].isValid()) {
        return;
    }

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 26, rhi::GpuTimestampStage::TopOfPipe);
    }

    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = hdrHistoryTarget_[currSlot],
        .depthTarget = {},
        .clearColor = {0.0F, 0.0F, 0.0F, 1.0F},
        .clearDepth = 1.0F,
        .clearTarget = false,
        .clearDepthTarget = false,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_));
    cmdList.setScissor(0, 0, width_, height_);

    cmdList.bindPipeline(taaPipeline_);
    const std::array<rhi::DescriptorSetHandle, 1> sets{taaDescriptorSet_[currSlot]};
    cmdList.bindDescriptorSets(taaPipelineLayout_, 0, sets);

    const auto& taa = settings_.quality.taa;
    TaaPushConstants pc{
        .taaParams = Vec4(taa.historyWeight, taa.varianceGamma, 0.0F, 0.0F),
        .resolution = Vec2(static_cast<float>(width_), static_cast<float>(height_)),
        .invResolution =
            Vec2(1.0F / static_cast<float>(width_), 1.0F / static_cast<float>(height_)),
        .isHistoryValid = isTaaHistoryValid_ ? 1U : 0U,
        .useMotionVectors = taa.useMotionVectors ? 1U : 0U,
        .pad0 = 0.0F,
        .pad1 = 0.0F,
    };
    cmdList.pushConstants(taaPipelineLayout_, rhi::ShaderStageFlags::Fragment, 0,
                          sizeof(TaaPushConstants), &pc);
    cmdList.draw(3, 1, 0, 0);
    cmdList.endRenderPass();

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 27, rhi::GpuTimestampStage::BottomOfPipe);
    }
}

void SceneRenderer::recordSharpenPass(rhi::RhiCommandList& cmdList) {
    const uint32_t currSlot = frameSlotIndex_ % 2;
    if (!sharpenPipeline_.isValid() || !sharpenDescriptorSet_[currSlot].isValid() ||
        !hdrTarget_.isValid()) {
        return;
    }

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 28, rhi::GpuTimestampStage::TopOfPipe);
    }

    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = hdrTarget_,
        .depthTarget = {},
        .clearColor = {0.0F, 0.0F, 0.0F, 1.0F},
        .clearDepth = 1.0F,
        .clearTarget = false,
        .clearDepthTarget = false,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_));
    cmdList.setScissor(0, 0, width_, height_);

    cmdList.bindPipeline(sharpenPipeline_);
    const std::array<rhi::DescriptorSetHandle, 1> sets{sharpenDescriptorSet_[currSlot]};
    cmdList.bindDescriptorSets(sharpenPipelineLayout_, 0, sets);

    const auto& taa = settings_.quality.taa;
    const float intensity = taa.sharpeningEnabled ? taa.sharpeningIntensity : 0.0F;
    SharpenPushConstants pc{
        .invResolution =
            Vec2(1.0F / static_cast<float>(width_), 1.0F / static_cast<float>(height_)),
        .intensity = intensity,
        .pad0 = 0.0F,
    };
    cmdList.pushConstants(sharpenPipelineLayout_, rhi::ShaderStageFlags::Fragment, 0,
                          sizeof(SharpenPushConstants), &pc);
    cmdList.draw(3, 1, 0, 0);
    cmdList.endRenderPass();

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 29, rhi::GpuTimestampStage::BottomOfPipe);
    }
}

Result<void> SceneRenderer::initPostProcessResources() {
    // 1. Post-process bilinear sampler with clamp-to-edge
    rhi::SamplerDesc sampDesc{
        .minFilter = rhi::Filter::Linear,
        .magFilter = rhi::Filter::Linear,
        .mipmapMode = rhi::Filter::Linear,
        .addressU = rhi::SamplerAddressMode::ClampToEdge,
        .addressV = rhi::SamplerAddressMode::ClampToEdge,
        .addressW = rhi::SamplerAddressMode::ClampToEdge,
        .debugName = "PostProcessLinearSampler",
    };
    auto sampRes = device_->createSampler(sampDesc);
    if (!sampRes.hasValue()) {
        return sampRes.error();
    }
    postProcessSampler_ = sampRes.value();

    // Default identity 3D LUT texture (256x16 unwrapped 16x16x16 identity)
    auto defaultLutRes =
        GpuTexture::create(*device_, createIdentityColorGradingLut(), false, &sampDesc);
    if (!defaultLutRes.hasValue()) {
        return defaultLutRes.error();
    }
    defaultLutTexture_ = std::move(defaultLutRes.value());

    // 2. Descriptor set layouts
    rhi::DescriptorSetLayoutDesc singleDesc{
        .bindings = {{
            .binding = 0,
            .descriptorType = rhi::DescriptorType::CombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = rhi::ShaderStageFlags::Fragment,
        }},
        .debugName = "PostProcessSingleTextureLayout",
    };
    auto singleRes = device_->createDescriptorSetLayout(singleDesc);
    if (!singleRes.hasValue()) {
        return singleRes.error();
    }
    singleTextureSetLayout_ = singleRes.value();

    rhi::DescriptorSetLayoutDesc dualDesc{
        .bindings =
            {
                {
                    .binding = 0,
                    .descriptorType = rhi::DescriptorType::CombinedImageSampler,
                    .descriptorCount = 1,
                    .stageFlags = rhi::ShaderStageFlags::Fragment,
                },
                {
                    .binding = 1,
                    .descriptorType = rhi::DescriptorType::CombinedImageSampler,
                    .descriptorCount = 1,
                    .stageFlags = rhi::ShaderStageFlags::Fragment,
                },
            },
        .debugName = "PostProcessDualTextureLayout",
    };
    auto dualRes = device_->createDescriptorSetLayout(dualDesc);
    if (!dualRes.hasValue()) {
        return dualRes.error();
    }
    dualTextureSetLayout_ = dualRes.value();

    if (!tripleTextureSetLayout_.isValid()) {
        rhi::DescriptorSetLayoutDesc tripleDesc{
            .bindings =
                {
                    {
                        .binding = 0,
                        .descriptorType = rhi::DescriptorType::CombinedImageSampler,
                        .descriptorCount = 1,
                        .stageFlags = rhi::ShaderStageFlags::Fragment,
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
                },
            .debugName = "GiTripleTextureLayout",
        };
        auto tripleRes = device_->createDescriptorSetLayout(tripleDesc);
        if (!tripleRes.hasValue()) {
            return tripleRes.error();
        }
        tripleTextureSetLayout_ = tripleRes.value();
    }

    // 3. Descriptor pool for 12 post-processing descriptor sets
    rhi::DescriptorPoolDesc poolDesc{
        .maxSets = 12,
        .poolSizes = {{
            .type = rhi::DescriptorType::CombinedImageSampler,
            .count = 24,
        }},
        .debugName = "PostProcessDescriptorPool",
    };
    auto poolRes = device_->createDescriptorPool(poolDesc);
    if (!poolRes.hasValue()) {
        return poolRes.error();
    }
    postProcessDescriptorPool_ = poolRes.value();

    for (size_t i = 0; i < kBloomPyramidLevels; ++i) {
        auto setRes =
            device_->allocateDescriptorSet(postProcessDescriptorPool_, singleTextureSetLayout_);
        if (!setRes.hasValue()) {
            return setRes.error();
        }
        bloomDownSets_[i] = setRes.value();
    }

    for (size_t i = 0; i < kBloomPyramidLevels - 1; ++i) {
        auto setRes =
            device_->allocateDescriptorSet(postProcessDescriptorPool_, dualTextureSetLayout_);
        if (!setRes.hasValue()) {
            return setRes.error();
        }
        bloomUpSets_[i] = setRes.value();
    }

    auto dofSetRes =
        device_->allocateDescriptorSet(postProcessDescriptorPool_, dualTextureSetLayout_);
    if (!dofSetRes.hasValue()) {
        return dofSetRes.error();
    }
    dofSet_ = dofSetRes.value();

    auto mbSetRes =
        device_->allocateDescriptorSet(postProcessDescriptorPool_, tripleTextureSetLayout_);
    if (!mbSetRes.hasValue()) {
        return mbSetRes.error();
    }
    motionBlurSet_ = mbSetRes.value();

    auto tmSetRes =
        device_->allocateDescriptorSet(postProcessDescriptorPool_, tripleTextureSetLayout_);
    if (!tmSetRes.hasValue()) {
        return tmSetRes.error();
    }
    toneMappingSet_ = tmSetRes.value();

    // 4. Shaders
    rhi::ShaderDesc fsVertDesc{
        .stage = rhi::ShaderStageFlags::Vertex,
        .spirvCode = kFullscreenVertexShaderSpirv,
        .entryPoint = "main",
        .debugName = "FullscreenVertexShader",
    };
    auto fsVertRes = device_->createShader(fsVertDesc);
    if (!fsVertRes.hasValue()) {
        return fsVertRes.error();
    }
    fullscreenVertexShader_ = fsVertRes.value();

    rhi::ShaderDesc dofFragDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kDofFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "DofFragmentShader",
    };
    auto dofFragRes = device_->createShader(dofFragDesc);
    if (!dofFragRes.hasValue()) {
        return dofFragRes.error();
    }
    dofFragmentShader_ = dofFragRes.value();

    rhi::ShaderDesc mbFragDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kMotionBlurFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "MotionBlurFragmentShader",
    };
    auto mbFragRes = device_->createShader(mbFragDesc);
    if (!mbFragRes.hasValue()) {
        return mbFragRes.error();
    }
    motionBlurFragmentShader_ = mbFragRes.value();

    rhi::ShaderDesc bloomDownDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kBloomDownsampleFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "BloomDownFragmentShader",
    };
    auto bloomDownRes = device_->createShader(bloomDownDesc);
    if (!bloomDownRes.hasValue()) {
        return bloomDownRes.error();
    }
    bloomDownFragmentShader_ = bloomDownRes.value();

    rhi::ShaderDesc bloomUpDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kBloomUpsampleFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "BloomUpFragmentShader",
    };
    auto bloomUpRes = device_->createShader(bloomUpDesc);
    if (!bloomUpRes.hasValue()) {
        return bloomUpRes.error();
    }
    bloomUpFragmentShader_ = bloomUpRes.value();

    rhi::ShaderDesc tmDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kToneMappingFragmentShaderSpirv,
        .entryPoint = "main",
        .debugName = "ToneMappingFragmentShader",
    };
    auto tmRes = device_->createShader(tmDesc);
    if (!tmRes.hasValue()) {
        return tmRes.error();
    }
    toneMappingFragmentShader_ = tmRes.value();

    // 5. Pipeline layouts
    rhi::PipelineLayoutDesc dofLayoutDesc{
        .pushConstants = {{
            .stageFlags = rhi::ShaderStageFlags::Fragment,
            .offset = 0,
            .size = sizeof(DofPushConstants),
        }},
        .descriptorSetLayoutHandles = {dualTextureSetLayout_},
        .descriptorSetLayouts = {},
        .debugName = "DofPipelineLayout",
    };
    auto dofLayoutRes = device_->createPipelineLayout(dofLayoutDesc);
    if (!dofLayoutRes.hasValue()) {
        return dofLayoutRes.error();
    }
    dofPipelineLayout_ = dofLayoutRes.value();

    rhi::PipelineLayoutDesc mbLayoutDesc{
        .pushConstants = {{
            .stageFlags = rhi::ShaderStageFlags::Fragment,
            .offset = 0,
            .size = sizeof(MotionBlurPushConstants),
        }},
        .descriptorSetLayoutHandles = {tripleTextureSetLayout_},
        .descriptorSetLayouts = {},
        .debugName = "MotionBlurPipelineLayout",
    };
    auto mbLayoutRes = device_->createPipelineLayout(mbLayoutDesc);
    if (!mbLayoutRes.hasValue()) {
        return mbLayoutRes.error();
    }
    motionBlurPipelineLayout_ = mbLayoutRes.value();

    rhi::PipelineLayoutDesc downLayoutDesc{
        .pushConstants = {{
            .stageFlags = rhi::ShaderStageFlags::Fragment,
            .offset = 0,
            .size = sizeof(BloomDownsamplePushConstants),
        }},
        .descriptorSetLayoutHandles = {singleTextureSetLayout_},
        .descriptorSetLayouts = {},
        .debugName = "BloomDownPipelineLayout",
    };
    auto downLayoutRes = device_->createPipelineLayout(downLayoutDesc);
    if (!downLayoutRes.hasValue()) {
        return downLayoutRes.error();
    }
    bloomDownPipelineLayout_ = downLayoutRes.value();

    rhi::PipelineLayoutDesc upLayoutDesc{
        .pushConstants = {{
            .stageFlags = rhi::ShaderStageFlags::Fragment,
            .offset = 0,
            .size = sizeof(BloomUpsamplePushConstants),
        }},
        .descriptorSetLayoutHandles = {dualTextureSetLayout_},
        .descriptorSetLayouts = {},
        .debugName = "BloomUpPipelineLayout",
    };
    auto upLayoutRes = device_->createPipelineLayout(upLayoutDesc);
    if (!upLayoutRes.hasValue()) {
        return upLayoutRes.error();
    }
    bloomUpPipelineLayout_ = upLayoutRes.value();

    rhi::PipelineLayoutDesc tmLayoutDesc{
        .pushConstants = {{
            .stageFlags = rhi::ShaderStageFlags::Fragment,
            .offset = 0,
            .size = sizeof(ToneMappingPushConstants),
        }},
        .descriptorSetLayoutHandles = {tripleTextureSetLayout_},
        .descriptorSetLayouts = {},
        .debugName = "ToneMappingPipelineLayout",
    };
    auto tmLayoutRes = device_->createPipelineLayout(tmLayoutDesc);
    if (!tmLayoutRes.hasValue()) {
        return tmLayoutRes.error();
    }
    toneMappingPipelineLayout_ = tmLayoutRes.value();

    // 6. Graphics pipelines for fullscreen passes
    rhi::GraphicsPipelineDesc dofPipeDesc{
        .vertexShader = fullscreenVertexShader_,
        .fragmentShader = dofFragmentShader_,
        .vertexInput = {},
        .topology = rhi::PrimitiveTopology::TriangleList,
        .rasterization =
            {
                .polygonMode = rhi::PolygonMode::Fill,
                .cullMode = rhi::CullMode::None,
                .frontFace = rhi::FrontFace::CounterClockwise,
            },
        .depthStencil =
            {
                .depthTestEnable = false,
                .depthWriteEnable = false,
                .depthCompareOp = rhi::CompareOp::Always,
            },
        .colorBlendAttachments = {{
            .blendEnable = false,
            .colorWriteMask = rhi::ColorComponentFlags::All,
        }},
        .layout = dofPipelineLayout_,
        .colorAttachmentFormats = {hdrFormat_},
        .debugName = "DofGraphicsPipeline",
    };
    auto dofPipeRes = device_->createGraphicsPipeline(dofPipeDesc);
    if (!dofPipeRes.hasValue()) {
        return dofPipeRes.error();
    }
    dofPipeline_ = dofPipeRes.value();

    rhi::GraphicsPipelineDesc mbPipeDesc{
        .vertexShader = fullscreenVertexShader_,
        .fragmentShader = motionBlurFragmentShader_,
        .vertexInput = {},
        .topology = rhi::PrimitiveTopology::TriangleList,
        .rasterization =
            {
                .polygonMode = rhi::PolygonMode::Fill,
                .cullMode = rhi::CullMode::None,
                .frontFace = rhi::FrontFace::CounterClockwise,
            },
        .depthStencil =
            {
                .depthTestEnable = false,
                .depthWriteEnable = false,
                .depthCompareOp = rhi::CompareOp::Always,
            },
        .colorBlendAttachments = {{
            .blendEnable = false,
            .colorWriteMask = rhi::ColorComponentFlags::All,
        }},
        .layout = motionBlurPipelineLayout_,
        .colorAttachmentFormats = {hdrFormat_},
        .debugName = "MotionBlurGraphicsPipeline",
    };
    auto mbPipeRes = device_->createGraphicsPipeline(mbPipeDesc);
    if (!mbPipeRes.hasValue()) {
        return mbPipeRes.error();
    }
    motionBlurPipeline_ = mbPipeRes.value();

    rhi::GraphicsPipelineDesc downPipeDesc{
        .vertexShader = fullscreenVertexShader_,
        .fragmentShader = bloomDownFragmentShader_,
        .vertexInput = {},
        .topology = rhi::PrimitiveTopology::TriangleList,
        .rasterization =
            {
                .polygonMode = rhi::PolygonMode::Fill,
                .cullMode = rhi::CullMode::None,
                .frontFace = rhi::FrontFace::CounterClockwise,
            },
        .depthStencil =
            {
                .depthTestEnable = false,
                .depthWriteEnable = false,
                .depthCompareOp = rhi::CompareOp::Always,
            },
        .colorBlendAttachments = {{
            .blendEnable = false,
            .colorWriteMask = rhi::ColorComponentFlags::All,
        }},
        .layout = bloomDownPipelineLayout_,
        .colorAttachmentFormats = {hdrFormat_},
        .debugName = "BloomDownGraphicsPipeline",
    };
    auto downPipeRes = device_->createGraphicsPipeline(downPipeDesc);
    if (!downPipeRes.hasValue()) {
        return downPipeRes.error();
    }
    bloomDownPipeline_ = downPipeRes.value();

    rhi::GraphicsPipelineDesc upPipeDesc{
        .vertexShader = fullscreenVertexShader_,
        .fragmentShader = bloomUpFragmentShader_,
        .vertexInput = {},
        .topology = rhi::PrimitiveTopology::TriangleList,
        .rasterization =
            {
                .polygonMode = rhi::PolygonMode::Fill,
                .cullMode = rhi::CullMode::None,
                .frontFace = rhi::FrontFace::CounterClockwise,
            },
        .depthStencil =
            {
                .depthTestEnable = false,
                .depthWriteEnable = false,
                .depthCompareOp = rhi::CompareOp::Always,
            },
        .colorBlendAttachments = {{
            .blendEnable = false,
            .colorWriteMask = rhi::ColorComponentFlags::All,
        }},
        .layout = bloomUpPipelineLayout_,
        .colorAttachmentFormats = {hdrFormat_},
        .debugName = "BloomUpGraphicsPipeline",
    };
    auto upPipeRes = device_->createGraphicsPipeline(upPipeDesc);
    if (!upPipeRes.hasValue()) {
        return upPipeRes.error();
    }
    bloomUpPipeline_ = upPipeRes.value();

    rhi::GraphicsPipelineDesc tmPipeDesc{
        .vertexShader = fullscreenVertexShader_,
        .fragmentShader = toneMappingFragmentShader_,
        .vertexInput = {},
        .topology = rhi::PrimitiveTopology::TriangleList,
        .rasterization =
            {
                .polygonMode = rhi::PolygonMode::Fill,
                .cullMode = rhi::CullMode::None,
                .frontFace = rhi::FrontFace::CounterClockwise,
            },
        .depthStencil =
            {
                .depthTestEnable = false,
                .depthWriteEnable = false,
                .depthCompareOp = rhi::CompareOp::Always,
            },
        .colorBlendAttachments = {{
            .blendEnable = false,
            .colorWriteMask = rhi::ColorComponentFlags::All,
        }},
        .layout = toneMappingPipelineLayout_,
        .colorAttachmentFormats = {colorFormat_},
        .debugName = "ToneMappingGraphicsPipeline",
    };
    auto tmPipeRes = device_->createGraphicsPipeline(tmPipeDesc);
    if (!tmPipeRes.hasValue()) {
        return tmPipeRes.error();
    }
    toneMappingPipeline_ = tmPipeRes.value();

    return Result<void>::success();
}

Result<void> SceneRenderer::createPostProcessTargets(uint32_t width, uint32_t height) {
    if (device_ == nullptr || width == 0 || height == 0) {
        return Result<void>::success();
    }

    // 1. Fullscreen HDR Scene Target
    rhi::TextureDesc hdrDesc{
        .type = rhi::TextureType::Texture2D,
        .format = hdrFormat_,
        .width = width,
        .height = height,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = rhi::TextureUsageFlags::ColorAttachment | rhi::TextureUsageFlags::Sampled |
                 rhi::TextureUsageFlags::TransferSrc,
        .debugName = "SceneRendererHdrTarget",
    };
    auto hdrRes = device_->createTexture(hdrDesc);
    if (!hdrRes.hasValue()) {
        return hdrRes.error();
    }
    hdrTarget_ = hdrRes.value();

    // 1b. Intermediate HDR Targets for DoF and Motion Blur
    rhi::TextureDesc dofTargetDesc = hdrDesc;
    dofTargetDesc.debugName = "PostProcessDofTarget";
    auto dofTargetRes = device_->createTexture(dofTargetDesc);
    if (!dofTargetRes.hasValue()) {
        return dofTargetRes.error();
    }
    postProcessTempTarget_[0] = dofTargetRes.value();

    rhi::TextureDesc mbTargetDesc = hdrDesc;
    mbTargetDesc.debugName = "PostProcessMotionBlurTarget";
    auto mbTargetRes = device_->createTexture(mbTargetDesc);
    if (!mbTargetRes.hasValue()) {
        return mbTargetRes.error();
    }
    postProcessTempTarget_[1] = mbTargetRes.value();

    // 2. Bloom Downsample Pyramid Targets
    for (size_t i = 0; i < kBloomPyramidLevels; ++i) {
        uint32_t downW = std::max(1u, width >> (i + 1));
        uint32_t downH = std::max(1u, height >> (i + 1));
        rhi::TextureDesc downDesc{
            .type = rhi::TextureType::Texture2D,
            .format = hdrFormat_,
            .width = downW,
            .height = downH,
            .depth = 1,
            .mipLevels = 1,
            .arrayLayers = 1,
            .usage = rhi::TextureUsageFlags::ColorAttachment | rhi::TextureUsageFlags::Sampled,
            .debugName = "BloomDownTarget",
        };
        auto downRes = device_->createTexture(downDesc);
        if (!downRes.hasValue()) {
            return downRes.error();
        }
        bloomDownTarget_[i] = downRes.value();
    }

    // 3. Bloom Upsample Targets
    for (size_t i = 0; i < kBloomPyramidLevels - 1; ++i) {
        size_t level = (kBloomPyramidLevels - 2) - i;
        uint32_t upW = std::max(1u, width >> (level + 1));
        uint32_t upH = std::max(1u, height >> (level + 1));
        rhi::TextureDesc upDesc{
            .type = rhi::TextureType::Texture2D,
            .format = hdrFormat_,
            .width = upW,
            .height = upH,
            .depth = 1,
            .mipLevels = 1,
            .arrayLayers = 1,
            .usage = rhi::TextureUsageFlags::ColorAttachment | rhi::TextureUsageFlags::Sampled,
            .debugName = "BloomUpTarget",
        };
        auto upRes = device_->createTexture(upDesc);
        if (!upRes.hasValue()) {
            return upRes.error();
        }
        bloomUpTarget_[level] = upRes.value();
    }

    updatePostProcessDescriptors();

    return Result<void>::success();
}

void SceneRenderer::updatePostProcessDescriptors() {
    if (device_ == nullptr || !postProcessSampler_.isValid() || !hdrTarget_.isValid()) {
        return;
    }

    const rhi::SamplerHandle depthSamp =
        pointClampSampler_.isValid() ? pointClampSampler_ : postProcessSampler_;

    const bool hasDof = postProcessSettings_.dofEnabled && dofPipeline_.isValid() &&
                        postProcessTempTarget_[0].isValid() && depthTarget_.isValid();
    const rhi::TextureHandle afterDofTex = hasDof ? postProcessTempTarget_[0] : hdrTarget_;

    const bool hasMotionBlur =
        postProcessSettings_.motionBlurEnabled && motionBlurPipeline_.isValid() &&
        postProcessTempTarget_[1].isValid() && velocityTarget_.isValid() && depthTarget_.isValid();
    const rhi::TextureHandle finalHdrTex = hasMotionBlur ? postProcessTempTarget_[1] : afterDofTex;

    if (dofSet_.isValid() && depthTarget_.isValid()) {
        std::array<rhi::DescriptorWrite, 2> writes{
            rhi::DescriptorWrite::makeCombinedImageSampler(0, hdrTarget_, postProcessSampler_),
            rhi::DescriptorWrite::makeCombinedImageSampler(1, depthTarget_, depthSamp),
        };
        std::ignore = device_->updateDescriptorSet(dofSet_, writes);
    }

    if (motionBlurSet_.isValid() && velocityTarget_.isValid() && depthTarget_.isValid()) {
        std::array<rhi::DescriptorWrite, 3> writes{
            rhi::DescriptorWrite::makeCombinedImageSampler(0, afterDofTex, postProcessSampler_),
            rhi::DescriptorWrite::makeCombinedImageSampler(1, velocityTarget_, depthSamp),
            rhi::DescriptorWrite::makeCombinedImageSampler(2, depthTarget_, depthSamp),
        };
        std::ignore = device_->updateDescriptorSet(motionBlurSet_, writes);
    }

    for (size_t i = 0; i < kBloomPyramidLevels; ++i) {
        if (!bloomDownSets_[i].isValid()) {
            continue;
        }
        rhi::TextureHandle srcTex = (i == 0) ? finalHdrTex : bloomDownTarget_[i - 1];
        if (srcTex.isValid()) {
            std::array<rhi::DescriptorWrite, 1> writes{
                rhi::DescriptorWrite::makeCombinedImageSampler(0, srcTex, postProcessSampler_),
            };
            std::ignore = device_->updateDescriptorSet(bloomDownSets_[i], writes);
        }
    }

    for (size_t step = 0; step < kBloomPyramidLevels - 1; ++step) {
        if (!bloomUpSets_[step].isValid()) {
            continue;
        }
        rhi::TextureHandle sampleTex = (step == 0) ? bloomDownTarget_[kBloomPyramidLevels - 1]
                                                   : bloomUpTarget_[kBloomPyramidLevels - 1 - step];
        size_t addLevel = (kBloomPyramidLevels - 2) - step;
        rhi::TextureHandle addTex = bloomDownTarget_[addLevel];
        if (sampleTex.isValid() && addTex.isValid()) {
            std::array<rhi::DescriptorWrite, 2> writes{
                rhi::DescriptorWrite::makeCombinedImageSampler(0, sampleTex, postProcessSampler_),
                rhi::DescriptorWrite::makeCombinedImageSampler(1, addTex, postProcessSampler_),
            };
            std::ignore = device_->updateDescriptorSet(bloomUpSets_[step], writes);
        }
    }

    const rhi::TextureHandle lutTex =
        (customLutTexture_ != nullptr && customLutTexture_->isValid())
            ? customLutTexture_->getTexture()
            : (defaultLutTexture_ != nullptr ? defaultLutTexture_->getTexture()
                                             : rhi::TextureHandle{});

    if (toneMappingSet_.isValid() && bloomUpTarget_[0].isValid() && lutTex.isValid()) {
        std::array<rhi::DescriptorWrite, 3> writes{
            rhi::DescriptorWrite::makeCombinedImageSampler(0, finalHdrTex, postProcessSampler_),
            rhi::DescriptorWrite::makeCombinedImageSampler(1, bloomUpTarget_[0],
                                                           postProcessSampler_),
            rhi::DescriptorWrite::makeCombinedImageSampler(2, lutTex, postProcessSampler_),
        };
        std::ignore = device_->updateDescriptorSet(toneMappingSet_, writes);
    }
}

void SceneRenderer::destroyPostProcessTargets() {
    if (device_ != nullptr) {
        if (hdrTarget_.isValid()) {
            device_->destroyTexture(hdrTarget_);
            hdrTarget_ = {};
        }
        for (size_t i = 0; i < 2; ++i) {
            if (postProcessTempTarget_[i].isValid()) {
                device_->destroyTexture(postProcessTempTarget_[i]);
                postProcessTempTarget_[i] = {};
            }
        }
        for (size_t i = 0; i < kBloomPyramidLevels; ++i) {
            if (bloomDownTarget_[i].isValid()) {
                device_->destroyTexture(bloomDownTarget_[i]);
                bloomDownTarget_[i] = {};
            }
        }
        for (size_t i = 0; i < kBloomPyramidLevels - 1; ++i) {
            if (bloomUpTarget_[i].isValid()) {
                device_->destroyTexture(bloomUpTarget_[i]);
                bloomUpTarget_[i] = {};
            }
        }
    }
}

void SceneRenderer::buildPostProcessGraph() {
    postProcessGraph_.reset();
    if (device_ == nullptr || !postProcessSettings_.enabled || !hdrTarget_.isValid() ||
        !colorTarget_.isValid()) {
        return;
    }

    auto hdrId = postProcessGraph_.importTexture("HdrSceneTarget", hdrTarget_);
    auto ldrId = postProcessGraph_.importTexture("DisplayColorTarget", colorTarget_);
    auto currentHdrId = hdrId;

    const bool hasDof = postProcessSettings_.dofEnabled && dofPipeline_.isValid() &&
                        postProcessTempTarget_[0].isValid() && depthTarget_.isValid();
    const bool hasMotionBlur =
        postProcessSettings_.motionBlurEnabled && motionBlurPipeline_.isValid() &&
        postProcessTempTarget_[1].isValid() && velocityTarget_.isValid() && depthTarget_.isValid();

    GraphTextureId depthId{};
    if (hasDof || hasMotionBlur) {
        depthId = postProcessGraph_.importTexture("PostProcessDepthTarget", depthTarget_);
    }

    if (hasDof) {
        auto dofOutId =
            postProcessGraph_.importTexture("PostProcessDofTarget", postProcessTempTarget_[0]);
        postProcessGraph_.addPass(
            "DofPass",
            [currentHdrId, depthId, dofOutId](RenderPassBuilder& builder) {
                builder.readTexture(currentHdrId, rhi::TextureUsageFlags::Sampled);
                builder.readTexture(depthId, rhi::TextureUsageFlags::Sampled);
                builder.writeTexture(dofOutId, rhi::TextureUsageFlags::ColorAttachment);
            },
            [this](rhi::RhiCommandList& cmdList) { recordDofPass(cmdList); });
        currentHdrId = dofOutId;
    }

    if (hasMotionBlur) {
        auto velId = postProcessGraph_.importTexture("PostProcessVelocityTarget", velocityTarget_);
        auto mbOutId = postProcessGraph_.importTexture("PostProcessMotionBlurTarget",
                                                       postProcessTempTarget_[1]);
        postProcessGraph_.addPass(
            "MotionBlurPass",
            [currentHdrId, velId, depthId, mbOutId](RenderPassBuilder& builder) {
                builder.readTexture(currentHdrId, rhi::TextureUsageFlags::Sampled);
                builder.readTexture(velId, rhi::TextureUsageFlags::Sampled);
                if (depthId.isValid()) {
                    builder.readTexture(depthId, rhi::TextureUsageFlags::Sampled);
                }
                builder.writeTexture(mbOutId, rhi::TextureUsageFlags::ColorAttachment);
            },
            [this](rhi::RhiCommandList& cmdList) { recordMotionBlurPass(cmdList); });
        currentHdrId = mbOutId;
    }

    GraphTextureId bloomDownIds[kBloomPyramidLevels]{};
    GraphTextureId bloomUpIds[kBloomPyramidLevels - 1]{};

    const bool hasBloom = postProcessSettings_.bloomEnabled && bloomDownPipeline_.isValid() &&
                          bloomUpPipeline_.isValid();

    if (hasBloom) {
        for (size_t i = 0; i < kBloomPyramidLevels; ++i) {
            if (bloomDownTarget_[i].isValid()) {
                bloomDownIds[i] = postProcessGraph_.importTexture(
                    "BloomDownTarget_" + std::to_string(i), bloomDownTarget_[i]);
            }
        }
        for (size_t i = 0; i < kBloomPyramidLevels - 1; ++i) {
            if (bloomUpTarget_[i].isValid()) {
                bloomUpIds[i] = postProcessGraph_.importTexture(
                    "BloomUpTarget_" + std::to_string(i), bloomUpTarget_[i]);
            }
        }

        // Bloom Downsample Passes
        for (size_t i = 0; i < kBloomPyramidLevels; ++i) {
            auto srcId = (i == 0) ? currentHdrId : bloomDownIds[i - 1];
            auto dstId = bloomDownIds[i];
            postProcessGraph_.addPass(
                "BloomDownsample_" + std::to_string(i),
                [srcId, dstId](RenderPassBuilder& builder) {
                    builder.readTexture(srcId, rhi::TextureUsageFlags::Sampled);
                    builder.writeTexture(dstId, rhi::TextureUsageFlags::ColorAttachment);
                },
                [this, i](rhi::RhiCommandList& cmdList) { recordBloomDownsamplePass(cmdList, i); });
        }

        // Bloom Upsample Passes
        for (size_t step = 0; step < kBloomPyramidLevels - 1; ++step) {
            size_t targetLevel = (kBloomPyramidLevels - 2) - step;
            auto srcHigherId =
                (step == 0) ? bloomDownIds[kBloomPyramidLevels - 1] : bloomUpIds[targetLevel + 1];
            auto srcLowerId = bloomDownIds[targetLevel];
            auto dstId = bloomUpIds[targetLevel];
            postProcessGraph_.addPass(
                "BloomUpsample_" + std::to_string(step),
                [srcHigherId, srcLowerId, dstId](RenderPassBuilder& builder) {
                    builder.readTexture(srcHigherId, rhi::TextureUsageFlags::Sampled);
                    builder.readTexture(srcLowerId, rhi::TextureUsageFlags::Sampled);
                    builder.writeTexture(dstId, rhi::TextureUsageFlags::ColorAttachment);
                },
                [this, step, targetLevel](rhi::RhiCommandList& cmdList) {
                    recordBloomUpsamplePass(cmdList, step, targetLevel);
                });
        }
    }

    // Tone Mapping Pass
    if (toneMappingPipeline_.isValid()) {
        postProcessGraph_.addPass(
            "ToneMappingPass",
            [currentHdrId, hasBloom, bloomUp0 = bloomUpIds[0], ldrId](RenderPassBuilder& builder) {
                builder.readTexture(currentHdrId, rhi::TextureUsageFlags::Sampled);
                if (hasBloom && bloomUp0.isValid()) {
                    builder.readTexture(bloomUp0, rhi::TextureUsageFlags::Sampled);
                }
                builder.writeTexture(ldrId, rhi::TextureUsageFlags::ColorAttachment);
            },
            [this](rhi::RhiCommandList& cmdList) { recordToneMappingPass(cmdList); });
    }

    auto compileRes = postProcessGraph_.compile();
    if (!compileRes.hasValue()) {
        ENG_LOG_ERROR("SceneRenderer", "Failed to compile PostProcess RenderGraph");
    }
}

void SceneRenderer::recordDofPass(rhi::RhiCommandList& cmdList) {
    if (!dofPipeline_.isValid() || !dofSet_.isValid() || !postProcessTempTarget_[0].isValid()) {
        return;
    }
    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 22, rhi::GpuTimestampStage::TopOfPipe);
    }
    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = postProcessTempTarget_[0],
        .depthTarget = {},
        .clearColor = {0.0F, 0.0F, 0.0F, 1.0F},
        .clearDepth = 1.0F,
        .clearTarget = true,
        .clearDepthTarget = false,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_));
    cmdList.setScissor(0, 0, width_, height_);
    cmdList.bindPipeline(dofPipeline_);

    const std::array<rhi::DescriptorSetHandle, 1> sets = {dofSet_};
    cmdList.bindDescriptorSets(dofPipelineLayout_, 0, sets);

    DofPushConstants pc{
        .texelSize = Vec2(1.0F / static_cast<float>(std::max(1U, width_)),
                          1.0F / static_cast<float>(std::max(1U, height_))),
        .focusDistance = std::max(0.1F, postProcessSettings_.dofFocusDistance),
        .focalLengthMm = std::max(1.0F, postProcessSettings_.dofFocalLength),
        .aperture = std::max(0.7F, postProcessSettings_.dofAperture),
        .maxBlurRadius = std::clamp(postProcessSettings_.dofMaxBlurRadius, 1.0F, 24.0F),
        .zNear = 0.1F,
        .zFar = 1000.0F,
    };
    cmdList.pushConstants(dofPipelineLayout_, rhi::ShaderStageFlags::Fragment, 0,
                          sizeof(DofPushConstants), &pc);
    cmdList.draw(3, 1, 0, 0);
    cmdList.endRenderPass();

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 23, rhi::GpuTimestampStage::BottomOfPipe);
    }
}

void SceneRenderer::recordMotionBlurPass(rhi::RhiCommandList& cmdList) {
    if (!motionBlurPipeline_.isValid() || !motionBlurSet_.isValid() ||
        !postProcessTempTarget_[1].isValid()) {
        return;
    }
    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 30, rhi::GpuTimestampStage::TopOfPipe);
    }
    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = postProcessTempTarget_[1],
        .depthTarget = {},
        .clearColor = {0.0F, 0.0F, 0.0F, 1.0F},
        .clearDepth = 1.0F,
        .clearTarget = true,
        .clearDepthTarget = false,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_));
    cmdList.setScissor(0, 0, width_, height_);
    cmdList.bindPipeline(motionBlurPipeline_);

    const std::array<rhi::DescriptorSetHandle, 1> sets = {motionBlurSet_};
    cmdList.bindDescriptorSets(motionBlurPipelineLayout_, 0, sets);

    MotionBlurPushConstants pc{
        .texelSize = Vec2(1.0F / static_cast<float>(std::max(1U, width_)),
                          1.0F / static_cast<float>(std::max(1U, height_))),
        .intensity = std::clamp(postProcessSettings_.motionBlurIntensity, 0.0F, 2.0F),
        .sampleCount =
            static_cast<int32_t>(std::clamp(postProcessSettings_.motionBlurSamples, 4U, 16U)),
        .maxVelocityPixels = 32.0F,
        .zNear = 0.1F,
        .zFar = 1000.0F,
        .pad0 = 0.0F,
    };
    cmdList.pushConstants(motionBlurPipelineLayout_, rhi::ShaderStageFlags::Fragment, 0,
                          sizeof(MotionBlurPushConstants), &pc);
    cmdList.draw(3, 1, 0, 0);
    cmdList.endRenderPass();

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 31, rhi::GpuTimestampStage::BottomOfPipe);
    }
}

void SceneRenderer::recordBloomDownsamplePass(rhi::RhiCommandList& cmdList, size_t i) {
    if (i == 0 && currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 6, rhi::GpuTimestampStage::TopOfPipe);
    }
    cmdList.bindPipeline(bloomDownPipeline_);
    uint32_t dstW = std::max(1u, width_ >> (i + 1));
    uint32_t dstH = std::max(1u, height_ >> (i + 1));
    uint32_t srcW = (i == 0) ? width_ : std::max(1u, width_ >> i);
    uint32_t srcH = (i == 0) ? height_ : std::max(1u, height_ >> i);

    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = bloomDownTarget_[i],
        .depthTarget = {},
        .clearColor = {0.0F, 0.0F, 0.0F, 1.0F},
        .clearDepth = 1.0F,
        .clearTarget = true,
        .clearDepthTarget = false,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(dstW), static_cast<float>(dstH));
    cmdList.setScissor(0, 0, dstW, dstH);

    const std::array<rhi::DescriptorSetHandle, 1> downSets = {bloomDownSets_[i]};
    cmdList.bindDescriptorSets(bloomDownPipelineLayout_, 0, downSets);

    BloomDownsamplePushConstants pc{
        .texelSize = Vec2(1.0F / static_cast<float>(srcW), 1.0F / static_cast<float>(srcH)),
        .threshold = postProcessSettings_.bloomThreshold,
        .knee = postProcessSettings_.bloomKnee,
        .isFirstPass = (i == 0) ? 1 : 0,
        .padding = {0.0F, 0.0F, 0.0F},
    };
    cmdList.pushConstants(bloomDownPipelineLayout_, rhi::ShaderStageFlags::Fragment, 0,
                          sizeof(BloomDownsamplePushConstants), &pc);
    cmdList.draw(3, 1, 0, 0);
    cmdList.endRenderPass();

    if (i == kBloomPyramidLevels - 1 && currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 7, rhi::GpuTimestampStage::BottomOfPipe);
    }
}

void SceneRenderer::recordBloomUpsamplePass(rhi::RhiCommandList& cmdList, size_t step,
                                            size_t targetLevel) {
    if (step == 0 && currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 8, rhi::GpuTimestampStage::TopOfPipe);
    }
    cmdList.bindPipeline(bloomUpPipeline_);
    uint32_t dstW = std::max(1u, width_ >> (targetLevel + 1));
    uint32_t dstH = std::max(1u, height_ >> (targetLevel + 1));
    uint32_t srcW = std::max(1u, width_ >> (targetLevel + 2));
    uint32_t srcH = std::max(1u, height_ >> (targetLevel + 2));

    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = bloomUpTarget_[targetLevel],
        .depthTarget = {},
        .clearColor = {0.0F, 0.0F, 0.0F, 1.0F},
        .clearDepth = 1.0F,
        .clearTarget = true,
        .clearDepthTarget = false,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(dstW), static_cast<float>(dstH));
    cmdList.setScissor(0, 0, dstW, dstH);

    const std::array<rhi::DescriptorSetHandle, 1> upSets = {bloomUpSets_[step]};
    cmdList.bindDescriptorSets(bloomUpPipelineLayout_, 0, upSets);

    BloomUpsamplePushConstants pc{
        .texelSize = Vec2(1.0F / static_cast<float>(srcW), 1.0F / static_cast<float>(srcH)),
        .filterRadius = postProcessSettings_.bloomFilterRadius,
        .blendFactor = 1.0F,
    };
    cmdList.pushConstants(bloomUpPipelineLayout_, rhi::ShaderStageFlags::Fragment, 0,
                          sizeof(BloomUpsamplePushConstants), &pc);
    cmdList.draw(3, 1, 0, 0);
    cmdList.endRenderPass();

    if (step == kBloomPyramidLevels - 2 && currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 9, rhi::GpuTimestampStage::BottomOfPipe);
    }
}

void SceneRenderer::recordToneMappingPass(rhi::RhiCommandList& cmdList) {
    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 10, rhi::GpuTimestampStage::TopOfPipe);
    }
    rhi::RenderPassBeginInfo passInfo{
        .colorTarget = colorTarget_,
        .depthTarget = {},
        .clearColor = {0.0F, 0.0F, 0.0F, 1.0F},
        .clearDepth = 1.0F,
        .clearTarget = true,
        .clearDepthTarget = false,
    };
    cmdList.beginRenderPass(passInfo);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_));
    cmdList.setScissor(0, 0, width_, height_);
    cmdList.bindPipeline(toneMappingPipeline_);

    const std::array<rhi::DescriptorSetHandle, 1> tmSets = {toneMappingSet_};
    cmdList.bindDescriptorSets(toneMappingPipelineLayout_, 0, tmSets);

    const float compGain = std::exp2(postProcessSettings_.exposureCompensation);
    const float effectiveExposure = postProcessSettings_.autoExposureEnabled
                                        ? (adaptedExposure_ * compGain)
                                        : (postProcessSettings_.exposure * compGain);

    const float lutStrength = postProcessSettings_.lutEnabled
                                  ? std::clamp(postProcessSettings_.lutIntensity, 0.0F, 1.0F)
                                  : 0.0F;
    const float vigStrength = postProcessSettings_.vignetteEnabled
                                  ? std::clamp(postProcessSettings_.vignetteIntensity, 0.0F, 1.0F)
                                  : 0.0F;
    const float grainStrength =
        postProcessSettings_.filmGrainEnabled
            ? std::clamp(postProcessSettings_.filmGrainIntensity, 0.0F, 0.25F)
            : 0.0F;
    const float aspectRatio =
        (height_ > 0) ? (static_cast<float>(width_) / static_cast<float>(height_)) : 1.0F;

    ToneMappingPushConstants pc{
        .exposure = effectiveExposure,
        .toneMapOp = static_cast<int32_t>(postProcessSettings_.toneMapOp),
        .bloomIntensity =
            postProcessSettings_.bloomEnabled ? postProcessSettings_.bloomIntensity : 0.0F,
        .bloomEnabled = postProcessSettings_.bloomEnabled ? 1 : 0,
        .whitePoint = postProcessSettings_.reinhardWhitePoint,
        .whiteBalance = postProcessSettings_.whiteBalance,
        .contrast = postProcessSettings_.contrast,
        .saturation = postProcessSettings_.saturation,
        .lutIntensity = lutStrength,
        .vignetteIntensity = vigStrength,
        .vignetteSmoothness = std::max(0.05F, postProcessSettings_.vignetteSmoothness),
        .vignetteRoundness = std::clamp(postProcessSettings_.vignetteRoundness, 0.0F, 1.0F),
        .filmGrainIntensity = grainStrength,
        .frameIndex = giFrameIndex_ % 1024U,
        .aspectRatio = aspectRatio,
        .pad0 = 0.0F,
    };
    cmdList.pushConstants(toneMappingPipelineLayout_, rhi::ShaderStageFlags::Fragment, 0,
                          sizeof(ToneMappingPushConstants), &pc);
    cmdList.draw(3, 1, 0, 0);
    cmdList.endRenderPass();

    if (currentHasTimestamps_) {
        cmdList.writeTimestamp(currentBaseQuery_ + 11, rhi::GpuTimestampStage::BottomOfPipe);
        cmdList.writeTimestamp(currentBaseQuery_ + 1, rhi::GpuTimestampStage::BottomOfPipe);
    }
}

void SceneRenderer::renderPostProcess(rhi::RhiCommandList& cmdList) {
    if (device_ == nullptr || !postProcessSettings_.enabled || !hdrTarget_.isValid() ||
        !colorTarget_.isValid()) {
        return;
    }

    currentHasTimestamps_ = device_->getCaps().features.timestampQueries;
    currentBaseQuery_ = (frameSlotIndex_ % 2) * 32;

    if (!postProcessGraph_.isCompiled()) {
        buildPostProcessGraph();
    }
    if (postProcessGraph_.isCompiled()) {
        auto execRes = postProcessGraph_.execute(*device_, cmdList);
        if (!execRes.hasValue()) {
            ENG_LOG_ERROR("SceneRenderer", "PostProcess RenderGraph execute failed");
        }
    }
}

void SceneRenderer::setPostProcessSettings(const PostProcessSettings& settings) noexcept {
    const bool graphDirty = (postProcessSettings_.enabled != settings.enabled) ||
                            (postProcessSettings_.bloomEnabled != settings.bloomEnabled) ||
                            (postProcessSettings_.dofEnabled != settings.dofEnabled) ||
                            (postProcessSettings_.motionBlurEnabled != settings.motionBlurEnabled);
    const bool mainGraphDirty =
        (postProcessSettings_.enabled != settings.enabled) ||
        (postProcessSettings_.motionBlurEnabled != settings.motionBlurEnabled);
    const bool lutPathChanged = (loadedLutPath_ != settings.lutFilePath);

    postProcessSettings_ = settings;
    settings_.postProcess = settings;

    if (device_ != nullptr && lutPathChanged) {
        if (settings.lutFilePath.empty()) {
            clearColorGradingLut();
        } else {
            auto res = loadColorGradingLutFromFile(settings.lutFilePath);
            if (!res.hasValue()) {
                ENG_LOG_WARN("SceneRenderer",
                             "Failed to load Color Grading LUT: " + settings.lutFilePath + " (" +
                                 std::string(res.error().message) + ")");
            }
        }
    }

    if (mainGraphDirty) {
        renderGraph_.reset();
    }
    if (graphDirty) {
        updatePostProcessDescriptors();
        postProcessGraph_.reset();
    }
}

ImageData SceneRenderer::createIdentityColorGradingLut() {
    constexpr uint32_t kLutSize = 16;
    constexpr uint32_t kWidth = kLutSize * kLutSize; // 256
    constexpr uint32_t kHeight = kLutSize;           // 16

    ImageData img{};
    img.width = kWidth;
    img.height = kHeight;
    img.channels = 4;
    img.format = rhi::TextureFormat::RGBA8_UNORM;
    img.pixels.resize(static_cast<size_t>(kWidth) * kHeight * 4U);

    for (uint32_t y = 0; y < kHeight; ++y) {
        const uint8_t gByte = static_cast<uint8_t>((y * 255U + 7U) / 15U);
        for (uint32_t bSlice = 0; bSlice < kLutSize; ++bSlice) {
            const uint8_t bByte = static_cast<uint8_t>((bSlice * 255U + 7U) / 15U);
            for (uint32_t rIdx = 0; rIdx < kLutSize; ++rIdx) {
                const uint8_t rByte = static_cast<uint8_t>((rIdx * 255U + 7U) / 15U);
                const uint32_t x = bSlice * kLutSize + rIdx;
                const size_t offset = (static_cast<size_t>(y) * kWidth + x) * 4U;
                img.pixels[offset + 0] = rByte;
                img.pixels[offset + 1] = gByte;
                img.pixels[offset + 2] = bByte;
                img.pixels[offset + 3] = 255U;
            }
        }
    }
    return img;
}

Result<void> SceneRenderer::loadColorGradingLut(const ImageData& lutImage) {
    if (device_ == nullptr) {
        return Error{.code = ErrorCode::InvalidState, .message = "Device not initialized"};
    }
    if (!lutImage.isValid() || lutImage.width != 256U || lutImage.height != 16U ||
        lutImage.channels != 4U) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Color grading LUT must be a 256x16 RGBA8 unwrapped 16^3 strip"};
    }

    ImageData linearLut = lutImage;
    linearLut.format = rhi::TextureFormat::RGBA8_UNORM;

    rhi::SamplerDesc sampDesc{
        .minFilter = rhi::Filter::Linear,
        .magFilter = rhi::Filter::Linear,
        .mipmapMode = rhi::Filter::Linear,
        .addressU = rhi::SamplerAddressMode::ClampToEdge,
        .addressV = rhi::SamplerAddressMode::ClampToEdge,
        .addressW = rhi::SamplerAddressMode::ClampToEdge,
        .debugName = "CustomLutLinearSampler",
    };
    auto texRes = GpuTexture::create(*device_, linearLut, false, &sampDesc);
    if (!texRes.hasValue()) {
        return texRes.error();
    }
    customLutTexture_ = std::move(texRes.value());
    updatePostProcessDescriptors();
    return Result<void>::success();
}

Result<void> SceneRenderer::loadColorGradingLutFromFile(std::string_view filePath) {
    if (filePath.empty()) {
        clearColorGradingLut();
        return Result<void>::success();
    }
    auto imgRes = ImageDecoder::decodeFromFile(filePath, 4, false);
    if (!imgRes.hasValue()) {
        return imgRes.error();
    }
    auto loadRes = loadColorGradingLut(imgRes.value());
    if (!loadRes.hasValue()) {
        return loadRes.error();
    }
    loadedLutPath_ = std::string(filePath);
    postProcessSettings_.lutFilePath = loadedLutPath_;
    return Result<void>::success();
}

void SceneRenderer::clearColorGradingLut() noexcept {
    customLutTexture_.reset();
    loadedLutPath_.clear();
    updatePostProcessDescriptors();
}

void SceneRenderer::updateExposureAdaptation(float sceneAverageLuminance,
                                             float deltaSeconds) noexcept {
    const float validLuma = (std::isfinite(sceneAverageLuminance) && sceneAverageLuminance > 1e-4F)
                                ? sceneAverageLuminance
                                : 1e-4F;
    const float minEv =
        std::min(postProcessSettings_.autoExposureMinEv, postProcessSettings_.autoExposureMaxEv);
    const float maxEv =
        std::max(postProcessSettings_.autoExposureMinEv, postProcessSettings_.autoExposureMaxEv);
    const float minLuma = 0.18F * std::exp2(minEv);
    const float maxLuma = 0.18F * std::exp2(maxEv);
    const float clampedTargetLuma = std::clamp(validLuma, minLuma, maxLuma);

    if (deltaSeconds <= 0.0F || !std::isfinite(deltaSeconds)) {
        adaptedLuminance_ = clampedTargetLuma;
    } else {
        const float speed = (clampedTargetLuma > adaptedLuminance_)
                                ? std::max(0.05F, postProcessSettings_.autoExposureSpeedUp)
                                : std::max(0.05F, postProcessSettings_.autoExposureSpeedDown);
        const float alpha = 1.0F - std::exp(-deltaSeconds * speed);
        adaptedLuminance_ = adaptedLuminance_ +
                            (clampedTargetLuma - adaptedLuminance_) * std::clamp(alpha, 0.0F, 1.0F);
    }
    adaptedLuminance_ = std::clamp(adaptedLuminance_, minLuma, maxLuma);
    adaptedExposure_ = 0.18F / std::max(1e-4F, adaptedLuminance_);
}

void SceneRenderer::updateGpuTimings(rhi::RhiDevice& device) {
    if (!device.getCaps().features.timestampQueries) {
        gpuPassTimings_.valid = false;
        return;
    }

    // Double-buffered query retrieval: inspect query results from prior frame slot
    const uint32_t readbackBase = (((frameSlotIndex_ + 1) % 2)) * 32;

    auto totalOpt = device.getQueryTimeMs(readbackBase + 0, readbackBase + 1);
    if (totalOpt.has_value()) {
        gpuPassTimings_.totalSceneMs = totalOpt.value();
        gpuPassTimings_.shadowPassMs =
            device.getQueryTimeMs(readbackBase + 2, readbackBase + 3).value_or(0.0F);
        gpuPassTimings_.depthPrepassMs =
            device.getQueryTimeMs(readbackBase + 12, readbackBase + 13).value_or(0.0F);
        gpuPassTimings_.mainPbrPassMs =
            device.getQueryTimeMs(readbackBase + 4, readbackBase + 5).value_or(0.0F);
        gpuPassTimings_.dofPassMs =
            postProcessSettings_.dofEnabled
                ? device.getQueryTimeMs(readbackBase + 22, readbackBase + 23).value_or(0.0F)
                : 0.0F;
        gpuPassTimings_.motionBlurPassMs =
            postProcessSettings_.motionBlurEnabled
                ? device.getQueryTimeMs(readbackBase + 30, readbackBase + 31).value_or(0.0F)
                : 0.0F;
        gpuPassTimings_.bloomDownsampleMs =
            postProcessSettings_.bloomEnabled
                ? device.getQueryTimeMs(readbackBase + 6, readbackBase + 7).value_or(0.0F)
                : 0.0F;
        gpuPassTimings_.bloomUpsampleMs =
            postProcessSettings_.bloomEnabled
                ? device.getQueryTimeMs(readbackBase + 8, readbackBase + 9).value_or(0.0F)
                : 0.0F;
        gpuPassTimings_.toneMappingMs =
            device.getQueryTimeMs(readbackBase + 10, readbackBase + 11).value_or(0.0F);
        gpuPassTimings_.gtaoPassMs =
            device.getQueryTimeMs(readbackBase + 14, readbackBase + 15).value_or(0.0F);
        gpuPassTimings_.ssgiPassMs =
            device.getQueryTimeMs(readbackBase + 16, readbackBase + 17).value_or(0.0F);
        gpuPassTimings_.ssrPassMs =
            device.getQueryTimeMs(readbackBase + 20, readbackBase + 21).value_or(0.0F);
        gpuPassTimings_.temporalDenoiseMs =
            device.getQueryTimeMs(readbackBase + 18, readbackBase + 19).value_or(0.0F);
        gpuPassTimings_.velocityPassMs =
            device.getQueryTimeMs(readbackBase + 24, readbackBase + 25).value_or(0.0F);
        gpuPassTimings_.taaPassMs =
            device.getQueryTimeMs(readbackBase + 26, readbackBase + 27).value_or(0.0F);
        gpuPassTimings_.sharpenPassMs =
            device.getQueryTimeMs(readbackBase + 28, readbackBase + 29).value_or(0.0F);
        gpuPassTimings_.totalGiMs = gpuPassTimings_.gtaoPassMs + gpuPassTimings_.ssgiPassMs +
                                    gpuPassTimings_.ssrPassMs + gpuPassTimings_.temporalDenoiseMs;
        gpuPassTimings_.totalPostProcessMs =
            gpuPassTimings_.dofPassMs + gpuPassTimings_.motionBlurPassMs +
            gpuPassTimings_.bloomDownsampleMs + gpuPassTimings_.bloomUpsampleMs +
            gpuPassTimings_.toneMappingMs;
        gpuPassTimings_.valid = true;
    }

    frameSlotIndex_++;
}

} // namespace eng::render
