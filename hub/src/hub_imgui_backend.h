/// @file hub_imgui_backend.h
/// @brief Pure-RHI Dear ImGui rendering backend for Hikari Machine Hub (zero graphics API headers).

#pragma once

#include "render/gpu_texture.h"
#include "rhi/rhi_types.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

struct ImDrawData;

namespace eng::rhi {
class RhiDevice;
class RhiCommandList;
} // namespace eng::rhi

namespace eng::hub {

/// @class HubImGuiBackend
/// @brief Standalone graphics-API-agnostic Dear ImGui renderer for Hikari Machine Hub.
class HubImGuiBackend {
public:
    HubImGuiBackend() = default;
    ~HubImGuiBackend();

    HubImGuiBackend(const HubImGuiBackend&) = delete;
    HubImGuiBackend& operator=(const HubImGuiBackend&) = delete;

    [[nodiscard]] bool initialize(rhi::RhiDevice& device);
    void shutdown();
    [[nodiscard]] bool isInitialized() const noexcept { return initialized_; }

    [[nodiscard]] uint64_t registerTexture(rhi::TextureHandle texture, rhi::SamplerHandle sampler);
    void unregisterTexture(uint64_t textureId);
    void renderDrawData(ImDrawData* drawData, rhi::RhiCommandList& cmdList);

private:
    struct FrameBuffers {
        rhi::BufferHandle vertexBuffer{};
        rhi::BufferHandle indexBuffer{};
        size_t vertexCapacityBytes{0};
        size_t indexCapacityBytes{0};
    };

    static constexpr uint32_t kFrameSlots = 4;

    rhi::RhiDevice* device_{nullptr};
    rhi::DescriptorSetLayoutHandle descriptorSetLayout_{};
    rhi::DescriptorPoolHandle descriptorPool_{};
    rhi::PipelineLayoutHandle pipelineLayout_{};
    rhi::ShaderHandle vertexShader_{};
    rhi::ShaderHandle fragmentShader_{};
    rhi::PipelineHandle pipeline_{};
    std::unique_ptr<render::GpuTexture> fontTexture_{};
    uint64_t fontTextureId_{0};

    std::array<FrameBuffers, kFrameSlots> frameBuffers_{};
    uint32_t currentFrameSlot_{0};

    std::vector<rhi::DescriptorSetHandle> allocatedSets_{};
    std::vector<rhi::DescriptorSetHandle> freeSets_{};
    bool initialized_{false};
};

} // namespace eng::hub
