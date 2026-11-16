/// @file imgui_rhi_backend.cpp
/// @brief Pure-RHI Dear ImGui rendering backend implementation.

#include "imgui_rhi_backend.h"

#include "core/log.h"
#include "render/image.h"
#include "rhi/buffer.h"
#include "rhi/command_list.h"
#include "rhi/descriptor.h"
#include "rhi/pipeline.h"
#include "rhi/rhi_device.h"
#include "rhi/shader.h"

#include <algorithm>
#include <cstring>
#include <imgui.h>

namespace eng::editor {

namespace {

static constexpr uint32_t kImGuiVertSpirv[] = {
    0x07230203u, 0x00010000u, 0x000d000bu, 0x0000002eu, 0x00000000u, 0x00020011u, 0x00000001u,
    0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu,
    0x00000000u, 0x00000001u, 0x000b000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u,
    0x00000009u, 0x0000000bu, 0x0000000fu, 0x00000011u, 0x00000018u, 0x0000001bu, 0x00030003u,
    0x00000002u, 0x000001c2u, 0x000a0004u, 0x475f4c47u, 0x4c474f4fu, 0x70635f45u, 0x74735f70u,
    0x5f656c79u, 0x656e696cu, 0x7269645fu, 0x69746365u, 0x00006576u, 0x00080004u, 0x475f4c47u,
    0x4c474f4fu, 0x6e695f45u, 0x64756c63u, 0x69645f65u, 0x74636572u, 0x00657669u, 0x00040005u,
    0x00000004u, 0x6e69616du, 0x00000000u, 0x00040005u, 0x00000009u, 0x6c6f4376u, 0x0000726fu,
    0x00040005u, 0x0000000bu, 0x6c6f4361u, 0x0000726fu, 0x00030005u, 0x0000000fu, 0x00565576u,
    0x00030005u, 0x00000011u, 0x00565561u, 0x00060005u, 0x00000016u, 0x505f6c67u, 0x65567265u,
    0x78657472u, 0x00000000u, 0x00060006u, 0x00000016u, 0x00000000u, 0x505f6c67u, 0x7469736fu,
    0x006e6f69u, 0x00070006u, 0x00000016u, 0x00000001u, 0x505f6c67u, 0x746e696fu, 0x657a6953u,
    0x00000000u, 0x00070006u, 0x00000016u, 0x00000002u, 0x435f6c67u, 0x4470696cu, 0x61747369u,
    0x0065636eu, 0x00070006u, 0x00000016u, 0x00000003u, 0x435f6c67u, 0x446c6c75u, 0x61747369u,
    0x0065636eu, 0x00030005u, 0x00000018u, 0x00000000u, 0x00040005u, 0x0000001bu, 0x736f5061u,
    0x00000000u, 0x00060005u, 0x0000001du, 0x73755075u, 0x6e6f4368u, 0x6e617473u, 0x00000074u,
    0x00050006u, 0x0000001du, 0x00000000u, 0x61635375u, 0x0000656cu, 0x00060006u, 0x0000001du,
    0x00000001u, 0x61725475u, 0x616c736eu, 0x00006574u, 0x00030005u, 0x0000001fu, 0x00006370u,
    0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x0000000bu, 0x0000001eu,
    0x00000002u, 0x00040047u, 0x0000000fu, 0x0000001eu, 0x00000001u, 0x00040047u, 0x00000011u,
    0x0000001eu, 0x00000001u, 0x00030047u, 0x00000016u, 0x00000002u, 0x00050048u, 0x00000016u,
    0x00000000u, 0x0000000bu, 0x00000000u, 0x00050048u, 0x00000016u, 0x00000001u, 0x0000000bu,
    0x00000001u, 0x00050048u, 0x00000016u, 0x00000002u, 0x0000000bu, 0x00000003u, 0x00050048u,
    0x00000016u, 0x00000003u, 0x0000000bu, 0x00000004u, 0x00040047u, 0x0000001bu, 0x0000001eu,
    0x00000000u, 0x00030047u, 0x0000001du, 0x00000002u, 0x00050048u, 0x0000001du, 0x00000000u,
    0x00000023u, 0x00000000u, 0x00050048u, 0x0000001du, 0x00000001u, 0x00000023u, 0x00000008u,
    0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u,
    0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u,
    0x00000003u, 0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u, 0x00000003u, 0x00040020u,
    0x0000000au, 0x00000001u, 0x00000007u, 0x0004003bu, 0x0000000au, 0x0000000bu, 0x00000001u,
    0x00040017u, 0x0000000du, 0x00000006u, 0x00000002u, 0x00040020u, 0x0000000eu, 0x00000003u,
    0x0000000du, 0x0004003bu, 0x0000000eu, 0x0000000fu, 0x00000003u, 0x00040020u, 0x00000010u,
    0x00000001u, 0x0000000du, 0x0004003bu, 0x00000010u, 0x00000011u, 0x00000001u, 0x00040015u,
    0x00000013u, 0x00000020u, 0x00000000u, 0x0004002bu, 0x00000013u, 0x00000014u, 0x00000001u,
    0x0004001cu, 0x00000015u, 0x00000006u, 0x00000014u, 0x0006001eu, 0x00000016u, 0x00000007u,
    0x00000006u, 0x00000015u, 0x00000015u, 0x00040020u, 0x00000017u, 0x00000003u, 0x00000016u,
    0x0004003bu, 0x00000017u, 0x00000018u, 0x00000003u, 0x00040015u, 0x00000019u, 0x00000020u,
    0x00000001u, 0x0004002bu, 0x00000019u, 0x0000001au, 0x00000000u, 0x0004003bu, 0x00000010u,
    0x0000001bu, 0x00000001u, 0x0004001eu, 0x0000001du, 0x0000000du, 0x0000000du, 0x00040020u,
    0x0000001eu, 0x00000009u, 0x0000001du, 0x0004003bu, 0x0000001eu, 0x0000001fu, 0x00000009u,
    0x00040020u, 0x00000020u, 0x00000009u, 0x0000000du, 0x0004002bu, 0x00000019u, 0x00000024u,
    0x00000001u, 0x0004002bu, 0x00000006u, 0x00000028u, 0x00000000u, 0x0004002bu, 0x00000006u,
    0x00000029u, 0x3f800000u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
    0x000200f8u, 0x00000005u, 0x0004003du, 0x00000007u, 0x0000000cu, 0x0000000bu, 0x0003003eu,
    0x00000009u, 0x0000000cu, 0x0004003du, 0x0000000du, 0x00000012u, 0x00000011u, 0x0003003eu,
    0x0000000fu, 0x00000012u, 0x0004003du, 0x0000000du, 0x0000001cu, 0x0000001bu, 0x00050041u,
    0x00000020u, 0x00000021u, 0x0000001fu, 0x0000001au, 0x0004003du, 0x0000000du, 0x00000022u,
    0x00000021u, 0x00050085u, 0x0000000du, 0x00000023u, 0x0000001cu, 0x00000022u, 0x00050041u,
    0x00000020u, 0x00000025u, 0x0000001fu, 0x00000024u, 0x0004003du, 0x0000000du, 0x00000026u,
    0x00000025u, 0x00050081u, 0x0000000du, 0x00000027u, 0x00000023u, 0x00000026u, 0x00050051u,
    0x00000006u, 0x0000002au, 0x00000027u, 0x00000000u, 0x00050051u, 0x00000006u, 0x0000002bu,
    0x00000027u, 0x00000001u, 0x00070050u, 0x00000007u, 0x0000002cu, 0x0000002au, 0x0000002bu,
    0x00000028u, 0x00000029u, 0x00050041u, 0x00000008u, 0x0000002du, 0x00000018u, 0x0000001au,
    0x0003003eu, 0x0000002du, 0x0000002cu, 0x000100fdu, 0x00010038u,
};

static constexpr uint32_t kImGuiFragSpirv[] = {
    0x07230203u, 0x00010000u, 0x000d000bu, 0x00000018u, 0x00000000u, 0x00020011u, 0x00000001u,
    0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu,
    0x00000000u, 0x00000001u, 0x0008000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u,
    0x00000009u, 0x0000000bu, 0x00000014u, 0x00030010u, 0x00000004u, 0x00000007u, 0x00030003u,
    0x00000002u, 0x000001c2u, 0x000a0004u, 0x475f4c47u, 0x4c474f4fu, 0x70635f45u, 0x74735f70u,
    0x5f656c79u, 0x656e696cu, 0x7269645fu, 0x69746365u, 0x00006576u, 0x00080004u, 0x475f4c47u,
    0x4c474f4fu, 0x6e695f45u, 0x64756c63u, 0x69645f65u, 0x74636572u, 0x00657669u, 0x00040005u,
    0x00000004u, 0x6e69616du, 0x00000000u, 0x00040005u, 0x00000009u, 0x6c6f4366u, 0x0000726fu,
    0x00040005u, 0x0000000bu, 0x6c6f4376u, 0x0000726fu, 0x00050005u, 0x00000010u, 0x78655473u,
    0x65727574u, 0x00000000u, 0x00030005u, 0x00000014u, 0x00565576u, 0x00040047u, 0x00000009u,
    0x0000001eu, 0x00000000u, 0x00040047u, 0x0000000bu, 0x0000001eu, 0x00000000u, 0x00040047u,
    0x00000010u, 0x00000021u, 0x00000000u, 0x00040047u, 0x00000010u, 0x00000022u, 0x00000000u,
    0x00040047u, 0x00000014u, 0x0000001eu, 0x00000001u, 0x00020013u, 0x00000002u, 0x00030021u,
    0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u,
    0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u, 0x00000003u, 0x00000007u, 0x0004003bu,
    0x00000008u, 0x00000009u, 0x00000003u, 0x00040020u, 0x0000000au, 0x00000001u, 0x00000007u,
    0x0004003bu, 0x0000000au, 0x0000000bu, 0x00000001u, 0x00090019u, 0x0000000du, 0x00000006u,
    0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000001u, 0x00000000u, 0x0003001bu,
    0x0000000eu, 0x0000000du, 0x00040020u, 0x0000000fu, 0x00000000u, 0x0000000eu, 0x0004003bu,
    0x0000000fu, 0x00000010u, 0x00000000u, 0x00040017u, 0x00000012u, 0x00000006u, 0x00000002u,
    0x00040020u, 0x00000013u, 0x00000001u, 0x00000012u, 0x0004003bu, 0x00000013u, 0x00000014u,
    0x00000001u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u,
    0x00000005u, 0x0004003du, 0x00000007u, 0x0000000cu, 0x0000000bu, 0x0004003du, 0x0000000eu,
    0x00000011u, 0x00000010u, 0x0004003du, 0x00000012u, 0x00000015u, 0x00000014u, 0x00050057u,
    0x00000007u, 0x00000016u, 0x00000011u, 0x00000015u, 0x00050085u, 0x00000007u, 0x00000017u,
    0x0000000cu, 0x00000016u, 0x0003003eu, 0x00000009u, 0x00000017u, 0x000100fdu, 0x00010038u,
};

struct ImGuiPushConstants {
    float scale[2];
    float translate[2];
};

} // namespace

ImGuiRhiBackend::~ImGuiRhiBackend() {
    shutdown();
}

bool ImGuiRhiBackend::initialize(rhi::RhiDevice& device) {
    if (initialized_) {
        return true;
    }

    device_ = &device;

    // 1. Descriptor set layout: binding 0 combined image sampler (fragment)
    rhi::DescriptorSetLayoutDesc setLayoutDesc{
        .bindings = {{
            .binding = 0,
            .descriptorType = rhi::DescriptorType::CombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = rhi::ShaderStageFlags::Fragment,
        }},
        .debugName = "ImGuiDescriptorSetLayout",
    };
    auto layoutRes = device_->createDescriptorSetLayout(setLayoutDesc);
    if (!layoutRes.hasValue()) {
        // Headless NullDevice does not implement descriptor layouts; keep backend inactive cleanly.
        ENG_LOG_INFO("Editor", "ImGuiRhiBackend running in headless mode (no descriptor layout)");
        initialized_ = true;
        return true;
    }
    descriptorSetLayout_ = layoutRes.value();

    // 2. Descriptor pool for font atlas + editor textures/viewports
    rhi::DescriptorPoolDesc poolDesc{
        .maxSets = 64,
        .poolSizes = {{
            .type = rhi::DescriptorType::CombinedImageSampler,
            .count = 64,
        }},
        .debugName = "ImGuiDescriptorPool",
    };
    auto poolRes = device_->createDescriptorPool(poolDesc);
    if (!poolRes.hasValue()) {
        shutdown();
        return false;
    }
    descriptorPool_ = poolRes.value();

    // 3. Pipeline layout with 16-byte vertex push constants (scale + translate)
    rhi::PipelineLayoutDesc pipeLayoutDesc{
        .descriptorSetLayoutHandles = {descriptorSetLayout_},
        .debugName = "ImGuiPipelineLayout",
    };
    pipeLayoutDesc.pushConstants = {{
        .stageFlags = rhi::ShaderStageFlags::Vertex,
        .offset = 0,
        .size = sizeof(ImGuiPushConstants),
    }};
    auto pipeLayoutRes = device_->createPipelineLayout(pipeLayoutDesc);
    if (!pipeLayoutRes.hasValue()) {
        shutdown();
        return false;
    }
    pipelineLayout_ = pipeLayoutRes.value();

    // 4. Shaders
    auto vsRes = device_->createShader(rhi::ShaderDesc{
        .stage = rhi::ShaderStageFlags::Vertex,
        .spirvCode = kImGuiVertSpirv,
        .entryPoint = "main",
        .debugName = "ImGuiVertShader",
    });
    auto fsRes = device_->createShader(rhi::ShaderDesc{
        .stage = rhi::ShaderStageFlags::Fragment,
        .spirvCode = kImGuiFragSpirv,
        .entryPoint = "main",
        .debugName = "ImGuiFragShader",
    });
    if (!vsRes.hasValue() || !fsRes.hasValue()) {
        if (vsRes.hasValue()) {
            device_->destroyShader(vsRes.value());
        }
        if (fsRes.hasValue()) {
            device_->destroyShader(fsRes.value());
        }
        shutdown();
        return false;
    }
    vertexShader_ = vsRes.value();
    fragmentShader_ = fsRes.value();

    // 5. Graphics pipeline matching ImDrawVert & active swapchain format
    rhi::GraphicsPipelineDesc pipeDesc{
        .vertexShader = vertexShader_,
        .fragmentShader = fragmentShader_,
        .vertexInput =
            {
                .bindings = {{
                    .binding = 0,
                    .stride = sizeof(ImDrawVert),
                    .inputRate = rhi::VertexInputRate::Vertex,
                }},
                .attributes =
                    {
                        {
                            .location = 0,
                            .binding = 0,
                            .format = rhi::VertexFormat::Float2,
                            .offset = static_cast<uint32_t>(offsetof(ImDrawVert, pos)),
                        },
                        {
                            .location = 1,
                            .binding = 0,
                            .format = rhi::VertexFormat::Float2,
                            .offset = static_cast<uint32_t>(offsetof(ImDrawVert, uv)),
                        },
                        {
                            .location = 2,
                            .binding = 0,
                            .format = rhi::VertexFormat::UByte4Norm,
                            .offset = static_cast<uint32_t>(offsetof(ImDrawVert, col)),
                        },
                    },
            },
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
            .blendEnable = true,
            .srcColorBlendFactor = rhi::BlendFactor::SrcAlpha,
            .dstColorBlendFactor = rhi::BlendFactor::OneMinusSrcAlpha,
            .colorBlendOp = rhi::BlendOp::Add,
            .srcAlphaBlendFactor = rhi::BlendFactor::One,
            .dstAlphaBlendFactor = rhi::BlendFactor::OneMinusSrcAlpha,
            .alphaBlendOp = rhi::BlendOp::Add,
        }},
        .layout = pipelineLayout_,
        .colorAttachmentFormats = {device_->getSwapchainFormat()},
        .depthAttachmentFormat = rhi::TextureFormat::Undefined,
        .debugName = "ImGuiPipeline",
    };
    auto pipeRes = device_->createGraphicsPipeline(pipeDesc);
    if (!pipeRes.hasValue()) {
        shutdown();
        return false;
    }
    pipeline_ = pipeRes.value();

    // 6. Upload ImGui font atlas texture via RHI
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* fontPixels = nullptr;
    int fontWidth = 0;
    int fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);

    if (fontPixels != nullptr && fontWidth > 0 && fontHeight > 0) {
        render::ImageData fontImg{};
        fontImg.width = static_cast<uint32_t>(fontWidth);
        fontImg.height = static_cast<uint32_t>(fontHeight);
        fontImg.channels = 4;
        fontImg.format = rhi::TextureFormat::RGBA8_UNORM;
        const size_t byteCount =
            static_cast<size_t>(fontWidth) * static_cast<size_t>(fontHeight) * 4;
        fontImg.pixels.assign(fontPixels, fontPixels + byteCount);

        auto texRes = render::GpuTexture::create(*device_, fontImg, false);
        if (texRes.hasValue()) {
            fontTexture_ = std::move(texRes.value());
            fontTextureId_ =
                registerTexture(fontTexture_->getTexture(), fontTexture_->getSampler());
            io.Fonts->SetTexID(static_cast<ImTextureID>(fontTextureId_));
        }
    }

    initialized_ = true;
    ENG_LOG_INFO("Editor", "Dear ImGui Pure-RHI backend initialized");
    return true;
}

void ImGuiRhiBackend::shutdown() {
    if (device_ == nullptr) {
        initialized_ = false;
        return;
    }

    device_->waitIdle();

    for (auto& fb : frameBuffers_) {
        if (fb.vertexBuffer.isValid()) {
            device_->destroyBuffer(fb.vertexBuffer);
            fb.vertexBuffer = {};
            fb.vertexCapacityBytes = 0;
        }
        if (fb.indexBuffer.isValid()) {
            device_->destroyBuffer(fb.indexBuffer);
            fb.indexBuffer = {};
            fb.indexCapacityBytes = 0;
        }
    }

    fontTextureId_ = 0;
    fontTexture_.reset();
    allocatedSets_.clear();
    freeSets_.clear();

    if (pipeline_.isValid()) {
        device_->destroyGraphicsPipeline(pipeline_);
        pipeline_ = {};
    }
    if (vertexShader_.isValid()) {
        device_->destroyShader(vertexShader_);
        vertexShader_ = {};
    }
    if (fragmentShader_.isValid()) {
        device_->destroyShader(fragmentShader_);
        fragmentShader_ = {};
    }
    if (pipelineLayout_.isValid()) {
        device_->destroyPipelineLayout(pipelineLayout_);
        pipelineLayout_ = {};
    }
    if (descriptorPool_.isValid()) {
        device_->destroyDescriptorPool(descriptorPool_);
        descriptorPool_ = {};
    }
    if (descriptorSetLayout_.isValid()) {
        device_->destroyDescriptorSetLayout(descriptorSetLayout_);
        descriptorSetLayout_ = {};
    }

    device_ = nullptr;
    initialized_ = false;
}

uint64_t ImGuiRhiBackend::registerTexture(rhi::TextureHandle texture, rhi::SamplerHandle sampler) {
    if (device_ == nullptr || !descriptorPool_.isValid() || !descriptorSetLayout_.isValid() ||
        !texture.isValid() || !sampler.isValid()) {
        return 0;
    }

    rhi::DescriptorSetHandle set{};
    if (!freeSets_.empty()) {
        set = freeSets_.back();
        freeSets_.pop_back();
    } else {
        auto allocRes = device_->allocateDescriptorSet(descriptorPool_, descriptorSetLayout_);
        if (!allocRes.hasValue()) {
            return 0;
        }
        set = allocRes.value();
        allocatedSets_.push_back(set);
    }

    const std::array<rhi::DescriptorWrite, 1> writes{
        rhi::DescriptorWrite::makeCombinedImageSampler(0, texture, sampler),
    };
    (void)device_->updateDescriptorSet(set, writes);
    return (static_cast<uint64_t>(set.generation()) << 32) | static_cast<uint64_t>(set.index());
}

void ImGuiRhiBackend::unregisterTexture(uint64_t textureId) {
    if (textureId == 0) {
        return;
    }
    const rhi::DescriptorSetHandle handle{GenerationalHandle{
        static_cast<uint32_t>(textureId & 0xFFFFFFFFULL),
        static_cast<uint32_t>(textureId >> 32),
    }};
    if (handle.isValid()) {
        freeSets_.push_back(handle);
    }
}

void ImGuiRhiBackend::renderDrawData(ImDrawData* drawData, rhi::RhiCommandList& cmdList) {
    if (!initialized_ || device_ == nullptr || !pipeline_.isValid() || drawData == nullptr ||
        drawData->TotalVtxCount <= 0 || drawData->TotalIdxCount <= 0) {
        return;
    }

    const int fbWidth = static_cast<int>(drawData->DisplaySize.x * drawData->FramebufferScale.x);
    const int fbHeight = static_cast<int>(drawData->DisplaySize.y * drawData->FramebufferScale.y);
    if (fbWidth <= 0 || fbHeight <= 0) {
        return;
    }

    currentFrameSlot_ = (currentFrameSlot_ + 1U) % kFrameSlots;
    FrameBuffers& fb = frameBuffers_[currentFrameSlot_];

    const size_t requiredVtxBytes =
        static_cast<size_t>(drawData->TotalVtxCount) * sizeof(ImDrawVert);
    const size_t requiredIdxBytes =
        static_cast<size_t>(drawData->TotalIdxCount) * sizeof(ImDrawIdx);

    if (!fb.vertexBuffer.isValid() || fb.vertexCapacityBytes < requiredVtxBytes) {
        if (fb.vertexBuffer.isValid()) {
            device_->destroyBuffer(fb.vertexBuffer);
        }
        const size_t allocBytes = std::max<size_t>(requiredVtxBytes, 64ULL * 1024ULL);
        auto vboRes = device_->createBuffer(rhi::BufferDesc{
            .sizeInBytes = allocBytes,
            .usage = rhi::BufferUsageFlags::Vertex,
            .memoryUsage = rhi::MemoryUsage::CpuToGpu,
            .debugName = "ImGuiVertexBuffer",
        });
        if (!vboRes.hasValue()) {
            return;
        }
        fb.vertexBuffer = vboRes.value();
        fb.vertexCapacityBytes = allocBytes;
    }

    if (!fb.indexBuffer.isValid() || fb.indexCapacityBytes < requiredIdxBytes) {
        if (fb.indexBuffer.isValid()) {
            device_->destroyBuffer(fb.indexBuffer);
        }
        const size_t allocBytes = std::max<size_t>(requiredIdxBytes, 32ULL * 1024ULL);
        auto iboRes = device_->createBuffer(rhi::BufferDesc{
            .sizeInBytes = allocBytes,
            .usage = rhi::BufferUsageFlags::Index,
            .memoryUsage = rhi::MemoryUsage::CpuToGpu,
            .debugName = "ImGuiIndexBuffer",
        });
        if (!iboRes.hasValue()) {
            return;
        }
        fb.indexBuffer = iboRes.value();
        fb.indexCapacityBytes = allocBytes;
    }

    // Upload vertices and indices sequentially into the slot's dynamic buffers
    size_t vtxOffsetBytes = 0;
    size_t idxOffsetBytes = 0;
    for (int n = 0; n < drawData->CmdListsCount; ++n) {
        const ImDrawList* cmdListData = drawData->CmdLists[n];
        const size_t vtxBytes =
            static_cast<size_t>(cmdListData->VtxBuffer.Size) * sizeof(ImDrawVert);
        const size_t idxBytes =
            static_cast<size_t>(cmdListData->IdxBuffer.Size) * sizeof(ImDrawIdx);
        if (vtxBytes > 0) {
            (void)device_->writeBuffer(fb.vertexBuffer, vtxOffsetBytes, cmdListData->VtxBuffer.Data,
                                       vtxBytes);
            vtxOffsetBytes += vtxBytes;
        }
        if (idxBytes > 0) {
            (void)device_->writeBuffer(fb.indexBuffer, idxOffsetBytes, cmdListData->IdxBuffer.Data,
                                       idxBytes);
            idxOffsetBytes += idxBytes;
        }
    }

    cmdList.bindPipeline(pipeline_);
    cmdList.setViewport(0.0F, 0.0F, static_cast<float>(fbWidth), static_cast<float>(fbHeight), 0.0F,
                        1.0F);

    ImGuiPushConstants pc{};
    pc.scale[0] = 2.0F / drawData->DisplaySize.x;
    pc.scale[1] = 2.0F / drawData->DisplaySize.y;
    pc.translate[0] = -1.0F - drawData->DisplayPos.x * pc.scale[0];
    pc.translate[1] = -1.0F - drawData->DisplayPos.y * pc.scale[1];
    cmdList.pushConstants(pipelineLayout_, rhi::ShaderStageFlags::Vertex, 0, sizeof(pc), &pc);

    cmdList.bindVertexBuffer(0, fb.vertexBuffer, 0);
    cmdList.bindIndexBuffer(fb.indexBuffer, 0, sizeof(ImDrawIdx) == 4);

    const ImVec2 clipOff = drawData->DisplayPos;
    const ImVec2 clipScale = drawData->FramebufferScale;

    uint32_t globalVtxOffset = 0;
    uint32_t globalIdxOffset = 0;
    for (int n = 0; n < drawData->CmdListsCount; ++n) {
        const ImDrawList* cmdListData = drawData->CmdLists[n];
        for (int cmdIdx = 0; cmdIdx < cmdListData->CmdBuffer.Size; ++cmdIdx) {
            const ImDrawCmd* pcmd = &cmdListData->CmdBuffer[cmdIdx];
            if (pcmd->UserCallback != nullptr) {
                pcmd->UserCallback(cmdListData, pcmd);
                continue;
            }

            ImVec2 clipMin((pcmd->ClipRect.x - clipOff.x) * clipScale.x,
                           (pcmd->ClipRect.y - clipOff.y) * clipScale.y);
            ImVec2 clipMax((pcmd->ClipRect.z - clipOff.x) * clipScale.x,
                           (pcmd->ClipRect.w - clipOff.y) * clipScale.y);
            clipMin.x = std::max(0.0F, clipMin.x);
            clipMin.y = std::max(0.0F, clipMin.y);
            clipMax.x = std::min(static_cast<float>(fbWidth), clipMax.x);
            clipMax.y = std::min(static_cast<float>(fbHeight), clipMax.y);
            if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) {
                continue;
            }

            cmdList.setScissor(static_cast<int32_t>(clipMin.x), static_cast<int32_t>(clipMin.y),
                               static_cast<uint32_t>(clipMax.x - clipMin.x),
                               static_cast<uint32_t>(clipMax.y - clipMin.y));

            uint64_t texId = static_cast<uint64_t>(pcmd->GetTexID());
            if (texId == 0) {
                texId = fontTextureId_;
            }
            if (texId != 0) {
                const rhi::DescriptorSetHandle setHandle{GenerationalHandle{
                    static_cast<uint32_t>(texId & 0xFFFFFFFFULL),
                    static_cast<uint32_t>(texId >> 32),
                }};
                const std::array<rhi::DescriptorSetHandle, 1> sets{setHandle};
                cmdList.bindDescriptorSets(pipelineLayout_, 0, sets);
                cmdList.drawIndexed(pcmd->ElemCount, 1, pcmd->IdxOffset + globalIdxOffset,
                                    static_cast<int32_t>(pcmd->VtxOffset + globalVtxOffset), 0);
            }
        }
        globalIdxOffset += static_cast<uint32_t>(cmdListData->IdxBuffer.Size);
        globalVtxOffset += static_cast<uint32_t>(cmdListData->VtxBuffer.Size);
    }
}

} // namespace eng::editor
