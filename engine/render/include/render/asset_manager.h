/// @file asset_manager.h
/// @brief Central asset repository and cache for mesh, material, and GPU resources.
///
/// Purpose:
/// Loads, deduplicates, caches, and provides handle-based access to engine assets
/// (meshes, materials, GPU buffers). Shared across edit and runtime simulation worlds.
///
/// Ownership and Lifetime:
/// Owned by EditorEngine or application root. Outlives active and standby simulation worlds.
///
/// Thread-safety:
/// Read methods are const; mutation (load/unload/upload) must be serialized on the main thread.
///
/// Performance:
/// Cached lookups via 64-bit AssetId are O(1) hash map operations with zero allocation.

#pragma once

#include "core/error.h"
#include "core/result.h"
#include "render/asset_types.h"
#include "render/gpu_material.h"
#include "render/gpu_mesh.h"
#include "render/gpu_texture.h"
#include "render/graphics_settings.h"
#include "render/image.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/sampler_cache.h"
#include "render/texture_residency.h"
#include "rhi/rhi_device.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace eng::render {

/// @enum DefaultTextureType
/// @brief Identifiers for singleton fallback GPU textures.
enum class DefaultTextureType {
    WhiteSRGB,
    WhiteLinear,
    BlackSRGB,
    FlatNormalLinear,
};

/// @class AssetManager
/// @brief Manages loading, caching, and lifecycle of engine assets.
class AssetManager {
public:
    AssetManager() = default;
    ~AssetManager() noexcept;

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;
    AssetManager(AssetManager&&) noexcept = default;
    AssetManager& operator=(AssetManager&&) noexcept = default;

    /// @brief Loads a 3D mesh asset from a relative or absolute file path.
    /// Deduplicates if the asset is already loaded or registered.
    ///
    /// @param path Relative path (e.g. "assets/models/cube.obj") or absolute path.
    /// @return The persistent 64-bit AssetId for this mesh.
    Result<AssetId> loadMesh(std::string_view path);

    /// @brief Registers an in-memory MeshData with a designated virtual path or name.
    ///
    /// @param path Identifier or path for the mesh.
    /// @param mesh Data describing vertices, indices, and submeshes.
    /// @return The persistent AssetId assigned to this mesh.
    Result<AssetId> registerMesh(std::string_view path, MeshData mesh);

    /// @brief Uploads a loaded CPU mesh to the GPU if not already uploaded.
    ///
    /// @param id Asset identifier of the mesh.
    /// @param device Active RhiDevice for buffer allocation.
    /// @return Pointer to the GpuMesh on success, or Error on failure.
    Result<GpuMesh*> uploadMesh(AssetId id, rhi::RhiDevice& device);

    /// @brief Retrieves the GPU mesh for an asset, or nullptr if not uploaded.
    [[nodiscard]] GpuMesh* getGpuMesh(AssetId id) noexcept;
    [[nodiscard]] const GpuMesh* getGpuMesh(AssetId id) const noexcept;

    /// @brief Loads an image texture asset from a relative or absolute file path.
    /// Deduplicates if the asset is already loaded or registered.
    ///
    /// @param path Relative path or absolute path to the image file.
    /// @param srgb Whether the texture format should be tagged as sRGB.
    /// @return The persistent 64-bit AssetId for this texture.
    Result<AssetId> loadTexture(std::string_view path, bool srgb = true);

    /// @brief Registers an in-memory ImageData with a designated virtual path or name.
    ///
    /// @param path Identifier or virtual path for the texture.
    /// @param image Decoded host bitmap pixel data.
    /// @return The persistent AssetId assigned to this texture.
    Result<AssetId> registerTexture(std::string_view path, ImageData image);

    /// @brief Uploads a loaded CPU image to the GPU if not already uploaded.
    ///
    /// @param id Asset identifier of the texture.
    /// @param device Active RhiDevice for texture and sampler allocation.
    /// @param generateMips Whether to generate complete mipmap chain.
    /// @param samplerDesc Optional custom sampler configuration.
    /// @return Pointer to the GpuTexture on success, or Error on failure.
    Result<GpuTexture*> uploadTexture(AssetId id, rhi::RhiDevice& device, bool generateMips = true,
                                      const rhi::SamplerDesc* samplerDesc = nullptr);

    /// @brief Retrieves the GPU texture for an asset, or nullptr if not uploaded.
    [[nodiscard]] GpuTexture* getGpuTexture(AssetId id) noexcept;
    [[nodiscard]] const GpuTexture* getGpuTexture(AssetId id) const noexcept;

    /// @brief Retrieves or creates a default fallback GPU texture.
    ///
    /// Fallback textures are 1x1 single-color textures used when a material does
    /// not supply an optional texture map:
    /// - WhiteSRGB: (1, 1, 1, 1) sRGB for missing BaseColor
    /// - WhiteLinear: (1, 1, 1, 1) Linear for missing MetallicRoughness or Occlusion
    /// - BlackSRGB: (0, 0, 0, 1) sRGB for missing Emissive
    /// - FlatNormalLinear: (0.5, 0.5, 1.0, 1.0) Linear for missing Normal
    ///
    /// @param device The active RHI device for resource allocation.
    /// @param type The default texture type to retrieve or instantiate.
    /// @return Pointer to valid GpuTexture or error.
    Result<GpuTexture*> getOrCreateDefaultTexture(rhi::RhiDevice& device, DefaultTextureType type);

    /// @brief Uploads or retrieves the GpuMaterial for a specific submesh/material index.
    Result<GpuMaterial*> uploadMaterial(AssetId meshId, size_t materialIndex,
                                        rhi::RhiDevice& device,
                                        rhi::DescriptorSetLayoutHandle materialLayout);

    /// @brief Retrieves the GpuMaterial for a mesh material index, or nullptr if not uploaded.
    [[nodiscard]] GpuMaterial* getGpuMaterial(AssetId meshId, size_t materialIndex) noexcept;
    [[nodiscard]] const GpuMaterial* getGpuMaterial(AssetId meshId,
                                                    size_t materialIndex) const noexcept;

    /// @brief Retrieves or creates a default fallback GpuMaterial.
    Result<GpuMaterial*> getOrCreateDefaultMaterial(rhi::RhiDevice& device,
                                                    rhi::DescriptorSetLayoutHandle materialLayout);

    /// @brief Retrieves CPU image data for an asset, or nullptr if not loaded.
    [[nodiscard]] const ImageData* getImageData(AssetId id) const noexcept;
    [[nodiscard]] const ImageData* getImageData(std::string_view path) const noexcept;

    /// @brief Retrieves CPU mesh data for an asset, or nullptr if not loaded.
    [[nodiscard]] const MeshData* getMeshData(AssetId id) const noexcept;
    [[nodiscard]] const MeshData* getMeshData(std::string_view path) const noexcept;

    /// @brief Retrieves materials associated with a loaded mesh, or nullptr if not loaded.
    [[nodiscard]] const std::vector<Material>* getMaterials(AssetId id) const noexcept;

    /// @brief Retrieves metadata for an asset by ID, or nullptr if not registered.
    [[nodiscard]] const AssetMetadata* getMetadata(AssetId id) const noexcept;

    /// @brief Finds the AssetId for a given path, or an invalid handle if not registered.
    [[nodiscard]] AssetId findAssetByPath(std::string_view path) const noexcept;

    /// @brief Checks whether the specified asset is loaded in CPU memory.
    [[nodiscard]] bool isLoaded(AssetId id) const noexcept;

    /// @brief Checks whether the specified asset previously failed to load.
    [[nodiscard]] bool isFailed(AssetId id) const noexcept;

    /// @brief Unloads CPU and GPU data for a specific asset.
    void unload(AssetId id) noexcept;

    /// @brief Unloads all CPU and GPU resources.
    void unloadAll() noexcept;

    /// @brief Returns a copy of all registered asset metadata records.
    [[nodiscard]] std::vector<AssetMetadata> getAllMetadata() const;

    /// @brief Configures quality settings (anisotropic filtering, texture quality) governing
    /// material samplers.
    void setQualitySettings(const QualitySettings& settings) noexcept {
        qualitySettings_ = settings;
    }

    /// @brief Retrieves the active quality settings governing material samplers.
    [[nodiscard]] const QualitySettings& getQualitySettings() const noexcept {
        return qualitySettings_;
    }

    /// @brief Accesses the internal SamplerCache deduplicating RHI samplers.
    [[nodiscard]] SamplerCache& getSamplerCache() noexcept { return samplerCache_; }
    [[nodiscard]] const SamplerCache& getSamplerCache() const noexcept { return samplerCache_; }

    /// @brief Reconfigures material samplers and updates descriptor sets when anisotropic
    /// filtering or texture quality settings change.
    Result<void> updateSamplerQuality(const QualitySettings& settings, rhi::RhiDevice& device);

    /// @brief Sets the texture memory budget configuration.
    void setTextureBudgetConfig(const TextureBudgetConfig& config) noexcept;

    /// @brief Gets the active texture memory budget configuration.
    [[nodiscard]] const TextureBudgetConfig& getTextureBudgetConfig() const noexcept {
        return budgetConfig_;
    }

    /// @brief Retrieves the real-time texture residency and memory telemetry metrics.
    [[nodiscard]] const TextureMemoryMetrics& getTextureMemoryMetrics() const noexcept {
        return metrics_;
    }

    /// @brief Returns the residency state of a specific texture asset.
    [[nodiscard]] TextureResidencyState getTextureResidencyState(AssetId id) const noexcept;

    /// @brief Updates the last-used frame index for a texture asset (for LRU eviction).
    void touchTexture(AssetId id, uint64_t frameIndex) noexcept;

    /// @brief Marks a texture as pinned (immune from eviction) or unpinned.
    void pinTexture(AssetId id, bool pinned) noexcept;

    /// @brief Checks if a texture asset is pinned.
    [[nodiscard]] bool isTexturePinned(AssetId id) const noexcept;

    /// @brief Safely evicts a GPU texture to reclaim memory, rebinding referencing materials to
    /// fallbacks.
    Result<bool> evictTexture(AssetId id, rhi::RhiDevice& device) noexcept;

    /// @brief Enforces texture memory budget using LRU eviction policy.
    Result<uint32_t> enforceTextureBudget(rhi::RhiDevice& device,
                                          uint64_t currentFrame = 0) noexcept;

    /// @brief Promotes texture mip level residency towards highest detail (LOD 0).
    Result<void> promoteTextureMips(AssetId id, rhi::RhiDevice& device) noexcept;

    /// @brief Demotes texture mip level residency towards coarse levels to conserve
    /// bandwidth/memory.
    Result<void> demoteTextureMips(AssetId id, rhi::RhiDevice& device) noexcept;

    /// @brief Rebinds all active material descriptor sets to current GPU textures or fallbacks.
    void rebindMaterialTextures(rhi::RhiDevice& device) noexcept;

private:
    void updateMemoryMetrics() noexcept;

    struct MeshEntry {
        AssetMetadata metadata{};
        MeshData cpuMesh{};
        std::unique_ptr<GpuMesh> gpuMesh{nullptr};
        std::vector<std::unique_ptr<GpuMaterial>> gpuMaterials{};
    };

    struct TextureEntry {
        AssetMetadata metadata{};
        ImageData cpuImage{};
        std::unique_ptr<GpuTexture> gpuTexture{nullptr};
        TextureResidencyState residencyState{TextureResidencyState::Unloaded};
        uint64_t lastUsedFrame{0};
        bool isPinned{false};
        size_t estimatedGpuBytes{0};
        uint32_t residentMipLevels{0};
    };

    std::unordered_map<AssetId, MeshEntry> meshEntries_{};
    std::unordered_map<AssetId, TextureEntry> textureEntries_{};
    std::unordered_map<std::string, AssetId> pathToId_{};
    std::unordered_set<AssetId> failedIds_{};
    std::unordered_map<std::string, std::string> failedPaths_{};
    std::unordered_map<DefaultTextureType, std::unique_ptr<GpuTexture>> defaultTextures_{};
    std::unique_ptr<GpuMaterial> defaultMaterial_{nullptr};
    SamplerCache samplerCache_{};
    QualitySettings qualitySettings_{};
    TextureBudgetConfig budgetConfig_{};
    TextureMemoryMetrics metrics_{};
    rhi::RhiDevice* device_{nullptr};
};

} // namespace eng::render
