/// @file asset_manager.cpp
/// @brief Asset repository and cache implementation.

#include "render/asset_manager.h"

#include "core/log.h"
#include "gltf_internal.h"
#include "render/gltf_importer.h"
#include "render/gltf_scene_importer.h"
#include "render/image.h"
#include "render/obj_importer.h"
#include "world/world.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace eng::render {

namespace {

std::string normalizePath(std::string_view path) {
    std::string result(path);
    std::replace(result.begin(), result.end(), '\\', '/');
    if (result.rfind("./", 0) == 0) {
        result = result.substr(2);
    }
    return result;
}

} // namespace

AssetManager::~AssetManager() noexcept {
    unloadAll();
}

Result<AssetId> AssetManager::loadMesh(std::string_view path) {
    const std::string normPath = normalizePath(path);

    auto failIt = failedPaths_.find(normPath);
    if (failIt != failedPaths_.end()) {
        return Result<AssetId>::failure(Error(ErrorCode::EntityNotFound, failIt->second));
    }

    auto pathIt = pathToId_.find(normPath);
    if (pathIt != pathToId_.end()) {
        auto entryIt = meshEntries_.find(pathIt->second);
        if (entryIt != meshEntries_.end() &&
            entryIt->second.metadata.loadState == AssetLoadState::Loaded) {
            return Result<AssetId>::success(pathIt->second);
        }
    }

    const auto hashPos = normPath.find('#');
    if (hashPos != std::string::npos) {
        std::string baseFile = normPath.substr(0, hashPos);
        world::World dummyWorld;
        auto importRes = GltfSceneImporter::importScene(baseFile, dummyWorld, this);
        if (importRes.success) {
            auto it = pathToId_.find(normPath);
            if (it != pathToId_.end()) {
                return Result<AssetId>::success(it->second);
            }
        }
        return Result<AssetId>::failure(
            Error(ErrorCode::InvalidArgument, "Mesh URI fragment not found in scene: " + normPath));
    }

    std::string ext{};
    const auto dotPos = normPath.rfind('.');
    if (dotPos != std::string::npos) {
        ext = normPath.substr(dotPos);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    Result<MeshData> importRes = Result<MeshData>::failure(
        Error(ErrorCode::InvalidArgument, "Unsupported 3D asset file extension"));

    if (ext == ".gltf" || ext == ".glb") {
        importRes = GltfImporter::importFromFile(normPath);
    } else if (ext == ".obj") {
        importRes = ObjImporter::importFromFile(normPath);
    } else {
        // Fallback: try glTF then OBJ
        importRes = GltfImporter::importFromFile(normPath);
        if (!importRes.hasValue()) {
            importRes = ObjImporter::importFromFile(normPath);
        }
    }

    if (!importRes.hasValue()) {
        const std::string errMsg = "Failed to load mesh from path: " + normPath;
        ENG_LOG_WARN("AssetManager", errMsg);
        const AssetId id = makeAssetIdFromPath(normPath);
        failedIds_.insert(id);
        failedPaths_[normPath] = errMsg;
        return importRes.error();
    }

    const AssetId id = makeAssetIdFromPath(normPath);

    MeshEntry entry{};
    entry.metadata.id = id;
    entry.metadata.path = normPath;
    entry.metadata.type = AssetType::Mesh;
    entry.metadata.loadState = AssetLoadState::Loaded;
    entry.cpuMesh = std::move(importRes.value());
    entry.gpuMesh = nullptr;

    pathToId_[normPath] = id;
    meshEntries_[id] = std::move(entry);

    ENG_LOG_INFO("AssetManager", "Loaded mesh asset successfully: " + normPath);

    return Result<AssetId>::success(id);
}

Result<AssetId> AssetManager::registerMesh(std::string_view path, MeshData mesh) {
    auto valRes = mesh.validate();
    if (!valRes.hasValue()) {
        return valRes.error();
    }

    const std::string normPath = normalizePath(path);
    const AssetId id = makeAssetIdFromPath(normPath);

    MeshEntry entry{};
    entry.metadata.id = id;
    entry.metadata.path = normPath;
    entry.metadata.type = AssetType::Mesh;
    entry.metadata.loadState = AssetLoadState::Loaded;
    entry.cpuMesh = std::move(mesh);
    entry.gpuMesh = nullptr;

    pathToId_[normPath] = id;
    meshEntries_[id] = std::move(entry);

    return Result<AssetId>::success(id);
}

Result<GpuMesh*> AssetManager::uploadMesh(AssetId id, rhi::RhiDevice& device) {
    auto it = meshEntries_.find(id);
    if (it == meshEntries_.end() || it->second.metadata.loadState != AssetLoadState::Loaded) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Mesh asset not loaded in AssetManager"};
    }

    if (it->second.gpuMesh != nullptr) {
        return Result<GpuMesh*>::success(it->second.gpuMesh.get());
    }

    auto gpuRes = GpuMesh::create(device, it->second.cpuMesh);
    if (!gpuRes.hasValue()) {
        ENG_LOG_ERROR("AssetManager",
                      "Failed to create GPU mesh for asset: " + it->second.metadata.path);
        return gpuRes.error();
    }

    it->second.gpuMesh = std::move(gpuRes.value());
    return Result<GpuMesh*>::success(it->second.gpuMesh.get());
}

GpuMesh* AssetManager::getGpuMesh(AssetId id) noexcept {
    auto it = meshEntries_.find(id);
    if (it != meshEntries_.end()) {
        return it->second.gpuMesh.get();
    }
    return nullptr;
}

const GpuMesh* AssetManager::getGpuMesh(AssetId id) const noexcept {
    auto it = meshEntries_.find(id);
    if (it != meshEntries_.end()) {
        return it->second.gpuMesh.get();
    }
    return nullptr;
}

Result<GpuMaterial*> AssetManager::uploadMaterial(AssetId meshId, size_t materialIndex,
                                                  rhi::RhiDevice& device,
                                                  rhi::DescriptorSetLayoutHandle materialLayout) {
    auto it = meshEntries_.find(meshId);
    if (it == meshEntries_.end() || it->second.metadata.loadState != AssetLoadState::Loaded) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Mesh asset not loaded in AssetManager"};
    }

    auto& entry = it->second;
    if (entry.gpuMaterials.empty()) {
        entry.gpuMaterials.resize(entry.cpuMesh.materials.size());
    }

    if (materialIndex >= entry.cpuMesh.materials.size()) {
        return getOrCreateDefaultMaterial(device, materialLayout);
    }

    if (entry.gpuMaterials[materialIndex] != nullptr &&
        entry.gpuMaterials[materialIndex]->isValid()) {
        return Result<GpuMaterial*>::success(entry.gpuMaterials[materialIndex].get());
    }

    auto createRes =
        GpuMaterial::create(device, materialLayout, entry.cpuMesh.materials[materialIndex], *this);
    if (!createRes.hasValue()) {
        return createRes.error();
    }

    entry.gpuMaterials[materialIndex] = std::move(createRes.value());
    return Result<GpuMaterial*>::success(entry.gpuMaterials[materialIndex].get());
}

GpuMaterial* AssetManager::getGpuMaterial(AssetId meshId, size_t materialIndex) noexcept {
    auto it = meshEntries_.find(meshId);
    if (it != meshEntries_.end()) {
        if (materialIndex < it->second.gpuMaterials.size() &&
            it->second.gpuMaterials[materialIndex] != nullptr) {
            return it->second.gpuMaterials[materialIndex].get();
        }
    }
    return nullptr;
}

const GpuMaterial* AssetManager::getGpuMaterial(AssetId meshId,
                                                size_t materialIndex) const noexcept {
    auto it = meshEntries_.find(meshId);
    if (it != meshEntries_.end()) {
        if (materialIndex < it->second.gpuMaterials.size() &&
            it->second.gpuMaterials[materialIndex] != nullptr) {
            return it->second.gpuMaterials[materialIndex].get();
        }
    }
    return nullptr;
}

Result<GpuMaterial*>
AssetManager::getOrCreateDefaultMaterial(rhi::RhiDevice& device,
                                         rhi::DescriptorSetLayoutHandle materialLayout) {
    if (defaultMaterial_ != nullptr && defaultMaterial_->isValid()) {
        return Result<GpuMaterial*>::success(defaultMaterial_.get());
    }

    Material defaultMat = Material::createDefault();
    auto createRes = GpuMaterial::create(device, materialLayout, defaultMat, *this);
    if (!createRes.hasValue()) {
        return createRes.error();
    }

    defaultMaterial_ = std::move(createRes.value());
    return Result<GpuMaterial*>::success(defaultMaterial_.get());
}

const MeshData* AssetManager::getMeshData(AssetId id) const noexcept {
    auto it = meshEntries_.find(id);
    if (it != meshEntries_.end() && it->second.metadata.loadState == AssetLoadState::Loaded) {
        return &it->second.cpuMesh;
    }
    return nullptr;
}

const MeshData* AssetManager::getMeshData(std::string_view path) const noexcept {
    const std::string normPath = normalizePath(path);
    auto it = pathToId_.find(normPath);
    if (it != pathToId_.end()) {
        return getMeshData(it->second);
    }
    return nullptr;
}

Result<AssetId> AssetManager::loadTexture(std::string_view path, bool srgb) {
    const std::string normPath = normalizePath(path);
    const std::string colorSuffix = (srgb ? "#srgb" : "#linear");
    const std::string cacheKey = (normPath.ends_with("#srgb") || normPath.ends_with("#linear"))
                                     ? normPath
                                     : (normPath + colorSuffix);

    auto failIt = failedPaths_.find(cacheKey);
    if (failIt != failedPaths_.end()) {
        return Result<AssetId>::failure(Error(ErrorCode::EntityNotFound, failIt->second));
    }

    auto pathIt = pathToId_.find(cacheKey);
    if (pathIt != pathToId_.end()) {
        auto entryIt = textureEntries_.find(pathIt->second);
        if (entryIt != textureEntries_.end() &&
            entryIt->second.metadata.loadState == AssetLoadState::Loaded) {
            return Result<AssetId>::success(pathIt->second);
        }
    }

    Result<ImageData> decodeRes = Result<ImageData>::failure(Error(ErrorCode::EntityNotFound, ""));
    const auto hashImg = normPath.find("#image_");
    if (hashImg != std::string::npos) {
        std::string baseFile = normPath.substr(0, hashImg);
        std::string sub = normPath.substr(hashImg + 7);
        size_t endNum = sub.find('#');
        size_t imgIdx = std::stoul(sub.substr(0, endNum));
        decodeRes = detail::extractEmbeddedImageFromFile(baseFile, imgIdx, srgb);
    } else {
        std::string filePath = normPath;
        if (filePath.ends_with("#srgb")) {
            filePath = filePath.substr(0, filePath.size() - 5);
        } else if (filePath.ends_with("#linear")) {
            filePath = filePath.substr(0, filePath.size() - 7);
        }
        if (!std::filesystem::exists(filePath)) {
            const std::string errMsg = "Texture file not found on disk: " + filePath;
            ENG_LOG_WARN("AssetManager", errMsg);
            const AssetId id = makeAssetIdFromPath(cacheKey);
            failedIds_.insert(id);
            failedPaths_[cacheKey] = errMsg;
            failedPaths_[normPath] = errMsg;
            return Result<AssetId>::failure(Error(ErrorCode::EntityNotFound, errMsg));
        }
        decodeRes = ImageDecoder::decodeFromFile(filePath, 4, srgb);
    }

    if (!decodeRes.hasValue()) {
        const std::string errMsg = "Failed to decode image from path: " + normPath;
        ENG_LOG_WARN("AssetManager", errMsg);
        const AssetId id = makeAssetIdFromPath(cacheKey);
        failedIds_.insert(id);
        failedPaths_[cacheKey] = errMsg;
        failedPaths_[normPath] = errMsg;
        metrics_.failedAllocationsLifetime++;
        return decodeRes.error();
    }

    const AssetId id = makeAssetIdFromPath(cacheKey);

    TextureEntry entry{};
    entry.metadata.id = id;
    entry.metadata.path = cacheKey;
    entry.metadata.type = AssetType::Texture;
    entry.metadata.loadState = AssetLoadState::Loaded;
    entry.metadata.fileSize = decodeRes.value().sizeInBytes();
    entry.cpuImage = std::move(decodeRes.value());
    entry.gpuTexture = nullptr;
    entry.residencyState = TextureResidencyState::CpuResident;

    pathToId_[cacheKey] = id;
    if (pathToId_.find(normPath) == pathToId_.end()) {
        pathToId_[normPath] = id;
    }
    textureEntries_[id] = std::move(entry);
    updateMemoryMetrics();

    ENG_LOG_INFO("AssetManager", "Loaded texture asset successfully: " + normPath);
    return Result<AssetId>::success(id);
}

Result<AssetId> AssetManager::registerTexture(std::string_view path, ImageData image) {
    if (!image.isValid()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Cannot register invalid ImageData"};
    }

    const std::string normPath = normalizePath(path);
    const bool isSrgb = (image.format == rhi::TextureFormat::RGBA8_SRGB);
    const std::string colorSuffix = (isSrgb ? "#srgb" : "#linear");
    const std::string cacheKey = (normPath.ends_with("#srgb") || normPath.ends_with("#linear"))
                                     ? normPath
                                     : (normPath + colorSuffix);

    const AssetId id = makeAssetIdFromPath(cacheKey);

    TextureEntry entry{};
    entry.metadata.id = id;
    entry.metadata.path = cacheKey;
    entry.metadata.type = AssetType::Texture;
    entry.metadata.loadState = AssetLoadState::Loaded;
    entry.metadata.fileSize = image.sizeInBytes();
    entry.cpuImage = std::move(image);
    entry.gpuTexture = nullptr;
    entry.residencyState = TextureResidencyState::CpuResident;

    pathToId_[cacheKey] = id;
    if (pathToId_.find(normPath) == pathToId_.end()) {
        pathToId_[normPath] = id;
    }
    textureEntries_[id] = std::move(entry);
    updateMemoryMetrics();

    return Result<AssetId>::success(id);
}

Result<GpuTexture*> AssetManager::uploadTexture(AssetId id, rhi::RhiDevice& device,
                                                bool generateMips,
                                                const rhi::SamplerDesc* samplerDesc) {
    device_ = &device;

    auto it = textureEntries_.find(id);
    if (it == textureEntries_.end() || it->second.metadata.loadState != AssetLoadState::Loaded) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Texture asset not loaded in AssetManager"};
    }

    if (it->second.gpuTexture != nullptr &&
        it->second.residencyState == TextureResidencyState::GpuResident) {
        return Result<GpuTexture*>::success(it->second.gpuTexture.get());
    }

    it->second.residencyState = TextureResidencyState::GpuUploadPending;
    auto gpuRes = GpuTexture::create(device, it->second.cpuImage, generateMips, samplerDesc);
    if (!gpuRes.hasValue()) {
        ENG_LOG_ERROR("AssetManager",
                      "Failed to create GPU texture for asset: " + it->second.metadata.path);
        it->second.residencyState = TextureResidencyState::Failed;
        metrics_.failedAllocationsLifetime++;
        return gpuRes.error();
    }

    it->second.gpuTexture = std::move(gpuRes.value());
    it->second.residencyState = TextureResidencyState::GpuResident;
    it->second.residentMipLevels = it->second.gpuTexture->getMipLevels();
    it->second.estimatedGpuBytes = estimateTextureMemoryBytes(
        it->second.gpuTexture->getWidth(), it->second.gpuTexture->getHeight(),
        it->second.residentMipLevels, it->second.gpuTexture->getFormat());
    metrics_.uploadBytesLifetime += it->second.estimatedGpuBytes;

    if (samplerDesc != nullptr) {
        auto cachedSmpRes = samplerCache_.getOrCreate(device, *samplerDesc);
        if (cachedSmpRes.hasValue()) {
            it->second.gpuTexture->setSharedSampler(cachedSmpRes.value());
        }
    }

    updateMemoryMetrics();
    if (deferMaterialRebinding_) {
        materialBindingsDirty_ = true;
    } else {
        rebindMaterialTextures(device);
    }

    return Result<GpuTexture*>::success(it->second.gpuTexture.get());
}

GpuTexture* AssetManager::getGpuTexture(AssetId id) noexcept {
    auto it = textureEntries_.find(id);
    if (it != textureEntries_.end()) {
        return it->second.gpuTexture.get();
    }
    return nullptr;
}

const GpuTexture* AssetManager::getGpuTexture(AssetId id) const noexcept {
    auto it = textureEntries_.find(id);
    if (it != textureEntries_.end()) {
        return it->second.gpuTexture.get();
    }
    return nullptr;
}

Result<GpuTexture*> AssetManager::getOrCreateDefaultTexture(rhi::RhiDevice& device,
                                                            DefaultTextureType type) {
    auto it = defaultTextures_.find(type);
    if (it != defaultTextures_.end() && it->second != nullptr && it->second->isValid()) {
        return Result<GpuTexture*>::success(it->second.get());
    }

    ImageData img{};
    img.width = 1;
    img.height = 1;
    img.channels = 4;

    switch (type) {
    case DefaultTextureType::WhiteSRGB:
        img.format = rhi::TextureFormat::RGBA8_SRGB;
        img.pixels = {255, 255, 255, 255};
        break;
    case DefaultTextureType::WhiteLinear:
        img.format = rhi::TextureFormat::RGBA8_UNORM;
        img.pixels = {255, 255, 255, 255};
        break;
    case DefaultTextureType::BlackSRGB:
        img.format = rhi::TextureFormat::RGBA8_SRGB;
        img.pixels = {0, 0, 0, 255};
        break;
    case DefaultTextureType::FlatNormalLinear:
        img.format = rhi::TextureFormat::RGBA8_UNORM;
        img.pixels = {128, 128, 255, 255};
        break;
    }

    auto createRes = GpuTexture::create(device, img, false);
    if (!createRes.hasValue()) {
        return createRes.error();
    }

    defaultTextures_[type] = std::move(createRes.value());
    return Result<GpuTexture*>::success(defaultTextures_[type].get());
}

const ImageData* AssetManager::getImageData(AssetId id) const noexcept {
    auto it = textureEntries_.find(id);
    if (it != textureEntries_.end() && it->second.metadata.loadState == AssetLoadState::Loaded) {
        return &it->second.cpuImage;
    }
    return nullptr;
}

const ImageData* AssetManager::getImageData(std::string_view path) const noexcept {
    const std::string normPath = normalizePath(path);
    auto it = pathToId_.find(normPath);
    if (it != pathToId_.end()) {
        return getImageData(it->second);
    }
    auto itSrgb = pathToId_.find(normPath + "#srgb");
    if (itSrgb != pathToId_.end()) {
        return getImageData(itSrgb->second);
    }
    auto itLinear = pathToId_.find(normPath + "#linear");
    if (itLinear != pathToId_.end()) {
        return getImageData(itLinear->second);
    }
    return nullptr;
}

const std::vector<Material>* AssetManager::getMaterials(AssetId id) const noexcept {
    auto it = meshEntries_.find(id);
    if (it != meshEntries_.end() && it->second.metadata.loadState == AssetLoadState::Loaded) {
        return &it->second.cpuMesh.materials;
    }
    return nullptr;
}

const AssetMetadata* AssetManager::getMetadata(AssetId id) const noexcept {
    auto it = meshEntries_.find(id);
    if (it != meshEntries_.end()) {
        return &it->second.metadata;
    }
    auto itTex = textureEntries_.find(id);
    if (itTex != textureEntries_.end()) {
        return &itTex->second.metadata;
    }
    return nullptr;
}

AssetId AssetManager::findAssetByPath(std::string_view path) const noexcept {
    const std::string normPath = normalizePath(path);
    auto it = pathToId_.find(normPath);
    if (it != pathToId_.end()) {
        return it->second;
    }
    auto itSrgb = pathToId_.find(normPath + "#srgb");
    if (itSrgb != pathToId_.end()) {
        return itSrgb->second;
    }
    auto itLinear = pathToId_.find(normPath + "#linear");
    if (itLinear != pathToId_.end()) {
        return itLinear->second;
    }
    return {};
}

bool AssetManager::isLoaded(AssetId id) const noexcept {
    auto it = meshEntries_.find(id);
    if (it != meshEntries_.end() && it->second.metadata.loadState == AssetLoadState::Loaded) {
        return true;
    }
    auto itTex = textureEntries_.find(id);
    return itTex != textureEntries_.end() &&
           itTex->second.metadata.loadState == AssetLoadState::Loaded;
}

bool AssetManager::isFailed(AssetId id) const noexcept {
    return failedIds_.find(id) != failedIds_.end();
}

void AssetManager::unload(AssetId id) noexcept {
    failedIds_.erase(id);
    auto it = meshEntries_.find(id);
    if (it != meshEntries_.end()) {
        pathToId_.erase(it->second.metadata.path);
        meshEntries_.erase(it);
        return;
    }
    auto itTex = textureEntries_.find(id);
    if (itTex != textureEntries_.end()) {
        const std::string& path = itTex->second.metadata.path;
        pathToId_.erase(path);
        if (path.ends_with("#srgb")) {
            auto plain = path.substr(0, path.size() - 5);
            auto pIt = pathToId_.find(plain);
            if (pIt != pathToId_.end() && pIt->second == id) {
                pathToId_.erase(pIt);
            }
        } else if (path.ends_with("#linear")) {
            auto plain = path.substr(0, path.size() - 7);
            auto pIt = pathToId_.find(plain);
            if (pIt != pathToId_.end() && pIt->second == id) {
                pathToId_.erase(pIt);
            }
        }
        if (device_ != nullptr && itTex->second.gpuTexture != nullptr) {
            device_->waitIdle();
        }
        textureEntries_.erase(itTex);
        updateMemoryMetrics();
        if (device_ != nullptr) {
            rebindMaterialTextures(*device_);
        }
    }
}

void AssetManager::unloadAll() noexcept {
    if (device_ != nullptr) {
        device_->waitIdle();
        processRetiredResources(*device_);
    }
    retiredTextures_.clear();
    defaultMaterial_.reset();
    defaultTextures_.clear();
    pathToId_.clear();
    meshEntries_.clear();
    textureEntries_.clear();
    failedIds_.clear();
    failedPaths_.clear();
    metrics_ = {};
    if (device_ != nullptr) {
        samplerCache_.clear(*device_);
        device_ = nullptr;
    }
}

void AssetManager::rebindMaterialTextures(rhi::RhiDevice& device) noexcept {
    device_ = &device;

    auto resolveTexSlot =
        [&](const TextureInfo& info,
            DefaultTextureType fallbackType) -> std::pair<rhi::TextureHandle, rhi::SamplerHandle> {
        if (info.isPresent && !info.uri.empty()) {
            AssetId texId = info.textureAsset;
            if (isLoaded(texId)) {
                GpuTexture* tex = getGpuTexture(texId);
                if (tex != nullptr && tex->isValid()) {
                    return {tex->getTexture(), tex->getSampler()};
                }
            }
        }
        auto fallbackRes = getOrCreateDefaultTexture(device, fallbackType);
        if (fallbackRes.hasValue() && fallbackRes.value() != nullptr) {
            return {fallbackRes.value()->getTexture(), fallbackRes.value()->getSampler()};
        }
        return {rhi::TextureHandle{}, rhi::SamplerHandle{}};
    };

    for (auto& [meshId, meshEntry] : meshEntries_) {
        for (size_t matIdx = 0; matIdx < meshEntry.gpuMaterials.size(); ++matIdx) {
            auto& gpuMat = meshEntry.gpuMaterials[matIdx];
            if (gpuMat != nullptr && gpuMat->isValid() &&
                matIdx < meshEntry.cpuMesh.materials.size()) {
                const auto& cpuMat = meshEntry.cpuMesh.materials[matIdx];
                std::array<std::pair<rhi::TextureHandle, rhi::SamplerHandle>, 15> slots{
                    resolveTexSlot(cpuMat.baseColorTexture, DefaultTextureType::WhiteSRGB),
                    resolveTexSlot(cpuMat.metallicRoughnessTexture,
                                   DefaultTextureType::WhiteLinear),
                    resolveTexSlot(cpuMat.normalTexture, DefaultTextureType::FlatNormalLinear),
                    resolveTexSlot(cpuMat.occlusionTexture, DefaultTextureType::WhiteLinear),
                    resolveTexSlot(cpuMat.emissiveTexture, DefaultTextureType::BlackSRGB),
                    resolveTexSlot(cpuMat.clearcoat.texture, DefaultTextureType::WhiteLinear),
                    resolveTexSlot(cpuMat.clearcoat.roughnessTexture,
                                   DefaultTextureType::WhiteLinear),
                    resolveTexSlot(cpuMat.clearcoat.normalTexture,
                                   DefaultTextureType::FlatNormalLinear),
                    resolveTexSlot(cpuMat.transmission.texture, DefaultTextureType::WhiteLinear),
                    resolveTexSlot(cpuMat.volume.thicknessTexture, DefaultTextureType::WhiteLinear),
                    resolveTexSlot(cpuMat.specular.texture, DefaultTextureType::WhiteLinear),
                    resolveTexSlot(cpuMat.specular.colorTexture, DefaultTextureType::WhiteSRGB),
                    resolveTexSlot(cpuMat.anisotropy.texture, DefaultTextureType::FlatNormalLinear),
                    resolveTexSlot(cpuMat.sheen.colorTexture, DefaultTextureType::BlackSRGB),
                    resolveTexSlot(cpuMat.sheen.roughnessTexture, DefaultTextureType::WhiteLinear),
                };
                gpuMat->updateTextureDescriptors(device, slots);
            }
        }
    }
}

Result<void> AssetManager::updateSamplerQuality(const QualitySettings& settings,
                                                rhi::RhiDevice& device) {
    device_ = &device;
    qualitySettings_ = settings;
    const auto& caps = device.getCaps();

    // 1. Destroy existing cached samplers (caller must ensure device is idle via waitIdle())
    samplerCache_.clear(device);

    const float maxCapsAniso =
        caps.features.samplerAnisotropy ? caps.limits.maxSamplerAnisotropy : 1.0F;
    const AnisotropicFiltering effectiveAniso = caps.features.samplerAnisotropy
                                                    ? qualitySettings_.anisotropicFiltering
                                                    : AnisotropicFiltering::Off;

    // 2. Re-evaluate and reassign samplers for all resident textures
    for (auto& [texId, texEntry] : textureEntries_) {
        if (texEntry.gpuTexture != nullptr && texEntry.gpuTexture->isValid()) {
            const uint32_t mipLevels = texEntry.gpuTexture->getMipLevels();
            float minLod = 0.0F;
            if (qualitySettings_.textureQuality == TextureQuality::Medium) {
                minLod = (mipLevels > 1) ? 1.0F : 0.0F;
            } else if (qualitySettings_.textureQuality == TextureQuality::Low) {
                minLod = (mipLevels > 2) ? 2.0F : ((mipLevels > 1) ? 1.0F : 0.0F);
            }
            const float maxLod = static_cast<float>(mipLevels);

            TextureSamplerDesc defaultAuthorDesc{};
            rhi::SamplerDesc smp =
                toRhiSamplerDesc(defaultAuthorDesc, effectiveAniso, maxCapsAniso, minLod, maxLod);

            auto cachedSmpRes = samplerCache_.getOrCreate(device, smp);
            if (cachedSmpRes.hasValue()) {
                texEntry.gpuTexture->setSharedSampler(cachedSmpRes.value());
            }
        }
    }

    // 3. Update descriptor sets on all resident materials
    rebindMaterialTextures(device);

    return Result<void>::success();
}

void AssetManager::setTextureBudgetConfig(const TextureBudgetConfig& config) noexcept {
    budgetConfig_ = config;
    updateMemoryMetrics();
}

TextureResidencyState AssetManager::getTextureResidencyState(AssetId id) const noexcept {
    if (failedIds_.find(id) != failedIds_.end()) {
        return TextureResidencyState::Failed;
    }
    auto it = textureEntries_.find(id);
    if (it != textureEntries_.end()) {
        return it->second.residencyState;
    }
    return TextureResidencyState::Unloaded;
}

void AssetManager::touchTexture(AssetId id, uint64_t frameIndex) noexcept {
    currentFrame_ = std::max(currentFrame_, frameIndex);
    auto it = textureEntries_.find(id);
    if (it != textureEntries_.end()) {
        it->second.lastUsedFrame = frameIndex;
    }
}

void AssetManager::pinTexture(AssetId id, bool pinned) noexcept {
    auto it = textureEntries_.find(id);
    if (it != textureEntries_.end()) {
        it->second.isPinned = pinned;
        updateMemoryMetrics();
    }
}

bool AssetManager::isTexturePinned(AssetId id) const noexcept {
    auto it = textureEntries_.find(id);
    if (it != textureEntries_.end()) {
        return it->second.isPinned;
    }
    return false;
}

const TextureFailureInfo* AssetManager::getTextureFailureInfo(AssetId id) const noexcept {
    auto it = textureEntries_.find(id);
    if (it != textureEntries_.end()) {
        return &it->second.failureInfo;
    }
    return nullptr;
}

uint32_t AssetManager::processRetiredResources(rhi::RhiDevice& device) noexcept {
    const uint64_t completedSerial = device.pollCompletedSubmissionSerial();
    uint32_t reclaimedCount = 0;

    auto it = retiredTextures_.begin();
    while (it != retiredTextures_.end()) {
        if (it->retiredSubmissionSerial <= completedSerial) {
            it->texture.reset();
            metrics_.retiredGpuMemoryBytes =
                (metrics_.retiredGpuMemoryBytes >= it->memoryBytes)
                    ? (metrics_.retiredGpuMemoryBytes - it->memoryBytes)
                    : 0;
            reclaimedCount++;

            auto entryIt = textureEntries_.find(it->id);
            if (entryIt != textureEntries_.end() &&
                entryIt->second.residencyState == TextureResidencyState::EvictionPending) {
                entryIt->second.residencyState = entryIt->second.cpuImage.isValid()
                                                     ? TextureResidencyState::CpuResident
                                                     : TextureResidencyState::Unloaded;
            }

            it = retiredTextures_.erase(it);
        } else {
            ++it;
        }
    }

    metrics_.pendingRetirementsCount = static_cast<uint32_t>(retiredTextures_.size());
    updateMemoryMetrics();
    return reclaimedCount;
}

Result<bool> AssetManager::evictTexture(AssetId id, rhi::RhiDevice& device) noexcept {
    auto it = textureEntries_.find(id);
    if (it == textureEntries_.end()) {
        return Result<bool>::success(false);
    }
    if (it->second.residencyState != TextureResidencyState::GpuResident ||
        it->second.gpuTexture == nullptr) {
        return Result<bool>::success(false);
    }
    if (it->second.isPinned) {
        // Pinned textures are strictly immune from eviction
        return Result<bool>::success(false);
    }

    // Mark as EvictionPending and move texture to deferred queue without waitIdle()
    it->second.residencyState = TextureResidencyState::EvictionPending;
    const uint64_t currentSerial = device.getCurrentSubmissionSerial();
    const size_t bytes = it->second.estimatedGpuBytes;

    retiredTextures_.push_back(RetiredTextureEntry{
        .texture = std::move(it->second.gpuTexture),
        .retiredSubmissionSerial = currentSerial,
        .retiredFrame = currentFrame_,
        .memoryBytes = bytes,
        .id = id,
    });

    metrics_.retiredGpuMemoryBytes += bytes;
    metrics_.residentGpuMemoryBytes =
        (metrics_.residentGpuMemoryBytes >= bytes) ? (metrics_.residentGpuMemoryBytes - bytes) : 0;
    it->second.estimatedGpuBytes = 0;
    it->second.residentMipLevels = 0;
    it->second.residentMipBase = 0;
    metrics_.evictionCountLifetime++;
    metrics_.pendingRetirementsCount = static_cast<uint32_t>(retiredTextures_.size());

    // Rebind material descriptors referencing this texture to default fallbacks
    rebindMaterialTextures(device);

    // Opportunistically reclaim resources whose GPU execution has completed
    processRetiredResources(device);

    updateMemoryMetrics();
    return Result<bool>::success(true);
}

Result<uint32_t> AssetManager::enforceTextureBudget(rhi::RhiDevice& device,
                                                    uint64_t currentFrame) noexcept {
    currentFrame_ = currentFrame;

    // First process any pending retired resources that have finished execution
    processRetiredResources(device);

    if (metrics_.estimatedTotalGpuMemoryBytes <= budgetConfig_.maxGpuMemoryBytes) {
        return Result<uint32_t>::success(0);
    }

    struct EvictCandidate {
        AssetId id;
        uint64_t lastUsedFrame;
        size_t bytes;
    };
    std::vector<EvictCandidate> candidates;
    candidates.reserve(textureEntries_.size());
    for (const auto& [id, entry] : textureEntries_) {
        if (entry.residencyState == TextureResidencyState::GpuResident && !entry.isPinned &&
            entry.gpuTexture != nullptr) {
            candidates.push_back({id, entry.lastUsedFrame, entry.estimatedGpuBytes});
        }
    }

    if (candidates.empty()) {
        return Result<uint32_t>::success(0);
    }

    // Sort by lastUsedFrame ascending (LRU: oldest frame evicted first)
    std::sort(candidates.begin(), candidates.end(),
              [](const EvictCandidate& a, const EvictCandidate& b) {
                  return a.lastUsedFrame < b.lastUsedFrame;
              });

    uint32_t evictedCount = 0;
    for (const auto& cand : candidates) {
        auto it = textureEntries_.find(cand.id);
        if (it != textureEntries_.end() &&
            it->second.residencyState == TextureResidencyState::GpuResident &&
            it->second.gpuTexture != nullptr) {

            it->second.residencyState = TextureResidencyState::EvictionPending;
            const uint64_t currentSerial = device.getCurrentSubmissionSerial();
            const size_t bytes = it->second.estimatedGpuBytes;

            retiredTextures_.push_back(RetiredTextureEntry{
                .texture = std::move(it->second.gpuTexture),
                .retiredSubmissionSerial = currentSerial,
                .retiredFrame = currentFrame_,
                .memoryBytes = bytes,
                .id = cand.id,
            });

            metrics_.retiredGpuMemoryBytes += bytes;
            metrics_.residentGpuMemoryBytes = (metrics_.residentGpuMemoryBytes >= bytes)
                                                  ? (metrics_.residentGpuMemoryBytes - bytes)
                                                  : 0;
            it->second.estimatedGpuBytes = 0;
            it->second.residentMipLevels = 0;
            it->second.residentMipBase = 0;
            metrics_.evictionCountLifetime++;
            evictedCount++;

            // Break when active resident GPU memory is down to target
            if (metrics_.residentGpuMemoryBytes <= budgetConfig_.targetGpuMemoryBytes) {
                break;
            }
        }
    }

    metrics_.pendingRetirementsCount = static_cast<uint32_t>(retiredTextures_.size());
    if (evictedCount > 0) {
        rebindMaterialTextures(device);
        processRetiredResources(device);
        updateMemoryMetrics();
    }

    return Result<uint32_t>::success(evictedCount);
}

Result<void> AssetManager::promoteTextureMips(AssetId id, rhi::RhiDevice& device) noexcept {
    auto it = textureEntries_.find(id);
    if (it == textureEntries_.end() || it->second.gpuTexture == nullptr ||
        !it->second.gpuTexture->isValid()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Texture not resident on GPU for mip promotion"};
    }

    if (it->second.cpuImage.isValid() && it->second.residentMipBase > 0) {
        // Real staged loading: re-allocate full resolution texture
        auto fullGpuRes = GpuTexture::create(device, it->second.cpuImage, true);
        if (fullGpuRes.hasValue()) {
            const uint64_t currentSerial = device.getCurrentSubmissionSerial();
            retiredTextures_.push_back(RetiredTextureEntry{
                .texture = std::move(it->second.gpuTexture),
                .retiredSubmissionSerial = currentSerial,
                .retiredFrame = currentFrame_,
                .memoryBytes = it->second.estimatedGpuBytes,
                .id = id,
            });
            metrics_.retiredGpuMemoryBytes += it->second.estimatedGpuBytes;
            metrics_.pendingRetirementsCount = static_cast<uint32_t>(retiredTextures_.size());

            it->second.gpuTexture = std::move(fullGpuRes.value());
            it->second.residentMipBase = 0;
            it->second.residentMipLevels = it->second.gpuTexture->getMipLevels();
            it->second.estimatedGpuBytes = estimateTextureMemoryBytes(
                it->second.gpuTexture->getWidth(), it->second.gpuTexture->getHeight(),
                it->second.residentMipLevels, it->second.gpuTexture->getFormat());

            rebindMaterialTextures(device);
            processRetiredResources(device);
            updateMemoryMetrics();
            return Result<void>::success();
        }
    }

    // Sampler LOD adjustment towards LOD 0
    const uint32_t mipLevels = it->second.gpuTexture->getMipLevels();
    const float maxLod = static_cast<float>(mipLevels);
    TextureSamplerDesc defaultAuthorDesc{};
    const auto& caps = device.getCaps();
    const float maxCapsAniso =
        caps.features.samplerAnisotropy ? caps.limits.maxSamplerAnisotropy : 1.0F;
    const AnisotropicFiltering effectiveAniso = caps.features.samplerAnisotropy
                                                    ? qualitySettings_.anisotropicFiltering
                                                    : AnisotropicFiltering::Off;
    rhi::SamplerDesc smp =
        toRhiSamplerDesc(defaultAuthorDesc, effectiveAniso, maxCapsAniso, 0.0F, maxLod);
    auto cachedSmpRes = samplerCache_.getOrCreate(device, smp);
    if (cachedSmpRes.hasValue()) {
        it->second.gpuTexture->setSharedSampler(cachedSmpRes.value());
        rebindMaterialTextures(device);
    }
    return Result<void>::success();
}

Result<void> AssetManager::demoteTextureMips(AssetId id, rhi::RhiDevice& device) noexcept {
    auto it = textureEntries_.find(id);
    if (it == textureEntries_.end() || it->second.gpuTexture == nullptr ||
        !it->second.gpuTexture->isValid()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Texture not resident on GPU for mip demotion"};
    }

    const uint32_t originalW = it->second.gpuTexture->getWidth();
    const uint32_t originalH = it->second.gpuTexture->getHeight();
    if (originalW <= 2 || originalH <= 2 || !it->second.cpuImage.isValid()) {
        // Fallback to sampler LOD clamping when image cannot be downscaled or CPU data not retained
        const uint32_t mipLevels = it->second.gpuTexture->getMipLevels();
        const float minLod = (mipLevels > 1) ? 1.0F : 0.0F;
        const float maxLod = static_cast<float>(mipLevels);
        TextureSamplerDesc defaultAuthorDesc{};
        const auto& caps = device.getCaps();
        const float maxCapsAniso =
            caps.features.samplerAnisotropy ? caps.limits.maxSamplerAnisotropy : 1.0F;
        const AnisotropicFiltering effectiveAniso = caps.features.samplerAnisotropy
                                                        ? qualitySettings_.anisotropicFiltering
                                                        : AnisotropicFiltering::Off;
        rhi::SamplerDesc smp =
            toRhiSamplerDesc(defaultAuthorDesc, effectiveAniso, maxCapsAniso, minLod, maxLod);
        auto cachedSmpRes = samplerCache_.getOrCreate(device, smp);
        if (cachedSmpRes.hasValue()) {
            it->second.gpuTexture->setSharedSampler(cachedSmpRes.value());
            rebindMaterialTextures(device);
        }
        return Result<void>::success();
    }

    // Real staged memory reduction: allocate downscaled coarse texture (50% scale, ~25% memory
    // footprint)
    const uint32_t coarseW = std::max(1U, originalW / 2U);
    const uint32_t coarseH = std::max(1U, originalH / 2U);
    const uint32_t channels = it->second.cpuImage.channels;
    ImageData coarseImg{};
    coarseImg.width = coarseW;
    coarseImg.height = coarseH;
    coarseImg.channels = channels;
    coarseImg.format = it->second.cpuImage.format;
    coarseImg.pixels.resize(static_cast<size_t>(coarseW) * coarseH * channels);

    // Box downsample 2x2
    for (uint32_t y = 0; y < coarseH; ++y) {
        for (uint32_t x = 0; x < coarseW; ++x) {
            for (uint32_t c = 0; c < channels; ++c) {
                uint32_t sum = 0;
                uint32_t count = 0;
                for (uint32_t dy = 0; dy < 2; ++dy) {
                    for (uint32_t dx = 0; dx < 2; ++dx) {
                        uint32_t sx = std::min(x * 2 + dx, it->second.cpuImage.width - 1);
                        uint32_t sy = std::min(y * 2 + dy, it->second.cpuImage.height - 1);
                        sum += it->second.cpuImage
                                   .pixels[(sy * it->second.cpuImage.width + sx) * channels + c];
                        count++;
                    }
                }
                coarseImg.pixels[(y * coarseW + x) * channels + c] =
                    static_cast<uint8_t>(sum / count);
            }
        }
    }

    auto coarseGpuRes = GpuTexture::create(device, coarseImg, true);
    if (!coarseGpuRes.hasValue()) {
        return coarseGpuRes.error();
    }

    // Retire old full-resolution GpuTexture to deferred queue
    const uint64_t currentSerial = device.getCurrentSubmissionSerial();
    retiredTextures_.push_back(RetiredTextureEntry{
        .texture = std::move(it->second.gpuTexture),
        .retiredSubmissionSerial = currentSerial,
        .retiredFrame = currentFrame_,
        .memoryBytes = it->second.estimatedGpuBytes,
        .id = id,
    });
    metrics_.retiredGpuMemoryBytes += it->second.estimatedGpuBytes;
    metrics_.pendingRetirementsCount = static_cast<uint32_t>(retiredTextures_.size());

    it->second.gpuTexture = std::move(coarseGpuRes.value());
    it->second.residentMipBase = 1;
    it->second.residentMipLevels = it->second.gpuTexture->getMipLevels();
    it->second.estimatedGpuBytes = estimateTextureMemoryBytes(
        coarseW, coarseH, it->second.residentMipLevels, coarseImg.format);

    rebindMaterialTextures(device);
    processRetiredResources(device);
    updateMemoryMetrics();
    return Result<void>::success();
}

void AssetManager::updateMemoryMetrics() noexcept {
    metrics_.residentGpuMemoryBytes = 0;
    metrics_.estimatedCpuMemoryBytes = 0;
    metrics_.residentTextureCount = 0;
    metrics_.residentMipLevelsTotal = 0;
    metrics_.pinnedBytes = 0;
    metrics_.pinnedTextureCount = 0;

    for (const auto& [id, entry] : textureEntries_) {
        if (entry.cpuImage.isValid()) {
            metrics_.estimatedCpuMemoryBytes += entry.cpuImage.sizeInBytes();
        }
        if (entry.isPinned) {
            metrics_.pinnedTextureCount++;
            if (entry.residencyState == TextureResidencyState::GpuResident) {
                metrics_.pinnedBytes += entry.estimatedGpuBytes;
            }
        }
        if (entry.residencyState == TextureResidencyState::GpuResident &&
            entry.gpuTexture != nullptr && entry.gpuTexture->isValid()) {
            metrics_.residentGpuMemoryBytes += entry.estimatedGpuBytes;
            metrics_.residentTextureCount++;
            metrics_.residentMipLevelsTotal += entry.residentMipLevels;
        }
    }

    metrics_.pendingRetirementsCount = static_cast<uint32_t>(retiredTextures_.size());
    metrics_.estimatedGpuMemoryBytes = metrics_.residentGpuMemoryBytes;
    metrics_.estimatedTotalGpuMemoryBytes = metrics_.residentGpuMemoryBytes +
                                            metrics_.retiredGpuMemoryBytes +
                                            metrics_.pendingUploadBytes;

    if (budgetConfig_.maxGpuMemoryBytes > 0) {
        metrics_.budgetUsageRatio = static_cast<float>(metrics_.estimatedTotalGpuMemoryBytes) /
                                    static_cast<float>(budgetConfig_.maxGpuMemoryBytes);
    } else {
        metrics_.budgetUsageRatio = 0.0F;
    }
    metrics_.isUnderMemoryPressure =
        (metrics_.budgetUsageRatio >= budgetConfig_.memoryPressureThreshold);
}

std::vector<AssetMetadata> AssetManager::getAllMetadata() const {
    std::vector<AssetMetadata> result;
    result.reserve(meshEntries_.size() + textureEntries_.size());
    for (const auto& [id, entry] : meshEntries_) {
        result.push_back(entry.metadata);
    }
    for (const auto& [id, entry] : textureEntries_) {
        result.push_back(entry.metadata);
    }
    return result;
}

} // namespace eng::render
