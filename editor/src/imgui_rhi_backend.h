/// @file imgui_rhi_backend.h
/// @brief Pure-RHI Dear ImGui rendering backend (zero graphics API headers).

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

namespace eng::editor {

/// @class ImGuiRhiBackend
/// @brief Graphics-API-agnostic Dear ImGui renderer built strictly on Hikari's RHI.
class ImGuiRhiBackend {
public:
    ImGuiRhiBackend() = default;
    ~ImGuiRhiBackend();

    ImGuiRhiBackend(const ImGuiRhiBackend&) = delete;
    ImGuiRhiBackend& operator=(const ImGuiRhiBackend&) = delete;

    /// Initializes shaders, pipeline, descriptor pool, and font atlas through RHI.
    [[nodiscard]] bool initialize(rhi::RhiDevice& device);

    /// Releases all RHI resources owned by this backend.
    void shutdown();

    /// Returns true if the RHI pipeline and font atlas are initialized.
    [[nodiscard]] bool isInitialized() const noexcept { return initialized_; }

    /// Allocates a descriptor set binding (texture + sampler) usable as an ImTextureID.
    [[nodiscard]] uint64_t registerTexture(rhi::TextureHandle texture, rhi::SamplerHandle sampler);

    /// Returns a previously registered texture descriptor set to the free list.
    void unregisterTexture(uint64_t textureId);

    /// Uploads ImDrawData vertex/index buffers and records RHI draw commands.
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

} // namespace eng::editor
