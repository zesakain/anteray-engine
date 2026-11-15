/// @file scene_renderer.h
/// @brief Scene mesh rendering system and RHI command recorder.
///
/// Purpose:
/// Traverses ECS entities with MeshComponent, resolves their GPU mesh buffers via AssetManager,
/// and records draw commands into an RhiCommandList. Transform priority per entity:
///   1. WorldTransformComponent::worldMatrix — set by HierarchyTransformSystem for parented
///   entities.
///   2. TransformComponent — local transform fallback.
///   3. PhysicsTransformComponent — fallback for dynamic rigid bodies at root level.
///   4. Identity matrix — stationary at origin if none of the above components are present.
/// Manages offscreen color and depth render targets, shaders, graphics pipeline, HDR target,
/// and post-processing pipeline (bloom pyramid downsample/upsample and tone mapping).
///
/// Ownership and Lifetime:
/// Owns offscreen render target textures, pipeline layouts, descriptor sets, and pipeline state
/// objects. Destroys all GPU resources upon shutdown() or destruction.
///
/// Thread-safety:
/// Main render thread only. Not re-entrant.
///
/// Performance:
/// Hot-path command recording. Does not allocate heap memory per frame or lock mutexes.

#pragma once

#include "core/math.h"
#include "core/result.h"
#include "render/asset_manager.h"
#include "render/gpu_environment_map.h"
#include "render/graphics_settings.h"
#include "render/irradiance_probe.h"
#include "render/light_components.h"
#include "render/lighting_data.h"
#include "render/mesh_component.h"
#include "render/post_process_types.h"
#include "render/render_graph.h"
#include "render/shadow_cascade.h"
#include "rhi/command_list.h"
#include "rhi/pipeline.h"
#include "rhi/rhi_device.h"
#include "rhi/sampler.h"
#include "rhi/texture.h"
#include "world/world.h"

#include <array>
#include <cstdint>
#include <vector>

namespace eng::render {

/// @struct RenderStats
/// @brief Telemetry metrics collected during scene rendering.
struct RenderStats {
    uint32_t drawCalls{0};             ///< Main forward PBR draw calls
    uint32_t shadowDrawCalls{0};       ///< Directional shadow depth draw calls
    uint32_t depthPrepassDrawCalls{0}; ///< Early depth prepass draw calls
    uint32_t triangles{0};             ///< Triangles rasterized in main PBR pass
    uint32_t shadowTriangles{0};       ///< Triangles rasterized in shadow pass
    uint32_t vertices{0};              ///< Vertices evaluated in main PBR pass
    uint32_t meshesRendered{0};        ///< Mesh entities evaluated and rendered
    uint32_t culledPrimitives{0};      ///< Mesh primitives culled or skipped
};

/// @struct GpuPassTimings
/// @brief Hardware GPU execution duration for individual scene rendering and post-processing
/// passes.
struct GpuPassTimings {
    /// Total duration spanning scene rendering from shadow pass through tone mapping
    /// (milliseconds).
    float totalSceneMs{0.0F};
    /// Duration of directional shadow map generation (milliseconds).
    float shadowPassMs{0.0F};
    /// Duration of early depth prepass (milliseconds).
    float depthPrepassMs{0.0F};
    /// Duration of forward PBR mesh rasterization and material shading (milliseconds).
    float mainPbrPassMs{0.0F};
    /// Duration of bloom downsampling pyramid passes (milliseconds).
    float bloomDownsampleMs{0.0F};
    /// Duration of bloom upsampling and accumulation passes (milliseconds).
    float bloomUpsampleMs{0.0F};
    /// Duration of tone mapping and presentation conversion pass (milliseconds).
    float toneMappingMs{0.0F};
    /// Duration of Ground-Truth Ambient Occlusion (GTAO) pass (milliseconds).
    float gtaoPassMs{0.0F};
    /// Duration of Screen-Space Global Illumination (SSGI) pass (milliseconds).
    float ssgiPassMs{0.0F};
    /// Duration of Screen-Space Reflections (SSR) pass (milliseconds).
    float ssrPassMs{0.0F};
    /// Duration of temporal accumulation and denoising pass (milliseconds).
    float temporalDenoiseMs{0.0F};
    /// Duration of screen-space motion vector / velocity pass (milliseconds).
    float velocityPassMs{0.0F};
    /// Duration of Temporal Anti-Aliasing accumulation pass (milliseconds).
    float taaPassMs{0.0F};
    /// Duration of contrast-adaptive sharpening pass (milliseconds).
    float sharpenPassMs{0.0F};
    /// Duration of Depth of Field (DoF) pass (milliseconds).
    float dofPassMs{0.0F};
    /// Duration of directional Motion Blur pass (milliseconds).
    float motionBlurPassMs{0.0F};
    /// Combined total duration of all post-processing passes (DoF, Motion Blur, Bloom, ToneMap)
    /// (milliseconds).
    float totalPostProcessMs{0.0F};
    /// Combined total duration of all Global Illumination passes (milliseconds).
    float totalGiMs{0.0F};
    /// Whether these timings reflect valid GPU query results from the preceding frame.
    bool valid{false};
};

/// @class SceneRenderer
/// @brief Traverses the ECS World and records mesh draw calls to the RHI command list.
class SceneRenderer {
public:
    static constexpr size_t kBloomPyramidLevels = 4;

    SceneRenderer() = default;
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    SceneRenderer(SceneRenderer&& other) noexcept;
    SceneRenderer& operator=(SceneRenderer&& other) noexcept;

    /// Initializes offscreen render targets, shaders, pipeline layout, and graphics pipeline.
    ///
    /// Preconditions : device must be a valid, initialized RhiDevice; width > 0, height > 0.
    /// Postconditions: color, depth, and HDR targets allocated; graphics pipelines ready for
    /// recording. Thread-safety : Main thread only.
    ///
    /// @param device The active RHI device for resource allocation.
    /// @param width Render target width in pixels.
    /// @param height Render target height in pixels.
    /// @param colorFormat Texture format for color attachment (default RGBA8_UNORM).
    /// @param depthFormat Texture format for depth attachment (default D32_FLOAT).
    /// @return Result indicating success or resource allocation error.
    [[nodiscard]] Result<void>
    init(rhi::RhiDevice& device, uint32_t width, uint32_t height,
         rhi::TextureFormat colorFormat = rhi::TextureFormat::RGBA8_UNORM,
         rhi::TextureFormat depthFormat = rhi::TextureFormat::D32_FLOAT);

    /// Shuts down all offscreen resources, pipelines, and shaders.
    void shutdown();

    /// Resizes offscreen render targets and bloom pyramid to match new viewport dimensions.
    /// Synchronizes GPU with waitIdle before destroying previous targets.
    ///
    /// @param width New width in pixels (must be > 0).
    /// @param height New height in pixels (must be > 0).
    /// @return Result indicating success or reallocation error.
    [[nodiscard]] Result<void> resize(uint32_t width, uint32_t height);

    /// Renders all visible mesh entities in the World to the specified command list.
    /// If post-processing is enabled, renders into the floating-point HDR scene target.
    /// If post-processing is disabled, renders directly into the LDR color target.
    ///
    /// @param world The ECS simulation or edit world.
    /// @param assetManager Repository for GPU mesh buffers.
    /// @param device The active RHI device for on-demand GPU uploads.
    /// @param cmdList Target command list for recording vertex/index binds and draw calls.
    /// @param viewProjection Camera view-projection transformation matrix.
    /// @return Render statistics collected for this pass.
    RenderStats renderScene(world::World& world, AssetManager& assetManager, rhi::RhiDevice& device,
                            rhi::RhiCommandList& cmdList,
                            const Mat4& viewProjection = Mat4::identity());

    /// Executes post-processing passes (bloom pyramid downsample/upsample and tone mapping)
    /// transitioning radiance from the HDR scene target into the display color target.
    ///
    /// Preconditions : cmdList must be recording outside an active render pass.
    /// Thread-safety : Main thread only.
    /// Cost          : Zero heap allocation in hot path.
    ///
    /// @param cmdList Command list recording post-processing passes.
    void renderPostProcess(rhi::RhiCommandList& cmdList);

    /// Asynchronously polls and updates GPU pass duration telemetry from the preceding frame.
    ///
    /// Cost : Zero heap allocation, non-blocking check. Allowed in hot path.
    ///
    /// @param device The active RHI device owning query pool resources.
    void updateGpuTimings(rhi::RhiDevice& device);

    /// Returns the telemetry metrics collected during the most recent scene rendering pass.
    [[nodiscard]] const RenderStats& getLastRenderStats() const noexcept {
        return lastRenderStats_;
    }

    /// Returns the latest measured GPU execution durations for all rendering passes.
    [[nodiscard]] const GpuPassTimings& getGpuPassTimings() const noexcept {
        return gpuPassTimings_;
    }

    [[nodiscard]] rhi::TextureHandle getColorTarget() const noexcept { return colorTarget_; }
    [[nodiscard]] rhi::TextureHandle getDepthTarget() const noexcept { return depthTarget_; }
    [[nodiscard]] rhi::TextureHandle getHdrTarget() const noexcept { return hdrTarget_; }
    [[nodiscard]] rhi::TextureFormat getHdrFormat() const noexcept { return hdrFormat_; }
    /// Returns true if genuine floating-point HDR rendering is active (RGBA16_FLOAT or
    /// RGBA32_FLOAT). Returns false if post-processing is disabled or operating in SDR fallback
    /// mode (RGBA8_UNORM).
    [[nodiscard]] bool isHdrActive() const noexcept {
        return postProcessSettings_.enabled && hdrTarget_.isValid() &&
               (hdrFormat_ == rhi::TextureFormat::RGBA16_FLOAT ||
                hdrFormat_ == rhi::TextureFormat::RGBA32_FLOAT);
    }

    /// Returns true if post-processing is enabled but falling back to an SDR format (e.g.
    /// RGBA8_UNORM).
    [[nodiscard]] bool isSdrFallbackActive() const noexcept {
        return postProcessSettings_.enabled && !isHdrActive();
    }

    /// Returns the central graphics and scalability configuration.
    [[nodiscard]] const GraphicsSettings& getGraphicsSettings() const noexcept { return settings_; }

    /// Validates and applies a modified graphics configuration across renderer subsystems.
    [[nodiscard]] Result<void> applyGraphicsSettings(const GraphicsSettings& settings);

    [[nodiscard]] const PostProcessSettings& getPostProcessSettings() const noexcept {
        return postProcessSettings_;
    }
    void setPostProcessSettings(const PostProcessSettings& settings) noexcept;
    [[nodiscard]] bool isPostProcessingEnabled() const noexcept {
        return postProcessSettings_.enabled;
    }
    void setPostProcessingEnabled(bool enabled) noexcept {
        postProcessSettings_.enabled = enabled;
        updatePostProcessDescriptors();
        buildPostProcessGraph();
        buildRenderGraph();
    }

    /// @brief Generates a neutral identity 256x16 RGBA8 unwrapped 3D LUT strip (16x16x16).
    [[nodiscard]] static ImageData createIdentityColorGradingLut();

    /// @brief Uploads a 256x16 RGBA8 color grading LUT strip texture to the GPU.
    [[nodiscard]] Result<void> loadColorGradingLut(const ImageData& lutImage);

    /// @brief Loads a 256x16 RGBA8 color grading LUT strip from disk and binds it for
    /// post-processing.
    [[nodiscard]] Result<void> loadColorGradingLutFromFile(std::string_view filePath);

    /// @brief Clears any custom color grading LUT and reverts to the neutral identity LUT.
    void clearColorGradingLut() noexcept;

    /// @brief Returns true if a custom color grading LUT texture is currently loaded.
    [[nodiscard]] bool hasCustomColorGradingLut() const noexcept {
        return customLutTexture_ != nullptr && customLutTexture_->isValid();
    }

    /// @brief Updates temporal exposure adaptation from measured or estimated scene luminance.
    ///
    /// Cost: Zero heap allocation, O(1) scalar math. Safe in hot path.
    /// @param sceneLuminance Average logarithmic/photometric scene luminance (> 0).
    /// @param dtSeconds Frame delta time in seconds.
    void updateExposureAdaptation(float sceneLuminance, float dtSeconds) noexcept;

    /// @brief Returns the current temporally-adapted exposure multiplier (including EV
    /// compensation).
    [[nodiscard]] float getAdaptedExposure() const noexcept { return adaptedExposure_; }

    /// @brief Resets temporal exposure adaptation state to the current manual exposure baseline.
    void resetExposureAdaptation() noexcept {
        adaptedLuminance_ = 0.18F;
        adaptedExposure_ =
            postProcessSettings_.exposure * std::exp2(postProcessSettings_.exposureCompensation);
    }

    [[nodiscard]] const GpuEnvironmentMap& getEnvironmentMap() const noexcept { return envMap_; }
    void setEnvironmentMap(GpuEnvironmentMap&& envMap);
    [[nodiscard]] Result<void> loadEnvironmentMap(const HdrImage& hdr);
    [[nodiscard]] Result<void> loadEnvironmentMapFromFile(std::string_view filePath);

    /// @brief Applies atmospheric lighting condition (daylight, sunset, night), generating or
    /// binding corresponding IBL cubemaps and synchronizing tone mapping color grading.
    [[nodiscard]] Result<void> applyAtmosphere(const AtmosphereParameters& params);

    /// @brief Applies atmospheric lighting condition to the renderer and synchronizes any
    /// DirectionalLightComponent present in the World, or creates one if absent.
    [[nodiscard]] Result<void> applyAtmosphere(world::World& world,
                                               const AtmosphereParameters& params);

    /// @brief Returns current active atmospheric lighting parameters.
    [[nodiscard]] const AtmosphereParameters& getAtmosphereParameters() const noexcept {
        return atmosphereParams_;
    }
    [[nodiscard]] bool isIblEnabled() const noexcept { return settings_.lighting.iblEnabled; }
    void setIblEnabled(bool enabled) noexcept { settings_.lighting.iblEnabled = enabled; }
    [[nodiscard]] float getIblIntensity() const noexcept {
        return settings_.lighting.environmentIntensity;
    }
    void setIblIntensity(float intensity) noexcept {
        settings_.lighting.environmentIntensity = intensity;
    }

    [[nodiscard]] rhi::PipelineHandle getPipeline() const noexcept { return pipeline_; }
    [[nodiscard]] rhi::PipelineLayoutHandle getPipelineLayout() const noexcept {
        return pipelineLayout_;
    }
    /// Returns the cached descriptor set layout for glTF materials (Set 0).
    ///
    /// Preconditions : SceneRenderer must be initialized.
    /// Thread-safety : Safe to call concurrently from any thread after init().
    /// Cost          : Free (no heap allocation, returns value handle).
    [[nodiscard]] rhi::DescriptorSetLayoutHandle getMaterialDescriptorSetLayout() const noexcept {
        return materialDescriptorSetLayout_;
    }
    [[nodiscard]] rhi::DescriptorSetLayoutHandle getSceneDescriptorSetLayout() const noexcept {
        return sceneDescriptorSetLayout_;
    }
    [[nodiscard]] rhi::TextureHandle getShadowDepthTarget() const noexcept {
        return shadowDepthTarget_;
    }
    [[nodiscard]] rhi::SamplerHandle getShadowSampler() const noexcept { return shadowSampler_; }
    [[nodiscard]] rhi::PipelineHandle getShadowPipeline() const noexcept { return shadowPipeline_; }
    /// Forces re-recording of the directional shadow depth pass on the next renderScene call.
    /// Thread-safety : Main thread only.
    void invalidateShadowMap() noexcept {
        shadowDirty_ = true;
        staticShadowDirty_ = true;
    }
    [[nodiscard]] rhi::BufferHandle getLightingUniformBuffer() const noexcept {
        return lightingUniformBuffer_;
    }
    [[nodiscard]] uint32_t getShadowResolution() const noexcept { return shadowResolution_; }
    void setShadowResolution(uint32_t resolution) noexcept {
        shadowResolution_ = resolution;
        shadowDirty_ = true;
        staticShadowDirty_ = true;
    }
    void markShadowDirty() noexcept {
        shadowDirty_ = true;
        staticShadowDirty_ = true;
    }
    [[nodiscard]] bool isShadowDirty() const noexcept { return shadowDirty_; }
    [[nodiscard]] bool isStaticShadowDirty() const noexcept { return staticShadowDirty_; }
    [[nodiscard]] rhi::TextureHandle getStaticShadowDepthTarget() const noexcept {
        return staticShadowTarget_;
    }
    [[nodiscard]] uint32_t getWidth() const noexcept { return width_; }
    [[nodiscard]] uint32_t getHeight() const noexcept { return height_; }
    [[nodiscard]] bool isInitialized() const noexcept {
        return device_ != nullptr && pipeline_.isValid();
    }

    /// Returns the live scene DAG render graph.
    [[nodiscard]] const RenderGraph& getRenderGraph() const noexcept { return renderGraph_; }

    /// Returns the live post-processing DAG render graph.
    [[nodiscard]] const RenderGraph& getPostProcessRenderGraph() const noexcept {
        return postProcessGraph_;
    }

    [[nodiscard]] rhi::TextureHandle getGtaoTarget() const noexcept { return gtaoFilteredTarget_; }
    [[nodiscard]] rhi::TextureHandle getGtaoRawTarget() const noexcept { return gtaoRawTarget_; }
    [[nodiscard]] rhi::TextureHandle getSsgiTarget() const noexcept { return ssgiFilteredTarget_; }
    [[nodiscard]] rhi::TextureHandle getVelocityTarget() const noexcept { return velocityTarget_; }
    [[nodiscard]] rhi::TextureHandle getTaaHistoryTarget() const noexcept {
        return hdrHistoryTarget_[frameSlotIndex_ % 2];
    }
    [[nodiscard]] bool isTaaEnabled() const noexcept {
        return postProcessSettings_.enabled && hdrTarget_.isValid() &&
               settings_.quality.antiAliasing == AntiAliasingMode::TAA &&
               settings_.quality.taa.enabled;
    }
    /// Invalidates temporal history accumulation (e.g. upon camera cuts, teleports, or resize).
    void invalidateTaaHistory() noexcept { isTaaHistoryValid_ = false; }
    [[nodiscard]] const std::vector<ShadowCascade>& getShadowCascades() const noexcept {
        return shadowCascades_;
    }

    struct GtaoPushConstants {
        Vec4 lightDirAndContact{0.5F, 0.8F, 0.5F, 0.6F};
        Vec4 cameraPosAndRadius{0.0F, 0.0F, 0.0F, 1.5F};
        Vec4 gtaoParams{1.0F, 0.05F, 1.0F, 1.0F};
        Vec4 screenAndProj{1920.0F, 1080.0F, 1.0F, 1.0F};
        Mat4 invViewProj{Mat4::identity()};
    };
    static_assert(sizeof(GtaoPushConstants) == 128, "GtaoPushConstants must be exactly 128 bytes");

    struct BilateralBlurPushConstants {
        Vec2 blurDirection{1.0F, 0.0F};
        Vec2 invResolution{1.0F / 1920.0F, 1.0F / 1080.0F};
        float depthThreshold{0.05F};
        float pad0{0.0F};
    };

    struct SsgiPushConstants {
        Mat4 reprojectMat{Mat4::identity()};
        Vec4 projParams{1.0F, 1.0F, 0.0F, 0.0F};
        Vec4 ssgiParams{1.0F, 8.0F, 0.2F, 3.0F};
        Vec2 resolution{960.0F, 540.0F};
        Vec2 invResolution{1.0F / 960.0F, 1.0F / 540.0F};
        uint32_t frameIndex{0};
        float pad0{0.0F};
    };

    struct SsrPushConstants {
        Mat4 reprojectMat{Mat4::identity()};
        Vec4 projParams{1.0F, 1.0F, 1.0F, 1.0F};
        Vec4 ssrParams{10.0F, 16.0F, 0.2F, 0.6F};
        Vec4 cameraPosAndIntensity{0.0F, 0.0F, 0.0F, 1.0F};
        Vec2 resolution{960.0F, 540.0F};
        Vec2 invResolution{1.0F / 960.0F, 1.0F / 540.0F};
    };

    struct TemporalFilterPushConstants {
        Mat4 reprojectMat{Mat4::identity()};
        Vec4 filterParams{0.1F, 0.05F, 0.0F, 0.0F};
        Vec2 resolution{960.0F, 540.0F};
        Vec2 invResolution{1.0F / 960.0F, 1.0F / 540.0F};
        uint32_t isHistoryValid{0};
        float pad0{0.0F};
    };

    // Milestone 11H: Motion Vectors, TAA, and Sharpening Push Constants
    struct VelocityPushConstants {
        Mat4 reprojectMatrix{Mat4::identity()}; // unjitteredPrevViewProj * unjitteredInvViewProj
        Vec2 resolution{1920.0F, 1080.0F};
        Vec2 invResolution{1.0F / 1920.0F, 1.0F / 1080.0F};
        uint32_t hasHistory{0};
        float pad0{0.0F};
        float pad1{0.0F};
        float pad2{0.0F};
    };
    static_assert(sizeof(VelocityPushConstants) == 96,
                  "VelocityPushConstants must be exactly 96 bytes");

    struct MeshVelocityPushConstants {
        Mat4 curMvp{Mat4::identity()};
        Mat4 prevMvp{Mat4::identity()};
    };
    static_assert(sizeof(MeshVelocityPushConstants) == 128,
                  "MeshVelocityPushConstants must be exactly 128 bytes");

    struct TaaPushConstants {
        Vec4 taaParams{0.90F, 1.25F, 0.0F, 0.0F}; // historyWeight, varianceGamma, pad, pad
        Vec2 resolution{1920.0F, 1080.0F};
        Vec2 invResolution{1.0F / 1920.0F, 1.0F / 1080.0F};
        uint32_t isHistoryValid{0};
        uint32_t useMotionVectors{1};
        float pad0{0.0F};
        float pad1{0.0F};
    };
    static_assert(sizeof(TaaPushConstants) == 48, "TaaPushConstants must be exactly 48 bytes");

    struct SharpenPushConstants {
        Vec2 invResolution{1.0F / 1920.0F, 1.0F / 1080.0F};
        float intensity{0.50F};
        float pad0{0.0F};
    };
    static_assert(sizeof(SharpenPushConstants) == 16,
                  "SharpenPushConstants must be exactly 16 bytes");

private:
    struct ShadowCasterItem {
        GpuMesh* gpuMesh{nullptr};
        Mat4 model{Mat4::identity()};
        uint32_t submeshIndex{0};
        AABB worldBounds{};
        float distanceToLight{0.0F};
        bool isDynamic{false};
    };

    [[nodiscard]] Result<void> initPostProcessResources();
    [[nodiscard]] Result<void> createPostProcessTargets(uint32_t width, uint32_t height);
    void destroyPostProcessTargets();
    void updatePostProcessDescriptors();

    [[nodiscard]] Result<void> initGiResources();
    void destroyGiResources();
    [[nodiscard]] Result<void> createGiTargets(uint32_t width, uint32_t height);
    void destroyGiTargets();
    void updateGiDescriptors();

    // Milestone 11H: Motion Vectors, TAA, and Sharpening Resource Management
    [[nodiscard]] Result<void> initTaaResources();
    void destroyTaaResources();
    [[nodiscard]] Result<void> createTaaTargets(uint32_t width, uint32_t height);
    void destroyTaaTargets();
    void updateTaaDescriptors();
    void recordVelocityPass(rhi::RhiCommandList& cmdList);
    void recordTaaPass(rhi::RhiCommandList& cmdList);
    void recordSharpenPass(rhi::RhiCommandList& cmdList);

    void buildRenderGraph();
    void buildPostProcessGraph();
    void recordShadowPass(rhi::RhiCommandList& cmdList);
    void renderCasters(rhi::RhiCommandList& cmdList, const std::vector<ShadowCasterItem>& casters);
    void recordDepthPrepass(rhi::RhiCommandList& cmdList);
    void recordGtaoPass(rhi::RhiCommandList& cmdList);
    void recordGtaoBlurPass(rhi::RhiCommandList& cmdList);
    void recordSsgiPass(rhi::RhiCommandList& cmdList);
    void recordSsrPass(rhi::RhiCommandList& cmdList);
    void recordTemporalFilterPass(rhi::RhiCommandList& cmdList);
    void recordSsrTemporalFilterPass(rhi::RhiCommandList& cmdList);
    void recordHistoryCopyPass(rhi::RhiCommandList& cmdList);
    void recordMainPbrPass(rhi::RhiCommandList& cmdList);
    void recordDofPass(rhi::RhiCommandList& cmdList);
    void recordMotionBlurPass(rhi::RhiCommandList& cmdList);
    void recordBloomDownsamplePass(rhi::RhiCommandList& cmdList, size_t level);
    void recordBloomUpsamplePass(rhi::RhiCommandList& cmdList, size_t step, size_t targetLevel);
    void recordToneMappingPass(rhi::RhiCommandList& cmdList);

    rhi::RhiDevice* device_{nullptr};
    rhi::TextureHandle colorTarget_{};
    rhi::TextureHandle depthTarget_{};
    rhi::ShaderHandle vertexShader_{};
    rhi::ShaderHandle fragmentShader_{};
    rhi::ShaderHandle fragmentShaderOpaque_{};
    rhi::DescriptorSetLayoutHandle materialDescriptorSetLayout_{};
    rhi::DescriptorSetLayoutHandle sceneDescriptorSetLayout_{};
    rhi::DescriptorPoolHandle sceneDescriptorPool_{};
    rhi::DescriptorSetHandle sceneDescriptorSet_{};
    rhi::BufferHandle lightingUniformBuffer_{};
    rhi::TextureHandle shadowDepthTarget_{};
    rhi::TextureHandle staticShadowTarget_{};
    rhi::SamplerHandle shadowSampler_{};
    rhi::ShaderHandle shadowVertexShader_{};
    rhi::PipelineLayoutHandle shadowPipelineLayout_{};
    rhi::PipelineHandle shadowPipeline_{};
    rhi::PipelineHandle depthPrepassPipeline_{};
    rhi::PipelineLayoutHandle pipelineLayout_{};
    rhi::PipelineHandle pipeline_{};
    rhi::PipelineHandle pipelineOpaque_{};
    rhi::PipelineHandle pipelineDepthPrepass_{};
    rhi::PipelineHandle pipelineDepthPrepassOpaque_{};
    rhi::PipelineHandle hdrPipelineDepthPrepass_{};
    rhi::PipelineHandle hdrPipelineDepthPrepassOpaque_{};

    // IBL Environment Resources
    GpuEnvironmentMap envMap_{};

    // HDR and Post-Processing Resources
    rhi::TextureHandle hdrTarget_{};
    rhi::TextureHandle postProcessTempTarget_[2]{};
    rhi::TextureFormat hdrFormat_{rhi::TextureFormat::RGBA16_FLOAT};
    PostProcessSettings postProcessSettings_{};
    rhi::TextureHandle bloomDownTarget_[kBloomPyramidLevels]{};
    rhi::TextureHandle bloomUpTarget_[kBloomPyramidLevels - 1]{};
    rhi::SamplerHandle postProcessSampler_{};
    rhi::ShaderHandle fullscreenVertexShader_{};
    rhi::ShaderHandle dofFragmentShader_{};
    rhi::ShaderHandle motionBlurFragmentShader_{};
    rhi::ShaderHandle bloomDownFragmentShader_{};
    rhi::ShaderHandle bloomUpFragmentShader_{};
    rhi::ShaderHandle toneMappingFragmentShader_{};
    rhi::DescriptorSetLayoutHandle singleTextureSetLayout_{};
    rhi::DescriptorSetLayoutHandle dualTextureSetLayout_{};
    rhi::DescriptorPoolHandle postProcessDescriptorPool_{};
    rhi::DescriptorSetHandle dofSet_{};
    rhi::DescriptorSetHandle motionBlurSet_{};
    rhi::DescriptorSetHandle bloomDownSets_[kBloomPyramidLevels]{};
    rhi::DescriptorSetHandle bloomUpSets_[kBloomPyramidLevels - 1]{};
    rhi::DescriptorSetHandle toneMappingSet_{};
    rhi::PipelineLayoutHandle dofPipelineLayout_{};
    rhi::PipelineLayoutHandle motionBlurPipelineLayout_{};
    rhi::PipelineLayoutHandle bloomDownPipelineLayout_{};
    rhi::PipelineLayoutHandle bloomUpPipelineLayout_{};
    rhi::PipelineLayoutHandle toneMappingPipelineLayout_{};
    rhi::PipelineHandle dofPipeline_{};
    rhi::PipelineHandle motionBlurPipeline_{};
    rhi::PipelineHandle bloomDownPipeline_{};
    rhi::PipelineHandle bloomUpPipeline_{};
    rhi::PipelineHandle toneMappingPipeline_{};
    std::unique_ptr<GpuTexture> defaultLutTexture_{};
    std::unique_ptr<GpuTexture> customLutTexture_{};
    std::string loadedLutPath_{};
    float adaptedLuminance_{0.18F};
    float adaptedExposure_{1.0F};
    rhi::PipelineHandle hdrPipeline_{};
    rhi::PipelineHandle hdrPipelineOpaque_{};
    rhi::PipelineHandle pipelineDoubleSided_{};
    rhi::PipelineHandle hdrPipelineDoubleSided_{};
    rhi::PipelineHandle pipelineTransparent_{};
    rhi::PipelineHandle pipelineTransparentDoubleSided_{};
    rhi::PipelineHandle hdrPipelineTransparent_{};
    rhi::PipelineHandle hdrPipelineTransparentDoubleSided_{};

    // Global Illumination (Milestone 11D) Resources
    rhi::TextureHandle gtaoRawTarget_{};
    rhi::TextureHandle gtaoFilteredTarget_{};
    rhi::TextureHandle ssgiRawTarget_{};
    rhi::TextureHandle ssgiFilteredTarget_{};
    rhi::TextureHandle ssgiHistoryTarget_[2]{};
    rhi::TextureHandle ssrRawTarget_{};
    rhi::TextureHandle ssrFilteredTarget_{};
    rhi::TextureHandle ssrHistoryTarget_[2]{};
    rhi::TextureHandle hdrHistoryTarget_[2]{};
    std::unique_ptr<GpuTexture> defaultAoTexture_{};
    std::unique_ptr<GpuTexture> defaultIndirectTexture_{};
    std::unique_ptr<GpuTexture> defaultSsrTexture_{};
    rhi::SamplerHandle linearClampSampler_{};
    rhi::SamplerHandle pointClampSampler_{};

    rhi::ShaderHandle gtaoFragmentShader_{};
    rhi::ShaderHandle bilateralBlurFragmentShader_{};
    rhi::ShaderHandle ssgiFragmentShader_{};
    rhi::ShaderHandle ssrFragmentShader_{};
    rhi::ShaderHandle temporalFilterFragmentShader_{};

    rhi::DescriptorSetLayoutHandle tripleTextureSetLayout_{};
    rhi::DescriptorPoolHandle giDescriptorPool_{};
    rhi::DescriptorSetHandle gtaoDescriptorSet_{};
    rhi::DescriptorSetHandle gtaoBlurDescriptorSet_{};
    rhi::DescriptorSetHandle ssgiDescriptorSet_{};
    rhi::DescriptorSetHandle ssrDescriptorSet_{};
    rhi::DescriptorSetHandle temporalFilterDescriptorSet_{};
    rhi::DescriptorSetHandle ssrTemporalFilterDescriptorSet_{};

    rhi::PipelineLayoutHandle gtaoPipelineLayout_{};
    rhi::PipelineLayoutHandle gtaoBlurPipelineLayout_{};
    rhi::PipelineLayoutHandle ssgiPipelineLayout_{};
    rhi::PipelineLayoutHandle ssrPipelineLayout_{};
    rhi::PipelineLayoutHandle temporalFilterPipelineLayout_{};

    rhi::PipelineHandle gtaoPipeline_{};
    rhi::PipelineHandle gtaoBlurPipeline_{};
    rhi::PipelineHandle ssgiPipeline_{};
    rhi::PipelineHandle ssrPipeline_{};
    rhi::PipelineHandle temporalFilterPipeline_{};

    // Milestone 11H: Motion Vectors, TAA, and Sharpening Resources
    rhi::TextureHandle velocityTarget_{};
    rhi::TextureHandle hdrRawTarget_{};
    rhi::ShaderHandle velocityFragmentShader_{};
    rhi::ShaderHandle meshVelocityVertexShader_{};
    rhi::ShaderHandle meshVelocityFragmentShader_{};
    rhi::ShaderHandle taaFragmentShader_{};
    rhi::ShaderHandle sharpenFragmentShader_{};

    rhi::DescriptorSetLayoutHandle velocitySetLayout_{};
    rhi::DescriptorSetLayoutHandle taaSetLayout_{};
    rhi::DescriptorSetLayoutHandle sharpenSetLayout_{};
    rhi::DescriptorPoolHandle taaDescriptorPool_{};
    rhi::DescriptorSetHandle velocityDescriptorSet_{};
    rhi::DescriptorSetHandle taaDescriptorSet_[2]{};
    rhi::DescriptorSetHandle sharpenDescriptorSet_[2]{};

    rhi::PipelineLayoutHandle velocityPipelineLayout_{};
    rhi::PipelineLayoutHandle meshVelocityPipelineLayout_{};
    rhi::PipelineLayoutHandle taaPipelineLayout_{};
    rhi::PipelineLayoutHandle sharpenPipelineLayout_{};

    rhi::PipelineHandle velocityPipeline_{};
    rhi::PipelineHandle meshVelocityPipeline_{};
    rhi::PipelineHandle taaPipeline_{};
    rhi::PipelineHandle sharpenPipeline_{};

    Mat4 unjitteredViewProj_{Mat4::identity()};
    Mat4 unjitteredPrevViewProj_{Mat4::identity()};
    uint32_t taaJitterPhase_{0};
    bool isTaaHistoryValid_{false};

    struct EntityTransformCache {
        uint64_t entityId{0};
        Mat4 model{Mat4::identity()};
    };
    std::vector<EntityTransformCache> prevEntityTransforms_{};

    Mat4 prevViewProj_{Mat4::identity()};
    bool isHistoryValid_{false};
    uint32_t giFrameIndex_{0};

    uint32_t width_{0};
    uint32_t height_{0};
    uint32_t shadowResolution_{2048};
    rhi::TextureFormat colorFormat_{rhi::TextureFormat::RGBA8_UNORM};
    rhi::TextureFormat depthFormat_{rhi::TextureFormat::D32_FLOAT};
    GraphicsSettings settings_{};
    rhi::PipelineHandle wireframePipeline_{};
    rhi::PipelineHandle wireframeLdrPipeline_{};
    RenderStats lastRenderStats_{};
    GpuPassTimings gpuPassTimings_{};
    uint32_t frameSlotIndex_{0};

    // Live RenderGraph DAGs and per-frame recording state
    RenderGraph renderGraph_{};
    RenderGraph postProcessGraph_{};
    rhi::RhiDevice* currentDevice_{nullptr};
    world::World* currentWorld_{nullptr};
    AssetManager* currentAssetManager_{nullptr};
    Mat4 currentViewProj_{Mat4::identity()};
    Vec3 currentCameraPos_{0.0F, 0.0F, 0.0F};
    Mat4 currentLightSpaceMatrix_{Mat4::identity()};
    Vec3 currentLightDir_{Vec3::unitY()};
    uint32_t currentBaseQuery_{0};
    bool currentHasTimestamps_{false};

    struct VisibleMeshItem {
        GpuMesh* gpuMesh{nullptr};
        rhi::DescriptorSetHandle materialSet{};
        Mat4 model{Mat4::identity()};
        Mat4 prevModel{Mat4::identity()};
        uint32_t submeshIndex{0};
        float depthSq{0.0F};
        bool isAlphaMasked{false};
        bool isDoubleSided{false};
        bool isTransparent{false};
        bool hasValidPrevTransform{false};
    };

    std::vector<VisibleMeshItem> visibleItems_{};
    std::vector<ShadowCasterItem> shadowCasters_{};
    std::vector<ShadowCasterItem> staticShadowCasters_{};
    std::vector<ShadowCasterItem> dynamicShadowCasters_{};
    std::vector<ShadowCascade> shadowCascades_{};
    std::array<Mat4, 4> lastCascadeViewProj_{};
    bool shadowDirty_{true};
    bool staticShadowDirty_{true};
    Vec3 lastLightDir_{Vec3::zero()};
    AtmosphereParameters atmosphereParams_{AtmosphereParameters::createDaylight()};
    Mat4 lastLightSpaceMatrix_{Mat4::identity()};
    world::World* lastWorld_{nullptr};
    uint32_t lastCasterCount_{0};
    uint32_t lastStaticCasterCount_{0};
    uint32_t lastDynamicCasterCount_{0};
    uint64_t lastCasterChecksum_{0};
    uint64_t lastStaticCasterChecksum_{0};
    uint64_t lastDynamicCasterChecksum_{0};
};

} // namespace eng::render
