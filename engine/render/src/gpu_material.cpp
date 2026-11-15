/// @file gpu_material.cpp
/// @brief Concrete implementation of GpuMaterial resource management and descriptor binding.

#include "render/gpu_material.h"

#include "core/log.h"
#include "render/asset_manager.h"

#include <utility>
#include <vector>

namespace eng::render {

GpuMaterial::GpuMaterial(rhi::RhiDevice& device, rhi::DescriptorPoolHandle pool,
                         rhi::DescriptorSetHandle set, rhi::BufferHandle ubo,
                         const MaterialUniformBufferData& uboData) noexcept
    : device_(&device), descriptorPool_(pool), descriptorSet_(set), uniformBuffer_(ubo),
      uboData_(uboData) {}

GpuMaterial::~GpuMaterial() noexcept {
    if (device_ != nullptr) {
        if (uniformBuffer_.isValid()) {
            device_->destroyBuffer(uniformBuffer_);
            uniformBuffer_ = {};
        }
        if (descriptorPool_.isValid()) {
            device_->destroyDescriptorPool(descriptorPool_);
            descriptorPool_ = {};
            descriptorSet_ = {};
        }
        device_ = nullptr;
    }
}

GpuMaterial::GpuMaterial(GpuMaterial&& other) noexcept
    : device_(other.device_), descriptorPool_(other.descriptorPool_),
      descriptorSet_(other.descriptorSet_), uniformBuffer_(other.uniformBuffer_),
      uboData_(other.uboData_) {
    other.device_ = nullptr;
    other.descriptorPool_ = {};
    other.descriptorSet_ = {};
    other.uniformBuffer_ = {};
}

GpuMaterial& GpuMaterial::operator=(GpuMaterial&& other) noexcept {
    if (this != &other) {
        if (device_ != nullptr) {
            if (uniformBuffer_.isValid()) {
                device_->destroyBuffer(uniformBuffer_);
            }
            if (descriptorPool_.isValid()) {
                device_->destroyDescriptorPool(descriptorPool_);
            }
        }
        device_ = other.device_;
        descriptorPool_ = other.descriptorPool_;
        descriptorSet_ = other.descriptorSet_;
        uniformBuffer_ = other.uniformBuffer_;
        uboData_ = other.uboData_;

        other.device_ = nullptr;
        other.descriptorPool_ = {};
        other.descriptorSet_ = {};
        other.uniformBuffer_ = {};
    }
    return *this;
}

Result<std::unique_ptr<GpuMaterial>> GpuMaterial::create(rhi::RhiDevice& device,
                                                         rhi::DescriptorSetLayoutHandle layout,
                                                         const Material& material,
                                                         AssetManager& assetManager) {
    if (!layout.isValid()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Invalid DescriptorSetLayoutHandle in GpuMaterial::create"};
    }

    // 1. Create dedicated descriptor pool for this material
    rhi::DescriptorPoolDesc poolDesc{};
    poolDesc.maxSets = 1;
    poolDesc.poolSizes = {
        rhi::DescriptorPoolSize{.type = rhi::DescriptorType::CombinedImageSampler, .count = 15},
        rhi::DescriptorPoolSize{.type = rhi::DescriptorType::UniformBuffer, .count = 1},
    };
    poolDesc.debugName = "GpuMaterial_DescriptorPool";

    auto poolRes = device.createDescriptorPool(poolDesc);
    if (!poolRes.hasValue()) {
        return poolRes.error();
    }
    const rhi::DescriptorPoolHandle poolHandle = poolRes.value();

    // 2. Allocate descriptor set
    auto setRes = device.allocateDescriptorSet(poolHandle, layout);
    if (!setRes.hasValue()) {
        device.destroyDescriptorPool(poolHandle);
        return setRes.error();
    }
    const rhi::DescriptorSetHandle setHandle = setRes.value();

    // 3. Resolve texture and sampler slots (falling back to engine 1x1 defaults if absent)
    auto resolveSlot =
        [&](const TextureInfo& info, DefaultTextureType fallbackType,
            uint32_t& outHasTex) -> std::pair<rhi::TextureHandle, rhi::SamplerHandle> {
        if (info.isPresent && !info.uri.empty()) {
            AssetId texId = info.textureAsset;
            if (!assetManager.isLoaded(texId)) {
                if (!assetManager.isFailed(texId)) {
                    auto loadRes = assetManager.loadTexture(info.uri, info.colorSpace ==
                                                                          TextureColorSpace::sRGB);
                    if (loadRes.hasValue()) {
                        texId = loadRes.value();
                    }
                }
            }
            if (assetManager.isLoaded(texId)) {
                uint32_t mipLevels = 1;
                const auto* img = assetManager.getImageData(texId);
                if (img != nullptr) {
                    const uint32_t maxDim = std::max(img->width, img->height);
                    mipLevels =
                        static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(maxDim)))) +
                        1U;
                }
                const auto& quality = assetManager.getQualitySettings();
                const auto& caps = device.getCaps();
                float minLod = 0.0F;
                if (quality.textureQuality == TextureQuality::Medium) {
                    minLod = (mipLevels > 1) ? 1.0F : 0.0F;
                } else if (quality.textureQuality == TextureQuality::Low) {
                    minLod = (mipLevels > 2) ? 2.0F : ((mipLevels > 1) ? 1.0F : 0.0F);
                }
                const float maxLod = static_cast<float>(mipLevels);
                const float maxCapsAniso =
                    caps.features.samplerAnisotropy ? caps.limits.maxSamplerAnisotropy : 1.0F;
                const AnisotropicFiltering effectiveAniso = caps.features.samplerAnisotropy
                                                                ? quality.anisotropicFiltering
                                                                : AnisotropicFiltering::Off;

                rhi::SamplerDesc smp =
                    toRhiSamplerDesc(info.sampler, effectiveAniso, maxCapsAniso, minLod, maxLod);
                auto smpRes = assetManager.getSamplerCache().getOrCreate(device, smp);
                rhi::SamplerHandle cachedSampler{};
                if (smpRes.hasValue()) {
                    cachedSampler = smpRes.value();
                }

                auto upRes = assetManager.uploadTexture(texId, device, true, &smp);
                if (upRes.hasValue() && upRes.value() != nullptr && upRes.value()->isValid()) {
                    if (cachedSampler.isValid()) {
                        upRes.value()->setSharedSampler(cachedSampler);
                    }
                    outHasTex = 1u;
                    return {upRes.value()->getTexture(), upRes.value()->getSampler()};
                }
            }
        }
        outHasTex = 0u;
        auto fallbackRes = assetManager.getOrCreateDefaultTexture(device, fallbackType);
        if (fallbackRes.hasValue() && fallbackRes.value() != nullptr) {
            return {fallbackRes.value()->getTexture(), fallbackRes.value()->getSampler()};
        }
        return {rhi::TextureHandle{}, rhi::SamplerHandle{}};
    };

    MaterialUniformBufferData uboData{};
    uboData.baseColorFactor = material.baseColor;
    uboData.emissiveFactorAndAlphaCutoff =
        Vec4{material.emissiveFactor.x, material.emissiveFactor.y, material.emissiveFactor.z,
             material.alphaCutoff};
    uboData.metallicFactor = material.metallic;
    uboData.roughnessFactor = material.roughness;
    uboData.normalScale = material.normalTexture.scale;
    uboData.occlusionStrength = material.occlusionTexture.strength;
    uboData.alphaMode = static_cast<uint32_t>(material.alphaMode);
    uboData.doubleSided = material.doubleSided ? 1u : 0u;
    uboData.transmissionFactor = material.transmissionFactor;
    uboData.ior = material.ior;
    uboData.clearcoatFactor = material.clearcoat.factor;
    uboData.clearcoatRoughness = material.clearcoat.roughnessFactor;
    uboData.clearcoatNormalScale = material.clearcoat.normalTexture.scale;
    uboData.thicknessFactor = material.volume.thicknessFactor;
    uboData.attenuationDistance = material.volume.attenuationDistance;
    uboData.specularFactor = material.specular.factor;

    const auto baseColorSlot = resolveSlot(material.baseColorTexture, DefaultTextureType::WhiteSRGB,
                                           uboData.hasBaseColorTex);
    const auto mrSlot =
        resolveSlot(material.metallicRoughnessTexture, DefaultTextureType::WhiteLinear,
                    uboData.hasMetallicRoughnessTex);
    const auto normSlot = resolveSlot(material.normalTexture, DefaultTextureType::FlatNormalLinear,
                                      uboData.hasNormalTex);
    const auto occSlot = resolveSlot(material.occlusionTexture, DefaultTextureType::WhiteLinear,
                                     uboData.hasOcclusionTex);
    const auto emissiveSlot = resolveSlot(material.emissiveTexture, DefaultTextureType::BlackSRGB,
                                          uboData.hasEmissiveTex);

    const auto clearcoatSlot = resolveSlot(
        material.clearcoat.texture, DefaultTextureType::WhiteLinear, uboData.hasClearcoatTex);
    const auto clearcoatRoughnessSlot =
        resolveSlot(material.clearcoat.roughnessTexture, DefaultTextureType::WhiteLinear,
                    uboData.hasClearcoatRoughnessTex);
    const auto clearcoatNormalSlot =
        resolveSlot(material.clearcoat.normalTexture, DefaultTextureType::FlatNormalLinear,
                    uboData.hasClearcoatNormalTex);
    const auto transmissionSlot = resolveSlot(
        material.transmission.texture, DefaultTextureType::WhiteLinear, uboData.hasTransmissionTex);
    const auto thicknessSlot = resolveSlot(
        material.volume.thicknessTexture, DefaultTextureType::WhiteLinear, uboData.hasThicknessTex);

    uint32_t hasSpecTex = 0u;
    const auto specularSlot =
        resolveSlot(material.specular.texture, DefaultTextureType::WhiteLinear, hasSpecTex);

    uint32_t hasSpecColorTex = 0u;
    const auto specularColorSlot =
        resolveSlot(material.specular.colorTexture, DefaultTextureType::WhiteSRGB, hasSpecColorTex);

    const auto anisotropySlot =
        resolveSlot(material.anisotropy.texture, DefaultTextureType::FlatNormalLinear,
                    uboData.hasAnisotropyTex);

    uint32_t hasSheenColorTex = 0u;
    const auto sheenColorSlot =
        resolveSlot(material.sheen.colorTexture, DefaultTextureType::BlackSRGB, hasSheenColorTex);

    uint32_t hasSheenRoughnessTex = 0u;
    const auto sheenRoughnessSlot = resolveSlot(
        material.sheen.roughnessTexture, DefaultTextureType::WhiteLinear, hasSheenRoughnessTex);

    uboData.anisotropyStrength = material.anisotropy.strength;
    uboData.anisotropyRotation = material.anisotropy.rotation;
    uboData.sheenRoughness = material.sheen.roughnessFactor;
    uboData.sheenColorFactorAndFlags = Vec4{
        material.sheen.colorFactor.x,
        material.sheen.colorFactor.y,
        material.sheen.colorFactor.z,
        hasSheenColorTex != 0u ? (hasSheenRoughnessTex != 0u ? 3.0F : 1.0F)
                               : (hasSheenRoughnessTex != 0u ? 2.0F : 0.0F),
    };

    uboData.attenuationColorAndHasSpecTex = Vec4{
        material.volume.attenuationColor.x,
        material.volume.attenuationColor.y,
        material.volume.attenuationColor.z,
        hasSpecTex != 0u ? 1.0F : 0.0F,
    };
    uboData.specularColorFactorAndHasColorTex = Vec4{
        material.specular.colorFactor.x,
        material.specular.colorFactor.y,
        material.specular.colorFactor.z,
        hasSpecColorTex != 0u ? 1.0F : 0.0F,
    };

    // 4. Create and upload uniform buffer
    rhi::BufferDesc uboDesc{};
    uboDesc.sizeInBytes = sizeof(MaterialUniformBufferData);
    uboDesc.usage = rhi::BufferUsageFlags::Uniform | rhi::BufferUsageFlags::TransferDst;
    uboDesc.memoryUsage = rhi::MemoryUsage::CpuToGpu;
    uboDesc.debugName = "MaterialUniformBuffer";

    auto uboRes = device.createBuffer(uboDesc);
    if (!uboRes.hasValue()) {
        device.destroyDescriptorPool(poolHandle);
        return uboRes.error();
    }
    const rhi::BufferHandle uboHandle = uboRes.value();

    auto writeRes = device.writeBuffer(uboHandle, 0, &uboData, sizeof(MaterialUniformBufferData));
    if (!writeRes.hasValue()) {
        device.destroyBuffer(uboHandle);
        device.destroyDescriptorPool(poolHandle);
        return writeRes.error();
    }

    // 5. Update descriptor set with textures and uniform buffer (16 writes total)
    std::vector<rhi::DescriptorWrite> writes;
    writes.reserve(16);
    writes.push_back(rhi::DescriptorWrite::makeCombinedImageSampler(0, baseColorSlot.first,
                                                                    baseColorSlot.second));
    writes.push_back(
        rhi::DescriptorWrite::makeCombinedImageSampler(1, mrSlot.first, mrSlot.second));
    writes.push_back(
        rhi::DescriptorWrite::makeCombinedImageSampler(2, normSlot.first, normSlot.second));
    writes.push_back(
        rhi::DescriptorWrite::makeCombinedImageSampler(3, occSlot.first, occSlot.second));
    writes.push_back(
        rhi::DescriptorWrite::makeCombinedImageSampler(4, emissiveSlot.first, emissiveSlot.second));
    writes.push_back(rhi::DescriptorWrite::makeBuffer(
        5, rhi::DescriptorType::UniformBuffer, uboHandle, 0, sizeof(MaterialUniformBufferData)));
    writes.push_back(rhi::DescriptorWrite::makeCombinedImageSampler(6, clearcoatSlot.first,
                                                                    clearcoatSlot.second));
    writes.push_back(rhi::DescriptorWrite::makeCombinedImageSampler(7, clearcoatRoughnessSlot.first,
                                                                    clearcoatRoughnessSlot.second));
    writes.push_back(rhi::DescriptorWrite::makeCombinedImageSampler(8, clearcoatNormalSlot.first,
                                                                    clearcoatNormalSlot.second));
    writes.push_back(rhi::DescriptorWrite::makeCombinedImageSampler(9, transmissionSlot.first,
                                                                    transmissionSlot.second));
    writes.push_back(rhi::DescriptorWrite::makeCombinedImageSampler(10, thicknessSlot.first,
                                                                    thicknessSlot.second));
    writes.push_back(rhi::DescriptorWrite::makeCombinedImageSampler(11, specularSlot.first,
                                                                    specularSlot.second));
    writes.push_back(rhi::DescriptorWrite::makeCombinedImageSampler(12, specularColorSlot.first,
                                                                    specularColorSlot.second));
    writes.push_back(rhi::DescriptorWrite::makeCombinedImageSampler(13, anisotropySlot.first,
                                                                    anisotropySlot.second));
    writes.push_back(rhi::DescriptorWrite::makeCombinedImageSampler(14, sheenColorSlot.first,
                                                                    sheenColorSlot.second));
    writes.push_back(rhi::DescriptorWrite::makeCombinedImageSampler(15, sheenRoughnessSlot.first,
                                                                    sheenRoughnessSlot.second));

    device.updateDescriptorSet(setHandle, writes);

    return Result<std::unique_ptr<GpuMaterial>>::success(std::unique_ptr<GpuMaterial>(
        new GpuMaterial(device, poolHandle, setHandle, uboHandle, uboData)));
}

void GpuMaterial::updateTextureDescriptors(
    rhi::RhiDevice& device,
    const std::array<std::pair<rhi::TextureHandle, rhi::SamplerHandle>, 15>& textureSlots) {
    if (!descriptorSet_.isValid()) {
        return;
    }
    std::array<rhi::DescriptorWrite, 15> writes{};
    for (uint32_t b = 0; b < 5; ++b) {
        writes[b] = rhi::DescriptorWrite::makeCombinedImageSampler(b, textureSlots[b].first,
                                                                   textureSlots[b].second);
    }
    for (uint32_t b = 5; b < 15; ++b) {
        writes[b] = rhi::DescriptorWrite::makeCombinedImageSampler(b + 1, textureSlots[b].first,
                                                                   textureSlots[b].second);
    }
    std::ignore = device.updateDescriptorSet(descriptorSet_, writes);
}

} // namespace eng::render
