/// @file async_scene_loader.h
/// @brief General-purpose asynchronous scene-loading pipeline and progressive staging architecture.
///
/// Purpose:
/// Provides progressive, non-blocking scene loading across small and massive environments (e.g.
/// Amazon Bistro with 6,000+ entities and hundreds of PBR textures). Prevents UI hangs, eliminates
/// OS window "Not Responding" stalls, supports safe cancellation, and maintains continuous engine
/// simulation during asset ingestion.
///
/// Architecture & Contracts:
/// - Maintains exactly ONE authoritative Flecs ECS World. Scene preparation never mutates the
///   active authoritative World; all data is staged CPU-side until a single atomic activation
///   point.
/// - Concurrency via JobSystem: Background workers execute file I/O, glTF parsing, mesh processing,
///   and texture decompression. GPU uploads are budgeted per frame on the main/render thread.
/// - Generational request tokens prevent stale jobs from mutating newer scene load requests.
/// - Structured asset error handling with fallback textures for missing optional assets.
///
/// Ownership and Lifetime:
/// Owned by the application runtime or workspace manager. Backing job system and asset manager
/// are referenced non-owning and must outlive active loading requests.
///
/// Thread Safety:
/// Public inquiry methods (progress, state, telemetry) and cancel() are thread-safe.
/// update() and activate() must be called on the main/UI application thread.

#pragma once

#include "core/error.h"
#include "core/job_system.h"
#include "core/math.h"
#include "core/result.h"
#include "core/time/duration.h"
#include "core/time/timestamp.h"
#include "framework/scene.h"
#include "render/graphics_settings.h"
#include "render/post_process_types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eng::world {
class World;
struct EntityHandle;
} // namespace eng::world

namespace eng::rhi {
class RhiDevice;
} // namespace eng::rhi

namespace eng::render {
class AssetManager;
class SceneRenderer;
} // namespace eng::render

namespace eng::framework {

/// @enum AsyncLoadState
/// @brief Progressive execution phases of an asynchronous scene load following the strict 10-phase
/// pipeline.
enum class AsyncLoadState : uint8_t {
    Idle = 0,              ///< No load request active.
    SceneRequest = 1,      ///< Phase 1: Request validation and token initialization.
    FileReading = 2,       ///< Phase 2: Reading scene descriptor from storage into CPU memory.
    ReadingFile = 2,       ///< Backward-compatible alias for Phase 2.
    AssetDiscovery = 3,    ///< Phase 3: Parsing hierarchy and collecting asset manifest.
    DiscoveringAssets = 3, ///< Backward-compatible alias for Phase 3.
    AssetDecoding = 4,     ///< Phase 4: Parallel worker decoding of images and textures.
    DecodingAssets = 4,    ///< Backward-compatible alias for Phase 4.
    CpuStaging = 5,        ///< Phase 5: Processing mesh geometry and staging entity hierarchy.
    GpuUpload = 6,         ///< Phase 6: Budgeted main-thread GPU buffer and texture uploads.
    UploadingGpu = 6,      ///< Backward-compatible alias for Phase 6.
    SceneValidation = 7,   ///< Phase 7: Verifying scene integrity and resolving asset fallbacks.
    Validating = 7,        ///< Backward-compatible alias for Phase 7.
    ReadyToActivate = 8,   ///< All assets staged and uploaded; ready for main-thread activation.
    EcsCommit = 9,         ///< Phase 8: Transactional commit into authoritative Flecs World.
    Activating = 9,        ///< Backward-compatible alias for Phase 8.
    ViewportPreparation =
        10,          ///< Phase 9: Propagating transforms, camera framing, and post-processing.
    SceneReady = 11, ///< Phase 10: Scene successfully activated and operational.
    Completed = 11,  ///< Backward-compatible alias for Phase 10.
    Failed = 12,     ///< Scene loading failed with a non-recoverable error.
    Cancelled = 13   ///< Scene load was safely cancelled by user or newer request.
};

/// @brief Converts AsyncLoadState to a stable string literal.
[[nodiscard]] constexpr std::string_view toString(AsyncLoadState state) noexcept {
    switch (state) {
    case AsyncLoadState::Idle:
        return "IDLE";
    case AsyncLoadState::SceneRequest:
        return "SCENE_REQUEST";
    case AsyncLoadState::FileReading:
        return "FILE_READING";
    case AsyncLoadState::AssetDiscovery:
        return "ASSET_DISCOVERY";
    case AsyncLoadState::AssetDecoding:
        return "ASSET_DECODING";
    case AsyncLoadState::CpuStaging:
        return "CPU_STAGING";
    case AsyncLoadState::GpuUpload:
        return "GPU_UPLOAD";
    case AsyncLoadState::SceneValidation:
        return "SCENE_VALIDATION";
    case AsyncLoadState::ReadyToActivate:
        return "READY_TO_ACTIVATE";
    case AsyncLoadState::EcsCommit:
        return "ECS_COMMIT";
    case AsyncLoadState::ViewportPreparation:
        return "VIEWPORT_PREPARATION";
    case AsyncLoadState::SceneReady:
        return "SCENE_READY";
    case AsyncLoadState::Failed:
        return "FAILED";
    case AsyncLoadState::Cancelled:
        return "CANCELLED";
    }
    return "UNKNOWN";
}

/// @brief Converts AsyncLoadState to a user-facing formatted phase title.
[[nodiscard]] constexpr std::string_view toPhaseDisplayName(AsyncLoadState state) noexcept {
    switch (state) {
    case AsyncLoadState::Idle:
        return "Idle";
    case AsyncLoadState::SceneRequest:
        return "Phase 1/10: Scene Request";
    case AsyncLoadState::FileReading:
        return "Phase 2/10: File Reading";
    case AsyncLoadState::AssetDiscovery:
        return "Phase 3/10: Asset Discovery";
    case AsyncLoadState::AssetDecoding:
        return "Phase 4/10: Asset Decoding";
    case AsyncLoadState::CpuStaging:
        return "Phase 5/10: CPU Processing & Staging";
    case AsyncLoadState::GpuUpload:
        return "Phase 6/10: Incremental GPU Upload";
    case AsyncLoadState::SceneValidation:
        return "Phase 7/10: Scene Validation";
    case AsyncLoadState::ReadyToActivate:
        return "Phase 8/10: Ready to Commit";
    case AsyncLoadState::EcsCommit:
        return "Phase 8/10: Safe ECS Commit";
    case AsyncLoadState::ViewportPreparation:
        return "Phase 9/10: Viewport Preparation";
    case AsyncLoadState::SceneReady:
        return "Phase 10/10: Scene Ready";
    case AsyncLoadState::Failed:
        return "Loading Failed";
    case AsyncLoadState::Cancelled:
        return "Loading Cancelled";
    }
    return "Processing";
}

/// @struct AssetErrorInfo
/// @brief Structured diagnostics for asset ingestion failures.
struct AssetErrorInfo {
    std::string path{};
    std::string assetType{};
    std::string reason{};
    bool isOptional{true};
};

/// @struct AsyncLoadProgress
/// @brief Live progress metrics reflecting actual completed workload across all 10 loading phases.
struct AsyncLoadProgress {
    AsyncLoadState state{AsyncLoadState::Idle};
    std::string sceneName{};
    std::string currentOperation{};
    float overallProgress{0.0F};    ///< Normalized [0.0, 1.0] aggregate completion.
    float discoveryProgress{0.0F};  ///< [0.0, 1.0]
    float textureProgress{0.0F};    ///< [0.0, 1.0]
    float meshProgress{0.0F};       ///< [0.0, 1.0]
    float gpuUploadProgress{0.0F};  ///< [0.0, 1.0]
    float activationProgress{0.0F}; ///< [0.0, 1.0]

    uint32_t entitiesDiscovered{0};
    uint32_t entitiesLoaded{0};
    uint32_t meshesTotal{0};
    uint32_t meshesDecoded{0};
    uint32_t meshesUploaded{0};
    uint32_t materialsTotal{0};
    uint32_t texturesTotal{0};
    uint32_t texturesDecoded{0};
    uint32_t texturesUploaded{0};
    uint32_t texturesFailed{0};

    size_t peakRamBytes{0};
    size_t peakVramBytes{0};
};

/// @struct AsyncLoadTelemetry
/// @brief Fine-grained performance accounting recorded across all 10 scene loading phases.
struct AsyncLoadTelemetry {
    Duration sceneRequestDuration{};  ///< Phase 1: Request initiation and token generation.
    Duration fileIoDuration{};        ///< Phase 2: Scene file reading from persistent storage.
    Duration discoveryDuration{};     ///< Phase 3: Hierarchy parsing and asset manifest discovery.
    Duration textureDecodeDuration{}; ///< Phase 4: Parallel worker texture decoding.
    Duration cpuStagingDuration{};    ///< Phase 5: Mesh geometry processing and CPU staging.
    Duration meshProcessDuration{};   ///< Phase 5: Compatible alias for mesh processing duration.
    Duration gpuUploadDuration{};     ///< Phase 6: Budgeted main-thread GPU resource upload.
    Duration validationDuration{};    ///< Phase 7: Staged scene validation and fallback checks.
    Duration ecsCreationDuration{};   ///< Phase 8: Authoritative World entity commit.
    Duration viewportPrepDuration{};  ///< Phase 9: Hierarchy matrices and camera framing setup.
    Duration totalDuration{};         ///< Phase 10: Aggregate duration from request to scene ready.
    Duration longestFrameStall{};     ///< Longest single frame duration during loading.
    uint32_t totalAllocations{0};
    uint32_t failedAssetCount{0};
};

/// @struct SceneActivationResult
/// @brief Payload delivered upon successful scene activation.
struct SceneActivationResult {
    uint32_t entitiesLoaded{0};
    std::optional<Vec3> cameraPosition{std::nullopt};
    std::optional<Vec3> cameraTarget{std::nullopt};
    std::optional<render::PostProcessSettings> postProcessSettings{std::nullopt};
    std::optional<render::GraphicsSettings> graphicsSettings{std::nullopt};
};

/// @class AsyncSceneLoader
/// @brief Non-blocking, progressive scene loading pipeline with thread-safe staging and
/// cancellation.
class AsyncSceneLoader {
public:
    AsyncSceneLoader();
    ~AsyncSceneLoader();

    AsyncSceneLoader(const AsyncSceneLoader&) = delete;
    AsyncSceneLoader& operator=(const AsyncSceneLoader&) = delete;
    AsyncSceneLoader(AsyncSceneLoader&&) noexcept;
    AsyncSceneLoader& operator=(AsyncSceneLoader&&) noexcept;

    /// @brief Binds the background JobSystem used for parallel decoding.
    void bindJobSystem(JobSystem* jobSystem) noexcept;

    /// @brief Binds the central AssetManager caching uploaded textures and meshes.
    void bindAssetManager(render::AssetManager* assetManager) noexcept;

    /// @brief Initiates an asynchronous scene load request for .hikari, .gltf, or .glb files.
    ///
    /// If an active load is currently running, it is safely cancelled before starting the new
    /// request.
    ///
    /// @param filepath Absolute or relative path to the scene asset.
    /// @return Success or error details if file does not exist.
    [[nodiscard]] Result<uint64_t, Error> startLoad(std::string_view filepath);

    /// @brief Safely requests cancellation of the current loading operation.
    ///
    /// Background jobs exit early, temporary staging buffers are reclaimed, and the active World
    /// is left completely unmodified.
    void cancel() noexcept;

    /// @brief Main-thread per-frame tick performing incremental GPU resource uploads.
    ///
    /// Respects the specified time budget (in milliseconds) to ensure frame-time consistency.
    ///
    /// @param device RHI device for vertex buffer and texture uploads.
    /// @param budgetMs Maximum time budget allocated for GPU uploads this frame (default: 3.0 ms).
    void update(rhi::RhiDevice& device, float budgetMs = 3.0F);

    /// @brief Activates the staged scene into the authoritative World at a safe frame boundary.
    ///
    /// Preconditions : getState() must be AsyncLoadState::ReadyToActivate.
    /// Postconditions: Active user entities in world are replaced with staged entities.
    [[nodiscard]] Result<SceneActivationResult, Error> activate(world::World& world);

    /// @brief Returns the current lifecycle state of the loader.
    [[nodiscard]] AsyncLoadState getState() const noexcept;

    /// @brief Checks if a scene load operation is actively in progress.
    [[nodiscard]] bool isLoading() const noexcept;

    /// @brief Checks if all assets are staged and the scene is ready for immediate activation.
    [[nodiscard]] bool isReadyToActivate() const noexcept;

    /// @brief Returns true if the most recent load completed and the scene is active.
    [[nodiscard]] bool isCompleted() const noexcept;

    /// @brief Returns true if the most recent load failed.
    [[nodiscard]] bool isFailed() const noexcept;

    /// @brief Returns true if the load was cancelled.
    [[nodiscard]] bool isCancelled() const noexcept;

    /// @brief Returns live progress accounting data.
    [[nodiscard]] AsyncLoadProgress getProgress() const noexcept;

    /// @brief Returns performance telemetry for the completed or current load.
    [[nodiscard]] const AsyncLoadTelemetry& getTelemetry() const noexcept;

    /// @brief Returns the error message if the load failed.
    [[nodiscard]] std::string_view getErrorMessage() const noexcept;

    /// @brief Returns structured asset errors encountered during ingestion.
    [[nodiscard]] const std::vector<AssetErrorInfo>& getAssetErrors() const noexcept;

    /// @brief Resets the loader to Idle state.
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eng::framework
