/// @file async_scene_loader.cpp
/// @brief Implementation of general-purpose asynchronous scene loader and progressive staging
/// pipeline.

#include "framework/async_scene_loader.h"

#include "anim/animation_components.h"
#include "core/assert.h"
#include "core/log.h"
#include "framework/scene_serializer.h"
#include "physics/physics_components.h"
#include "render/asset_manager.h"
#include "render/asset_types.h"
#include "render/camera_component.h"
#include "render/gltf_scene_importer.h"
#include "render/image.h"
#include "render/irradiance_probe.h"
#include "render/light_components.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/mesh_component.h"
#include "rhi/rhi_device.h"
#include "script/script_components.h"
#include "world/components.h"
#include "world/hierarchy_systems.h"
#include "world/world.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace eng::framework {

namespace {

struct StagedTextureItem {
    std::string cacheKey{};
    std::string filePath{};
    bool srgb{true};
    bool isOptional{true};
    std::optional<render::ImageData> decodedImage{std::nullopt};
    bool decodeFailed{false};
    std::string errorMessage{};
};

struct StagedEntityData {
    uint64_t stableId{0};
    std::string name{};
    uint64_t parentStableId{0};

    struct {
        bool present{false};
        Vec3 position{0.0F, 0.0F, 0.0F};
        Quat rotation{Quat::identity()};
        Vec3 scale{1.0F, 1.0F, 1.0F};
    } transform;

    struct {
        bool present{false};
        physics::MotionType motionType{physics::MotionType::Static};
        uint16_t collisionLayer{physics::CollisionLayer::kNonMoving};
    } rigidBody;

    struct {
        bool present{false};
        physics::ColliderDesc desc{};
    } collider;

    struct {
        bool present{false};
        std::string scriptName{};
        bool enabled{true};
        float updateInterval{0.0F};
    } script;

    struct {
        bool present{false};
        std::string assetPath{};
        uint32_t submeshIndex{0};
        bool visible{true};
        bool castShadows{true};
    } mesh;

    struct {
        bool present{false};
        render::DirectionalLightComponent light{};
    } directionalLight;

    struct {
        bool present{false};
        render::PointLightComponent light{};
    } pointLight;

    struct {
        bool present{false};
        render::SpotLightComponent light{};
    } spotLight;

    struct {
        bool present{false};
        render::CameraComponent camera{};
    } camera;

    struct {
        bool present{false};
        render::PostProcessSettings settings{};
    } postProcess;

    struct {
        bool present{false};
        render::IrradianceProbeComponent probe{};
    } irradianceProbe;
};

} // namespace

struct AsyncSceneLoader::Impl {
    JobSystem* jobSystem{nullptr};
    render::AssetManager* assetManager{nullptr};

    std::atomic<AsyncLoadState> state{AsyncLoadState::Idle};
    std::atomic<uint64_t> currentRequestId{0};
    std::atomic<bool> cancellationRequested{false};
    std::atomic<bool> discoveryFinished{false};
    std::atomic<bool> allDecodingFinished{false};

    mutable std::mutex progressMutex{};
    AsyncLoadProgress progress{};
    AsyncLoadTelemetry telemetry{};
    std::string errorMessage{};
    std::vector<AssetErrorInfo> assetErrors{};

    Timestamp loadStartTime{Timestamp::now()};
    Timestamp discoveryStartTime{Timestamp::now()};
    Timestamp decodeStartTime{Timestamp::now()};
    Timestamp uploadStartTime{Timestamp::now()};

    std::string targetFilePath{};
    std::string sceneName{"Untitled"};
    SceneMetadata metadata{};
    std::optional<Vec3> cameraPosition{std::nullopt};
    std::optional<Vec3> cameraTarget{std::nullopt};
    std::optional<render::PostProcessSettings> postProcessSettings{std::nullopt};

    std::vector<StagedEntityData> stagedEntities{};
    std::deque<StagedTextureItem> pendingTextureDecodes{};
    std::deque<StagedTextureItem> pendingTextureUploads{};

    std::mutex queueMutex{};

    void updateProgress(const std::function<void(AsyncLoadProgress&)>& fn) {
        std::lock_guard<std::mutex> lock(progressMutex);
        fn(progress);
        progress.state = state.load(std::memory_order_relaxed);
    }

    void setFailure(std::string_view msg) {
        std::lock_guard<std::mutex> lock(progressMutex);
        errorMessage = std::string(msg);
        state.store(AsyncLoadState::Failed, std::memory_order_release);
        progress.state = AsyncLoadState::Failed;
        progress.currentOperation = errorMessage;
        ENG_LOG_ERROR("AsyncSceneLoader", errorMessage);
    }

    void cancelLoading() {
        cancellationRequested.store(true, std::memory_order_release);
        state.store(AsyncLoadState::Cancelled, std::memory_order_release);
        std::lock_guard<std::mutex> lock(queueMutex);
        stagedEntities.clear();
        pendingTextureDecodes.clear();
        pendingTextureUploads.clear();

        std::lock_guard<std::mutex> progLock(progressMutex);
        progress.state = AsyncLoadState::Cancelled;
        progress.currentOperation = "Loading cancelled by user";
    }

    // -------------------------------------------------------------------------
    // Stage 1 & 2: Background File Reading & Asset Discovery
    // -------------------------------------------------------------------------
    void runDiscoveryJob(uint64_t reqId) {
        if (cancellationRequested.load(std::memory_order_acquire) ||
            reqId != currentRequestId.load(std::memory_order_acquire)) {
            return;
        }

        const Timestamp tStart = Timestamp::now();
        discoveryStartTime = tStart;

        updateProgress([&](AsyncLoadProgress& p) {
            p.currentOperation = "Reading scene file from disk...";
            p.discoveryProgress = 0.1F;
            p.overallProgress = 0.05F;
        });

        const std::string path = targetFilePath;
        if (!std::filesystem::exists(path)) {
            setFailure("Scene file does not exist on disk: " + path);
            return;
        }

        world::World stagingWorld;
        std::unordered_set<std::string> referencedGltfFiles;

        if (path.ends_with(".hikari")) {
            std::ifstream file(path, std::ios::in | std::ios::binary);
            if (!file.is_open()) {
                setFailure("Failed to open .hikari scene file for reading: " + path);
                return;
            }

            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();
            telemetry.fileIoDuration = Timestamp::now() - discoveryStartTime;

            updateProgress([&](AsyncLoadProgress& p) {
                p.currentOperation = "Parsing scene hierarchy and discovering assets...";
                p.discoveryProgress = 0.3F;
                p.overallProgress = 0.10F;
            });

            auto deserRes = SceneSerializer::deserializeFromString(stagingWorld, content, false);
            if (!deserRes.hasValue()) {
                setFailure("Scene deserialization failed: " +
                           std::string(deserRes.error().message));
                return;
            }

            metadata = deserRes.value().metadata;
            cameraPosition = deserRes.value().cameraPosition;
            cameraTarget = deserRes.value().cameraTarget;
            postProcessSettings = deserRes.value().postProcessSettings;

            // Collect referenced glTF base files from mesh components
            flecs::world& ecs = stagingWorld.getRawWorld();
            ecs.each([&](flecs::entity, const render::MeshComponent& mc) {
                if (!mc.assetPath.empty()) {
                    const auto hashPos = mc.assetPath.find('#');
                    if (hashPos != std::string::npos) {
                        referencedGltfFiles.insert(mc.assetPath.substr(0, hashPos));
                    }
                }
            });
        } else if (path.ends_with(".gltf") || path.ends_with(".glb")) {
            telemetry.fileIoDuration = Duration::fromMilliseconds(1);
            updateProgress([&](AsyncLoadProgress& p) {
                p.currentOperation = "Importing glTF scene hierarchy...";
                p.discoveryProgress = 0.3F;
                p.overallProgress = 0.10F;
            });

            auto importRes =
                render::GltfSceneImporter::importScene(path, stagingWorld, assetManager);
            if (!importRes.success) {
                setFailure("glTF scene import failed: " + importRes.errorMessage);
                return;
            }
            referencedGltfFiles.insert(path);
        } else {
            setFailure("Unsupported scene format: " + path);
            return;
        }

        // Import referenced glTF base files to register all meshes into assetManager
        for (const auto& gltfPath : referencedGltfFiles) {
            if (cancellationRequested.load(std::memory_order_acquire) ||
                reqId != currentRequestId.load(std::memory_order_acquire)) {
                return;
            }
            if (path.ends_with(".hikari")) {
                world::World dummyGltfWorld;
                (void)render::GltfSceneImporter::importScene(gltfPath, dummyGltfWorld,
                                                             assetManager);
            }
        }

        if (cancellationRequested.load(std::memory_order_acquire) ||
            reqId != currentRequestId.load(std::memory_order_acquire)) {
            return;
        }

        // Extract staged entity data from stagingWorld
        std::vector<StagedEntityData> entities;
        flecs::world& ecs = stagingWorld.getRawWorld();
        ecs.each([&](flecs::entity e) {
            StagedEntityData staged{};
            staged.stableId = e.has<StableIdComponent>() ? e.get<StableIdComponent>()->id
                                                         : static_cast<uint64_t>(e.id());
            staged.name = e.name().c_str();

            if (e.has<world::TransformComponent>()) {
                staged.transform.present = true;
                staged.transform.position = e.get<world::TransformComponent>()->transform.position;
                staged.transform.rotation = e.get<world::TransformComponent>()->transform.rotation;
                staged.transform.scale = e.get<world::TransformComponent>()->transform.scale;
            }

            if (e.has<physics::RigidBodyComponent>()) {
                staged.rigidBody.present = true;
                staged.rigidBody.motionType = e.get<physics::RigidBodyComponent>()->motionType;
                staged.rigidBody.collisionLayer =
                    e.get<physics::RigidBodyComponent>()->collisionLayer;
            }

            if (e.has<physics::ColliderComponent>()) {
                staged.collider.present = true;
                staged.collider.desc = e.get<physics::ColliderComponent>()->desc;
            }

            if (e.has<script::ScriptComponent>()) {
                staged.script.present = true;
                staged.script.scriptName = e.get<script::ScriptComponent>()->scriptName;
                staged.script.enabled = e.get<script::ScriptComponent>()->enabled;
                staged.script.updateInterval = e.get<script::ScriptComponent>()->updateInterval;
            }

            if (e.has<render::MeshComponent>()) {
                const auto* mc = e.get<render::MeshComponent>();
                staged.mesh.present = true;
                staged.mesh.assetPath = mc->assetPath;
                staged.mesh.submeshIndex = mc->submeshIndex;
                staged.mesh.visible = mc->visible;
                staged.mesh.castShadows = mc->castShadows;
            }

            if (e.has<render::DirectionalLightComponent>()) {
                staged.directionalLight.present = true;
                staged.directionalLight.light = *e.get<render::DirectionalLightComponent>();
            }

            if (e.has<render::PointLightComponent>()) {
                staged.pointLight.present = true;
                staged.pointLight.light = *e.get<render::PointLightComponent>();
            }

            if (e.has<render::SpotLightComponent>()) {
                staged.spotLight.present = true;
                staged.spotLight.light = *e.get<render::SpotLightComponent>();
            }

            if (e.has<render::CameraComponent>()) {
                staged.camera.present = true;
                staged.camera.camera = *e.get<render::CameraComponent>();
            }

            if (e.has<render::PostProcessSettings>()) {
                staged.postProcess.present = true;
                staged.postProcess.settings = *e.get<render::PostProcessSettings>();
            }

            if (e.has<render::IrradianceProbeComponent>()) {
                staged.irradianceProbe.present = true;
                staged.irradianceProbe.probe = *e.get<render::IrradianceProbeComponent>();
            }

            auto parent = e.parent();
            if (parent.is_valid() && parent.id() != 0) {
                staged.parentStableId = parent.has<StableIdComponent>()
                                            ? parent.get<StableIdComponent>()->id
                                            : static_cast<uint64_t>(parent.id());
            }

            entities.push_back(std::move(staged));
        });

        // Discover required textures from all registered meshes
        std::unordered_set<std::string> discoveredTextureKeys;
        std::vector<StagedTextureItem> texturesToDecode;

        auto addTexture = [&](const render::TextureInfo& info, bool srgb, bool isOpt) {
            if (!info.isPresent || info.uri.empty()) {
                return;
            }
            const std::string colorSuffix = srgb ? "#srgb" : "#linear";
            const std::string cacheKey =
                (info.uri.ends_with("#srgb") || info.uri.ends_with("#linear"))
                    ? info.uri
                    : (info.uri + colorSuffix);

            if (discoveredTextureKeys.insert(cacheKey).second) {
                if (assetManager != nullptr &&
                    assetManager->isLoaded(render::makeAssetIdFromPath(cacheKey))) {
                    return;
                }

                StagedTextureItem item{};
                item.cacheKey = cacheKey;
                item.filePath = info.uri;
                item.srgb = srgb;
                item.isOptional = isOpt;

                if (item.filePath.ends_with("#srgb")) {
                    item.filePath = item.filePath.substr(0, item.filePath.size() - 5);
                } else if (item.filePath.ends_with("#linear")) {
                    item.filePath = item.filePath.substr(0, item.filePath.size() - 7);
                }

                texturesToDecode.push_back(std::move(item));
            }
        };

        if (assetManager != nullptr) {
            for (const auto& metaRec : assetManager->getAllMetadata()) {
                if (metaRec.type == render::AssetType::Mesh) {
                    const auto* meshData = assetManager->getMeshData(metaRec.id);
                    if (meshData != nullptr) {
                        for (const auto& mat : meshData->materials) {
                            addTexture(mat.baseColorTexture, true, false);
                            addTexture(mat.metallicRoughnessTexture, false, true);
                            addTexture(mat.normalTexture, false, true);
                            addTexture(mat.occlusionTexture, false, true);
                            addTexture(mat.emissiveTexture, true, true);
                            addTexture(mat.clearcoat.texture, false, true);
                            addTexture(mat.clearcoat.roughnessTexture, false, true);
                            addTexture(mat.clearcoat.normalTexture, false, true);
                        }
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stagedEntities = std::move(entities);
            pendingTextureDecodes.clear();
            for (auto& t : texturesToDecode) {
                pendingTextureDecodes.push_back(std::move(t));
            }
        }

        telemetry.discoveryDuration = Timestamp::now() - tStart;

        updateProgress([&](AsyncLoadProgress& p) {
            p.entitiesDiscovered = static_cast<uint32_t>(stagedEntities.size());
            p.texturesTotal = static_cast<uint32_t>(pendingTextureDecodes.size());
            p.discoveryProgress = 1.0F;
            p.overallProgress = 0.20F;
            if (p.texturesTotal == 0) {
                p.textureProgress = 1.0F;
            }
        });

        discoveryFinished.store(true, std::memory_order_release);
        state.store(AsyncLoadState::DecodingAssets, std::memory_order_release);
        decodeStartTime = Timestamp::now();

        // Dispatch background decode jobs
        dispatchTextureDecodeJobs(reqId);
    }

    // -------------------------------------------------------------------------
    // Stage 3 & 4: Parallel Texture Decoding on Background Workers
    // -------------------------------------------------------------------------
    struct DecodeTaskContext {
        Impl* self{nullptr};
        uint64_t reqId{0};
    };

    void dispatchTextureDecodeJobs(uint64_t reqId) {
        if (pendingTextureDecodes.empty()) {
            allDecodingFinished.store(true, std::memory_order_release);
            updateProgress([&](AsyncLoadProgress& p) {
                p.textureProgress = 1.0F;
                p.overallProgress = 0.40F;
            });
            return;
        }

        updateProgress([&](AsyncLoadProgress& p) {
            p.currentOperation = "Decoding texture assets in background worker jobs...";
        });

        if (jobSystem != nullptr && jobSystem->isRunning() && jobSystem->workerCount() > 0) {
            auto* taskCtx = new DecodeTaskContext{this, reqId};
            (void)jobSystem->submit(
                [](void* ctx) noexcept -> bool {
                    auto* c = static_cast<DecodeTaskContext*>(ctx);
                    c->self->runTextureDecodeLoop(c->reqId);
                    delete c;
                    return true;
                },
                taskCtx);
        } else {
            runTextureDecodeLoop(reqId);
        }
    }

    void runTextureDecodeLoop(uint64_t reqId) {
        const Timestamp tStart = Timestamp::now();

        while (!cancellationRequested.load(std::memory_order_acquire) &&
               reqId == currentRequestId.load(std::memory_order_acquire)) {
            StagedTextureItem item{};
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (pendingTextureDecodes.empty()) {
                    break;
                }
                item = std::move(pendingTextureDecodes.front());
                pendingTextureDecodes.pop_front();
            }

            Result<render::ImageData> decodeRes =
                Result<render::ImageData>::failure(Error(ErrorCode::EntityNotFound, ""));
            if (std::filesystem::exists(item.filePath)) {
                decodeRes = render::ImageDecoder::decodeFromFile(item.filePath, 4, item.srgb);
            } else {
                decodeRes = Result<render::ImageData>::failure(Error(
                    ErrorCode::EntityNotFound, "Texture file not found on disk: " + item.filePath));
            }

            if (decodeRes.hasValue()) {
                item.decodedImage = std::move(decodeRes.value());
                item.decodeFailed = false;
                std::lock_guard<std::mutex> lock(queueMutex);
                pendingTextureUploads.push_back(std::move(item));
            } else {
                item.decodeFailed = true;
                item.errorMessage = decodeRes.error().message;
                std::lock_guard<std::mutex> lock(progressMutex);
                assetErrors.push_back(AssetErrorInfo{.path = item.filePath,
                                                     .assetType = "Texture",
                                                     .reason = item.errorMessage,
                                                     .isOptional = item.isOptional});
            }

            updateProgress([&](AsyncLoadProgress& p) {
                if (item.decodeFailed) {
                    p.texturesFailed++;
                } else {
                    p.texturesDecoded++;
                }
                if (p.texturesTotal > 0) {
                    p.textureProgress = static_cast<float>(p.texturesDecoded + p.texturesFailed) /
                                        static_cast<float>(p.texturesTotal);
                    p.overallProgress = 0.20F + (p.textureProgress * 0.30F);
                }
            });
        }

        telemetry.textureDecodeDuration = Timestamp::now() - tStart;
        allDecodingFinished.store(true, std::memory_order_release);
    }

    // -------------------------------------------------------------------------
    // Stage 5: Incremental GPU Resource Upload (Main Thread Budgeted Tick)
    // -------------------------------------------------------------------------
    void tickGpuUpload(rhi::RhiDevice& device, float budgetMs) {
        auto curState = state.load(std::memory_order_acquire);
        if (curState != AsyncLoadState::UploadingGpu &&
            curState != AsyncLoadState::DecodingAssets) {
            return;
        }

        state.store(AsyncLoadState::UploadingGpu, std::memory_order_release);

        const auto startTick = std::chrono::high_resolution_clock::now();
        const double budgetUs = static_cast<double>(budgetMs) * 1000.0;

        if (assetManager != nullptr) {
            assetManager->beginBatchUpload();
        }

        while (!cancellationRequested.load(std::memory_order_acquire)) {
            StagedTextureItem texItem{};
            bool hasTex = false;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (!pendingTextureUploads.empty()) {
                    texItem = std::move(pendingTextureUploads.front());
                    pendingTextureUploads.pop_front();
                    hasTex = true;
                }
            }

            if (hasTex && texItem.decodedImage.has_value()) {
                if (assetManager != nullptr) {
                    auto regRes = assetManager->registerTexture(texItem.cacheKey,
                                                                std::move(*texItem.decodedImage));
                    if (regRes.hasValue()) {
                        (void)assetManager->uploadTexture(regRes.value(), device, true, nullptr);
                    }
                }
                updateProgress([&](AsyncLoadProgress& p) {
                    p.texturesUploaded++;
                    if (p.texturesTotal > 0) {
                        p.gpuUploadProgress = static_cast<float>(p.texturesUploaded) /
                                              static_cast<float>(p.texturesTotal);
                        p.overallProgress = 0.50F + (p.gpuUploadProgress * 0.40F);
                    }
                });
            }

            if (!hasTex) {
                if (allDecodingFinished.load(std::memory_order_acquire)) {
                    state.store(AsyncLoadState::Validating, std::memory_order_release);
                    validateAndPrepareActivation(device);
                }
                break;
            }

            const auto now = std::chrono::high_resolution_clock::now();
            const double elapsedUs =
                std::chrono::duration<double, std::micro>(now - startTick).count();
            if (elapsedUs >= budgetUs) {
                break; // Enforce frame time budget to maintain 60+ FPS UI responsiveness
            }
        }
    }

    // -------------------------------------------------------------------------
    // Stage 6: Validation & Readiness
    // -------------------------------------------------------------------------
    void validateAndPrepareActivation(rhi::RhiDevice& device) {
        if (assetManager != nullptr) {
            assetManager->endBatchUpload(device);
        }

        updateProgress([&](AsyncLoadProgress& p) {
            p.currentOperation = "Validating staged scene data...";
            p.overallProgress = 0.95F;
        });

        state.store(AsyncLoadState::ReadyToActivate, std::memory_order_release);

        updateProgress([&](AsyncLoadProgress& p) {
            p.currentOperation = "Scene ready for safe activation";
            p.overallProgress = 1.0F;
        });
    }

    // -------------------------------------------------------------------------
    // Stage 7: Atomic Main-Thread Scene Activation
    // -------------------------------------------------------------------------
    Result<SceneActivationResult, Error> activateScene(world::World& world) {
        if (state.load(std::memory_order_acquire) != AsyncLoadState::ReadyToActivate) {
            return Result<SceneActivationResult, Error>::failure(
                Error(ErrorCode::InvalidState, "Scene is not ready for activation (state: " +
                                                   std::string(toString(state.load())) + ")"));
        }

        state.store(AsyncLoadState::Activating, std::memory_order_release);
        const Timestamp tStart = Timestamp::now();

        // 1. Clear old user entities from authoritative World
        world.clearUserEntities();

        // 2. Instantiate staged entities into World
        std::unordered_map<uint64_t, world::EntityHandle> stableIdToHandle;
        stableIdToHandle.reserve(stagedEntities.size());

        for (const auto& staged : stagedEntities) {
            world::EntityHandle h =
                staged.name.empty() ? world.createEntity() : world.createEntity(staged.name);
            world.setComponent(h, StableIdComponent{staged.stableId});
            stableIdToHandle[staged.stableId] = h;

            if (staged.transform.present) {
                Transform tr(staged.transform.position, staged.transform.rotation,
                             staged.transform.scale);
                world.setComponent(h, world::TransformComponent{tr});
                world.setComponent(h, physics::PhysicsTransformComponent{.transform = tr});
            }

            if (staged.rigidBody.present) {
                world.setComponent(h, physics::RigidBodyComponent{
                                          .bodyHandle = {},
                                          .motionType = staged.rigidBody.motionType,
                                          .collisionLayer = staged.rigidBody.collisionLayer,
                                      });
            }

            if (staged.collider.present) {
                world.setComponent(h, physics::ColliderComponent{
                                          .desc = staged.collider.desc,
                                      });
            }

            if (staged.script.present) {
                script::ScriptComponent sc{};
                const size_t len =
                    std::min(staged.script.scriptName.size(), sizeof(sc.scriptName) - 1);
                for (size_t k = 0; k < len; ++k) {
                    sc.scriptName[k] = staged.script.scriptName[k];
                }
                sc.scriptName[len] = '\0';
                sc.enabled = staged.script.enabled;
                sc.updateInterval = staged.script.updateInterval;
                sc.timeSinceLastUpdate = 0.0F;
                world.setComponent(h, sc);
            }

            if (staged.mesh.present) {
                render::MeshComponent mc{};
                mc.assetPath = staged.mesh.assetPath;
                mc.meshAsset = render::makeAssetIdFromPath(mc.assetPath);
                mc.submeshIndex = staged.mesh.submeshIndex;
                mc.visible = staged.mesh.visible;
                mc.castShadows = staged.mesh.castShadows;
                world.setComponent(h, mc);
            }

            if (staged.directionalLight.present) {
                world.setComponent(h, staged.directionalLight.light);
            }

            if (staged.pointLight.present) {
                world.setComponent(h, staged.pointLight.light);
            }

            if (staged.spotLight.present) {
                world.setComponent(h, staged.spotLight.light);
            }

            if (staged.camera.present) {
                world.setComponent(h, staged.camera.camera);
            }

            if (staged.postProcess.present) {
                world.setComponent(h, staged.postProcess.settings);
            }

            if (staged.irradianceProbe.present) {
                world.setComponent(h, staged.irradianceProbe.probe);
            }
        }

        // 3. Establish parent-child hierarchy
        for (const auto& staged : stagedEntities) {
            if (staged.parentStableId != 0) {
                auto it = stableIdToHandle.find(staged.parentStableId);
                if (it != stableIdToHandle.end()) {
                    world::EntityHandle child = stableIdToHandle[staged.stableId];
                    world.setParent(child, it->second, false);
                }
            }
        }

        // 4. Propagate hierarchy matrices
        world::updateHierarchyTransforms(world);

        telemetry.ecsCreationDuration = Timestamp::now() - tStart;
        telemetry.totalDuration = Timestamp::now() - loadStartTime;

        const uint32_t count = static_cast<uint32_t>(stagedEntities.size());
        stagedEntities.clear();

        state.store(AsyncLoadState::Completed, std::memory_order_release);
        updateProgress([&](AsyncLoadProgress& p) {
            p.activationProgress = 1.0F;
            p.overallProgress = 1.0F;
            p.currentOperation = "Scene activated successfully";
        });

        ENG_LOG_INFO("AsyncSceneLoader",
                     "Scene activated successfully with " + std::to_string(count) + " entities");

        return Result<SceneActivationResult, Error>::success(SceneActivationResult{
            .entitiesLoaded = count,
            .cameraPosition = cameraPosition,
            .cameraTarget = cameraTarget,
            .postProcessSettings = postProcessSettings,
        });
    }
};

AsyncSceneLoader::AsyncSceneLoader() : impl_(std::make_unique<Impl>()) {}
AsyncSceneLoader::~AsyncSceneLoader() {
    if (impl_) {
        impl_->cancelLoading();
    }
}

AsyncSceneLoader::AsyncSceneLoader(AsyncSceneLoader&&) noexcept = default;
AsyncSceneLoader& AsyncSceneLoader::operator=(AsyncSceneLoader&&) noexcept = default;

void AsyncSceneLoader::bindJobSystem(JobSystem* jobSystem) noexcept {
    impl_->jobSystem = jobSystem;
}

void AsyncSceneLoader::bindAssetManager(render::AssetManager* assetManager) noexcept {
    impl_->assetManager = assetManager;
}

Result<uint64_t, Error> AsyncSceneLoader::startLoad(std::string_view filepath) {
    if (filepath.empty()) {
        return Result<uint64_t, Error>::failure(
            Error(ErrorCode::InvalidArgument, "Empty scene file path"));
    }

    if (!std::filesystem::exists(filepath)) {
        return Result<uint64_t, Error>::failure(
            Error(ErrorCode::InvalidArgument,
                  "Scene file does not exist on disk: " + std::string(filepath)));
    }

    impl_->cancelLoading();

    const uint64_t reqId = ++impl_->currentRequestId;
    impl_->cancellationRequested.store(false, std::memory_order_release);
    impl_->discoveryFinished.store(false, std::memory_order_release);
    impl_->allDecodingFinished.store(false, std::memory_order_release);
    impl_->targetFilePath = std::string(filepath);
    impl_->sceneName = std::filesystem::path(filepath).stem().string();
    impl_->loadStartTime = Timestamp::now();

    {
        std::lock_guard<std::mutex> lock(impl_->progressMutex);
        impl_->progress = AsyncLoadProgress{};
        impl_->progress.sceneName = impl_->sceneName;
        impl_->progress.currentOperation = "Initializing scene load request...";
        impl_->telemetry = AsyncLoadTelemetry{};
        impl_->errorMessage.clear();
        impl_->assetErrors.clear();
    }

    impl_->state.store(AsyncLoadState::ReadingFile, std::memory_order_release);

    if (impl_->jobSystem != nullptr && impl_->jobSystem->isRunning() &&
        impl_->jobSystem->workerCount() > 0) {
        struct DiscoveryContext {
            Impl* self{nullptr};
            uint64_t reqId{0};
        };
        auto* ctx = new DiscoveryContext{impl_.get(), reqId};
        auto submitRes = impl_->jobSystem->submit(
            [](void* c) noexcept -> bool {
                auto* dCtx = static_cast<DiscoveryContext*>(c);
                dCtx->self->runDiscoveryJob(dCtx->reqId);
                delete dCtx;
                return true;
            },
            ctx);
        if (!submitRes.hasValue()) {
            delete ctx;
            impl_->runDiscoveryJob(reqId);
        }
    } else {
        impl_->runDiscoveryJob(reqId);
    }

    return Result<uint64_t, Error>::success(reqId);
}

void AsyncSceneLoader::cancel() noexcept {
    impl_->cancelLoading();
}

void AsyncSceneLoader::update(rhi::RhiDevice& device, float budgetMs) {
    impl_->tickGpuUpload(device, budgetMs);
}

Result<SceneActivationResult, Error> AsyncSceneLoader::activate(world::World& world) {
    return impl_->activateScene(world);
}

AsyncLoadState AsyncSceneLoader::getState() const noexcept {
    return impl_->state.load(std::memory_order_acquire);
}

bool AsyncSceneLoader::isLoading() const noexcept {
    const auto s = getState();
    return s != AsyncLoadState::Idle && s != AsyncLoadState::Completed &&
           s != AsyncLoadState::Failed && s != AsyncLoadState::Cancelled;
}

bool AsyncSceneLoader::isReadyToActivate() const noexcept {
    return getState() == AsyncLoadState::ReadyToActivate;
}

bool AsyncSceneLoader::isCompleted() const noexcept {
    return getState() == AsyncLoadState::Completed;
}

bool AsyncSceneLoader::isFailed() const noexcept {
    return getState() == AsyncLoadState::Failed;
}

bool AsyncSceneLoader::isCancelled() const noexcept {
    return getState() == AsyncLoadState::Cancelled;
}

AsyncLoadProgress AsyncSceneLoader::getProgress() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->progressMutex);
    return impl_->progress;
}

const AsyncLoadTelemetry& AsyncSceneLoader::getTelemetry() const noexcept {
    return impl_->telemetry;
}

std::string_view AsyncSceneLoader::getErrorMessage() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->progressMutex);
    return impl_->errorMessage;
}

const std::vector<AssetErrorInfo>& AsyncSceneLoader::getAssetErrors() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->progressMutex);
    return impl_->assetErrors;
}

void AsyncSceneLoader::reset() noexcept {
    impl_->cancelLoading();
    impl_->state.store(AsyncLoadState::Idle, std::memory_order_release);
    std::lock_guard<std::mutex> lock(impl_->progressMutex);
    impl_->progress = AsyncLoadProgress{};
}

} // namespace eng::framework
