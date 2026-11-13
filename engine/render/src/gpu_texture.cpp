/// @file gpu_texture.cpp
/// @brief GPU texture allocation, staging upload, and sampler creation implementation.

#include "render/gpu_texture.h"

#include "core/log.h"
#include "rhi/buffer.h"
#include "rhi/command_list.h"

#include <algorithm>
#include <cmath>

namespace eng::render {

GpuTexture::GpuTexture(rhi::RhiDevice& device, rhi::TextureHandle texture,
                       rhi::SamplerHandle sampler, uint32_t width, uint32_t height,
                       uint32_t mipLevels, rhi::TextureFormat format) noexcept
    : device_(&device), texture_(texture), sampler_(sampler), width_(width), height_(height),
      mipLevels_(mipLevels), format_(format) {}

GpuTexture::~GpuTexture() noexcept {
    if (device_ != nullptr) {
        if (sampler_.isValid()) {
            device_->destroySampler(sampler_);
            sampler_ = {};
        }
        if (texture_.isValid()) {
            device_->destroyTexture(texture_);
            texture_ = {};
        }
        device_ = nullptr;
    }
}

GpuTexture::GpuTexture(GpuTexture&& other) noexcept
    : device_(other.device_), texture_(other.texture_), sampler_(other.sampler_),
      width_(other.width_), height_(other.height_), mipLevels_(other.mipLevels_),
      format_(other.format_) {
    other.device_ = nullptr;
    other.texture_ = {};
    other.sampler_ = {};
    other.width_ = 0;
    other.height_ = 0;
    other.mipLevels_ = 1;
}

GpuTexture& GpuTexture::operator=(GpuTexture&& other) noexcept {
    if (this != &other) {
        if (device_ != nullptr) {
            if (sampler_.isValid()) {
                device_->destroySampler(sampler_);
            }
            if (texture_.isValid()) {
                device_->destroyTexture(texture_);
            }
        }
        device_ = other.device_;
        texture_ = other.texture_;
        sampler_ = other.sampler_;
        width_ = other.width_;
        height_ = other.height_;
        mipLevels_ = other.mipLevels_;
        format_ = other.format_;

        other.device_ = nullptr;
        other.texture_ = {};
        other.sampler_ = {};
        other.width_ = 0;
        other.height_ = 0;
        other.mipLevels_ = 1;
    }
    return *this;
}

Result<std::unique_ptr<GpuTexture>> GpuTexture::create(rhi::RhiDevice& device,
                                                       const ImageData& imageData,
                                                       bool generateMips,
                                                       const rhi::SamplerDesc* customSamplerDesc) {
    if (!imageData.isValid()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Cannot create GpuTexture from invalid ImageData"};
    }

    uint32_t mipLevels = 1;
    if (generateMips) {
        const uint32_t maxDim = std::max(imageData.width, imageData.height);
        mipLevels = static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(maxDim)))) + 1U;
    }

    rhi::TextureUsageFlags usage =
        rhi::TextureUsageFlags::Sampled | rhi::TextureUsageFlags::TransferDst;
    if (generateMips && mipLevels > 1) {
        usage |= rhi::TextureUsageFlags::TransferSrc;
    }

    rhi::TextureDesc texDesc{
        .type = rhi::TextureType::Texture2D,
        .format = imageData.format,
        .width = imageData.width,
        .height = imageData.height,
        .depth = 1,
        .mipLevels = mipLevels,
        .arrayLayers = 1,
        .usage = usage,
        .debugName = "GpuTexture",
    };

    auto texRes = device.createTexture(texDesc);
    if (!texRes.hasValue()) {
        return texRes.error();
    }
    rhi::TextureHandle texture = texRes.value();

    // Configure and create sampler
    rhi::SamplerDesc defaultSamplerDesc{
        .minFilter = rhi::Filter::Linear,
        .magFilter = rhi::Filter::Linear,
        .mipmapMode = rhi::Filter::Linear,
        .addressU = rhi::SamplerAddressMode::Repeat,
        .addressV = rhi::SamplerAddressMode::Repeat,
        .addressW = rhi::SamplerAddressMode::Repeat,
        .anisotropyEnable = true,
        .maxAnisotropy = 16.0F,
        .minLod = 0.0F,
        .maxLod = static_cast<float>(mipLevels),
        .mipLodBias = 0.0F,
        .compareEnable = false,
        .debugName = "GpuTextureSampler",
    };

    const rhi::SamplerDesc& samplerToCreate =
        (customSamplerDesc != nullptr) ? *customSamplerDesc : defaultSamplerDesc;
    auto samplerRes = device.createSampler(samplerToCreate);
    if (!samplerRes.hasValue()) {
        device.destroyTexture(texture);
        return samplerRes.error();
    }
    rhi::SamplerHandle sampler = samplerRes.value();

    // Create host-visible staging buffer
    rhi::BufferDesc stagingDesc{
        .sizeInBytes = imageData.sizeInBytes(),
        .usage = rhi::BufferUsageFlags::TransferSrc,
        .memoryUsage = rhi::MemoryUsage::CpuToGpu,
        .debugName = "TextureStagingBuffer",
    };
    auto stagingRes = device.createBuffer(stagingDesc);
    if (!stagingRes.hasValue()) {
        device.destroySampler(sampler);
        device.destroyTexture(texture);
        return stagingRes.error();
    }
    rhi::BufferHandle stagingBuf = stagingRes.value();

    auto writeRes =
        device.writeBuffer(stagingBuf, 0, imageData.pixels.data(), imageData.sizeInBytes());
    if (!writeRes.hasValue()) {
        device.destroyBuffer(stagingBuf);
        device.destroySampler(sampler);
        device.destroyTexture(texture);
        return writeRes.error();
    }

    auto cmdRes = device.createCommandList();
    if (!cmdRes.hasValue()) {
        device.destroyBuffer(stagingBuf);
        device.destroySampler(sampler);
        device.destroyTexture(texture);
        return cmdRes.error();
    }
    auto cmd = std::move(cmdRes.value());

    cmd->begin();
    cmd->transitionTextureLayout(texture, rhi::TextureLayout::Undefined,
                                 rhi::TextureLayout::TransferDst, 0, 1, 0, 1);
    cmd->copyBufferToTexture(stagingBuf, texture, 0, 0, 0);

    if (generateMips && mipLevels > 1) {
        cmd->generateMipmaps(texture);
    } else {
        cmd->transitionTextureLayout(texture, rhi::TextureLayout::TransferDst,
                                     rhi::TextureLayout::ShaderReadOnly, 0, 1, 0, 1);
    }
    cmd->end();

    auto submitRes = device.submit(cmd.get());
    device.destroyBuffer(stagingBuf);

    if (!submitRes.hasValue()) {
        device.destroySampler(sampler);
        device.destroyTexture(texture);
        return submitRes.error();
    }

    return std::unique_ptr<GpuTexture>(new GpuTexture(
        device, texture, sampler, imageData.width, imageData.height, mipLevels, imageData.format));
}

Result<std::unique_ptr<GpuTexture>>
GpuTexture::createCubemap(rhi::RhiDevice& device, const CubemapData& cubemapData,
                          const rhi::SamplerDesc* customSamplerDesc) {
    if (!cubemapData.isValid()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Cannot create GpuTexture from invalid CubemapData"};
    }

    const uint32_t size = cubemapData.size;
    const uint32_t mipLevels = cubemapData.mipLevels;

    rhi::TextureDesc texDesc{
        .type = rhi::TextureType::TextureCube,
        .format = cubemapData.format,
        .width = size,
        .height = size,
        .depth = 1,
        .mipLevels = mipLevels,
        .arrayLayers = 6,
        .usage = rhi::TextureUsageFlags::Sampled | rhi::TextureUsageFlags::TransferDst,
        .debugName = "GpuCubemapTexture",
    };

    auto texRes = device.createTexture(texDesc);
    if (!texRes.hasValue()) {
        return texRes.error();
    }
    rhi::TextureHandle texture = texRes.value();

    rhi::SamplerDesc defaultSamplerDesc{
        .minFilter = rhi::Filter::Linear,
        .magFilter = rhi::Filter::Linear,
        .mipmapMode = rhi::Filter::Linear,
        .addressU = rhi::SamplerAddressMode::ClampToEdge,
        .addressV = rhi::SamplerAddressMode::ClampToEdge,
        .addressW = rhi::SamplerAddressMode::ClampToEdge,
        .anisotropyEnable = true,
        .maxAnisotropy = 16.0F,
        .minLod = 0.0F,
        .maxLod = static_cast<float>(mipLevels),
        .mipLodBias = 0.0F,
        .compareEnable = false,
        .debugName = "GpuCubemapSampler",
    };

    const rhi::SamplerDesc& samplerToCreate =
        (customSamplerDesc != nullptr) ? *customSamplerDesc : defaultSamplerDesc;
    auto samplerRes = device.createSampler(samplerToCreate);
    if (!samplerRes.hasValue()) {
        device.destroyTexture(texture);
        return samplerRes.error();
    }
    rhi::SamplerHandle sampler = samplerRes.value();

    size_t totalBytes = 0;
    for (uint32_t m = 0; m < mipLevels; ++m) {
        for (uint32_t f = 0; f < 6; ++f) {
            totalBytes += cubemapData.mips[m].faces[f].halfPixels.size() * sizeof(uint16_t);
        }
    }

    rhi::BufferDesc stagingDesc{
        .sizeInBytes = totalBytes,
        .usage = rhi::BufferUsageFlags::TransferSrc,
        .memoryUsage = rhi::MemoryUsage::CpuToGpu,
        .debugName = "CubemapStagingBuffer",
    };
    auto stagingRes = device.createBuffer(stagingDesc);
    if (!stagingRes.hasValue()) {
        device.destroySampler(sampler);
        device.destroyTexture(texture);
        return stagingRes.error();
    }
    rhi::BufferHandle stagingBuf = stagingRes.value();

    struct CopyOp {
        size_t offset;
        uint32_t mip;
        uint32_t face;
    };
    std::vector<CopyOp> copyOps;
    size_t currentOffset = 0;

    for (uint32_t m = 0; m < mipLevels; ++m) {
        for (uint32_t f = 0; f < 6; ++f) {
            const auto& face = cubemapData.mips[m].faces[f];
            const size_t faceBytes = face.halfPixels.size() * sizeof(uint16_t);
            auto writeRes =
                device.writeBuffer(stagingBuf, currentOffset, face.halfPixels.data(), faceBytes);
            if (!writeRes.hasValue()) {
                device.destroyBuffer(stagingBuf);
                device.destroySampler(sampler);
                device.destroyTexture(texture);
                return writeRes.error();
            }
            copyOps.push_back(CopyOp{.offset = currentOffset, .mip = m, .face = f});
            currentOffset += faceBytes;
        }
    }

    auto cmdRes = device.createCommandList();
    if (!cmdRes.hasValue()) {
        device.destroyBuffer(stagingBuf);
        device.destroySampler(sampler);
        device.destroyTexture(texture);
        return cmdRes.error();
    }
    auto cmd = std::move(cmdRes.value());

    cmd->begin();
    cmd->transitionTextureLayout(texture, rhi::TextureLayout::Undefined,
                                 rhi::TextureLayout::TransferDst, 0, mipLevels, 0, 6);

    for (const auto& op : copyOps) {
        cmd->copyBufferToTexture(stagingBuf, texture, op.offset, op.mip, op.face);
    }

    cmd->transitionTextureLayout(texture, rhi::TextureLayout::TransferDst,
                                 rhi::TextureLayout::ShaderReadOnly, 0, mipLevels, 0, 6);
    cmd->end();

    auto submitRes = device.submit(cmd.get());
    device.destroyBuffer(stagingBuf);

    if (!submitRes.hasValue()) {
        device.destroySampler(sampler);
        device.destroyTexture(texture);
        return submitRes.error();
    }

    return std::unique_ptr<GpuTexture>(
        new GpuTexture(device, texture, sampler, size, size, mipLevels, cubemapData.format));
}

Result<std::unique_ptr<GpuTexture>> GpuTexture::createBrdfLut(rhi::RhiDevice& device,
                                                              const BrdfLutData& brdfLutData) {
    if (!brdfLutData.isValid()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Cannot create GpuTexture from invalid BrdfLutData"};
    }

    const uint32_t width = brdfLutData.width;
    const uint32_t height = brdfLutData.height;

    rhi::TextureDesc texDesc{
        .type = rhi::TextureType::Texture2D,
        .format = brdfLutData.format,
        .width = width,
        .height = height,
        .depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .usage = rhi::TextureUsageFlags::Sampled | rhi::TextureUsageFlags::TransferDst,
        .debugName = "GpuBrdfLutTexture",
    };

    auto texRes = device.createTexture(texDesc);
    if (!texRes.hasValue()) {
        return texRes.error();
    }
    rhi::TextureHandle texture = texRes.value();

    rhi::SamplerDesc defaultSamplerDesc{
        .minFilter = rhi::Filter::Linear,
        .magFilter = rhi::Filter::Linear,
        .mipmapMode = rhi::Filter::Nearest,
        .addressU = rhi::SamplerAddressMode::ClampToEdge,
        .addressV = rhi::SamplerAddressMode::ClampToEdge,
        .addressW = rhi::SamplerAddressMode::ClampToEdge,
        .anisotropyEnable = false,
        .maxAnisotropy = 1.0F,
        .minLod = 0.0F,
        .maxLod = 0.0F,
        .mipLodBias = 0.0F,
        .compareEnable = false,
        .debugName = "GpuBrdfLutSampler",
    };

    auto samplerRes = device.createSampler(defaultSamplerDesc);
    if (!samplerRes.hasValue()) {
        device.destroyTexture(texture);
        return samplerRes.error();
    }
    rhi::SamplerHandle sampler = samplerRes.value();

    const size_t totalBytes = brdfLutData.halfPixels.size() * sizeof(uint16_t);
    rhi::BufferDesc stagingDesc{
        .sizeInBytes = totalBytes,
        .usage = rhi::BufferUsageFlags::TransferSrc,
        .memoryUsage = rhi::MemoryUsage::CpuToGpu,
        .debugName = "BrdfLutStagingBuffer",
    };
    auto stagingRes = device.createBuffer(stagingDesc);
    if (!stagingRes.hasValue()) {
        device.destroySampler(sampler);
        device.destroyTexture(texture);
        return stagingRes.error();
    }
    rhi::BufferHandle stagingBuf = stagingRes.value();

    auto writeRes = device.writeBuffer(stagingBuf, 0, brdfLutData.halfPixels.data(), totalBytes);
    if (!writeRes.hasValue()) {
        device.destroyBuffer(stagingBuf);
        device.destroySampler(sampler);
        device.destroyTexture(texture);
        return writeRes.error();
    }

    auto cmdRes = device.createCommandList();
    if (!cmdRes.hasValue()) {
        device.destroyBuffer(stagingBuf);
        device.destroySampler(sampler);
        device.destroyTexture(texture);
        return cmdRes.error();
    }
    auto cmd = std::move(cmdRes.value());

    cmd->begin();
    cmd->transitionTextureLayout(texture, rhi::TextureLayout::Undefined,
                                 rhi::TextureLayout::TransferDst, 0, 1, 0, 1);
    cmd->copyBufferToTexture(stagingBuf, texture, 0, 0, 0);
    cmd->transitionTextureLayout(texture, rhi::TextureLayout::TransferDst,
                                 rhi::TextureLayout::ShaderReadOnly, 0, 1, 0, 1);
    cmd->end();

    auto submitRes = device.submit(cmd.get());
    device.destroyBuffer(stagingBuf);

    if (!submitRes.hasValue()) {
        device.destroySampler(sampler);
        device.destroyTexture(texture);
        return submitRes.error();
    }

    return std::unique_ptr<GpuTexture>(
        new GpuTexture(device, texture, sampler, width, height, 1, brdfLutData.format));
}

} // namespace eng::render
