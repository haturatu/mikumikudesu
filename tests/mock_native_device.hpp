#pragma once
#include "graphics/device.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>
namespace dayo::test {
struct MockNativeDevice final : dayo::graphics::Device {
    struct Buffer {
        std::vector<std::byte> bytes;
    };

    const dayo::graphics::DeviceCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }
    const dayo::graphics::GraphicsConvention& convention() const noexcept override {
        return convention_;
    }
    dayo::graphics::RendererKind activeRenderer() const noexcept override {
        return active;
    }
    void selectRenderer(dayo::graphics::RendererKind renderer) override {
        active = renderer;
    }
    void resize() override {}
    void beginUiFrame() override {}
    void renderFrame() override {}
    std::size_t currentFrameSlot() const noexcept override {
        return frameSlot;
    }
    void waitIdle() override {
        ++idleWaits;
    }
    void prepareResourceDestruction() override {
        ++retirementBarriers;
    }
    void setPreviewRenderSettings(const dayo::graphics::PreviewRenderSettings& settings) override {
        previewSettings = settings;
    }
    dayo::graphics::PreviewRenderSettings previewRenderSettings() const noexcept override {
        return previewSettings;
    }
    std::size_t idleWaits{}, retirementBarriers{};
    dayo::graphics::PreviewRenderSettings previewSettings;
    void uploadPreviewMesh(std::span<const dayo::graphics::PreviewVertex> vertices,
                           std::span<const std::uint32_t>) override {
        previewVertices.assign(vertices.begin(), vertices.end());
    }
    void updatePreviewVertices(std::span<const dayo::graphics::PreviewVertex> vertices) override {
        previewVertices.assign(vertices.begin(), vertices.end());
    }
    void updatePreviewBones(std::span<const dayo::graphics::PreviewBoneTransform>) override {}
    void uploadPreviewMorphDeltas(std::span<const dayo::graphics::PreviewMorphDelta>) override {}
    void updatePreviewMorphWeights(std::span<const float>) override {}
    void updatePreviewMaterials(std::span<const dayo::graphics::PreviewMaterial>) override {}
    void updatePreviewDraws(std::span<const dayo::graphics::PreviewDraw>) override {}
    void uploadPreviewTextures(std::span<const dayo::graphics::PreviewTexture>) override {}
    void uploadPreviewBackground(std::span<const dayo::graphics::PreviewTexture>) override {}
    void clearPreviewResources() override {}
    void updatePreviewScene(const dayo::graphics::PreviewScene&) override {}
    dayo::graphics::BufferHandle createBuffer(const dayo::graphics::BufferDesc&) override {
        return nextHandle_++;
    }
    dayo::graphics::TextureHandle createTexture(const dayo::graphics::TextureDesc&) override {
        return nextHandle_++;
    }
    dayo::graphics::handles::DescriptorSetLayoutHandle
    createDescriptorSetLayoutEx(const dayo::graphics::DescriptorSetLayoutDesc& desc) override {
        lastDescriptorLayout = desc;
        return {nextDescriptorLayout_++, 1};
    }
    dayo::graphics::handles::TextureHandle createTextureEx(const dayo::graphics::TextureResourceDesc& desc) override {
        textureDescriptions.push_back(desc);
        return {nextTypedTexture_++, 1};
    }
    dayo::graphics::handles::BufferHandle createBufferEx(const dayo::graphics::BufferResourceDesc& desc) override {
        const auto handle = dayo::graphics::handles::BufferHandle{nextTypedBuffer_++, 1};
        typedBuffers_.emplace(handle, Buffer{std::vector<std::byte>(desc.size)});
        typedBufferUsages.emplace_back(handle, desc.usage);
        return handle;
    }
    void destroyTextureEx(dayo::graphics::handles::TextureHandle handle) override {
        if (handle.valid())
            ++destroyedTextures;
    }
    void clearTextureEx(dayo::graphics::handles::TextureHandle, const std::array<float, 4>&) override {
        ++clearedTextures;
    }
    void destroyBufferEx(dayo::graphics::handles::BufferHandle handle) override {
        ++destroyedBuffers;
        typedBuffers_.erase(handle);
    }
    void copyBufferEx(dayo::graphics::handles::BufferHandle source,
                      dayo::graphics::handles::BufferHandle destination) override {
        const auto sourceIt = typedBuffers_.find(source);
        const auto destinationIt = typedBuffers_.find(destination);
        if (sourceIt == typedBuffers_.end() || destinationIt == typedBuffers_.end() ||
            sourceIt->second.bytes.size() > destinationIt->second.bytes.size())
            throw std::out_of_range("mock typed buffer copy exceeds allocation");
        std::copy(sourceIt->second.bytes.begin(), sourceIt->second.bytes.end(), destinationIt->second.bytes.begin());
    }
    dayo::graphics::handles::DescriptorSetHandle
    allocateDescriptorSetEx(dayo::graphics::handles::DescriptorSetLayoutHandle layout,
                            std::span<const dayo::graphics::DescriptorBindingEx> bindings) override {
        if (!layout.valid())
            throw std::invalid_argument("mock descriptor layout is invalid");
        lastDescriptorBindings.assign(bindings.begin(), bindings.end());
        descriptorAllocations.emplace_back(bindings.begin(), bindings.end());
        const auto set = dayo::graphics::handles::DescriptorSetHandle{nextDescriptorSet_++, 1};
        descriptorBindings_[set].assign(bindings.begin(), bindings.end());
        return set;
    }
    void updateDescriptorSetEx(dayo::graphics::handles::DescriptorSetHandle set,
                               std::span<const dayo::graphics::DescriptorBindingEx> bindings) override {
        lastDescriptorBindings.assign(bindings.begin(), bindings.end());
        descriptorBindings_[set].assign(bindings.begin(), bindings.end());
    }
    void destroyDescriptorSetEx(dayo::graphics::handles::DescriptorSetHandle handle) override {
        if (handle.valid()) {
            ++destroyedDescriptorSets;
            descriptorBindings_.erase(handle);
        }
    }
    void destroyDescriptorSetLayoutEx(dayo::graphics::handles::DescriptorSetLayoutHandle handle) override {
        if (handle.valid())
            ++destroyedDescriptorLayouts;
    }
    dayo::graphics::handles::PipelineLayoutHandle
    createPipelineLayoutEx(const dayo::graphics::PipelineLayoutDesc&) override {
        return {nextPipelineLayout_++, 1};
    }
    dayo::graphics::handles::SamplerHandle createSamplerEx() override {
        return {nextSampler_++, 1};
    }
    dayo::graphics::handles::SamplerHandle createSamplerEx(const dayo::graphics::SamplerResourceDesc& desc) override {
        samplerDescriptions.push_back(desc);
        return createSamplerEx();
    }
    dayo::graphics::handles::PipelineHandle
    createGraphicsPipelineEx(const dayo::graphics::GraphicsPipelineDescEx&) override {
        return {nextPipeline_++, 1};
    }
    void destroyPipelineLayoutEx(dayo::graphics::handles::PipelineLayoutHandle) override {}
    dayo::graphics::handles::ShaderHandle createShaderEx(const dayo::graphics::ShaderDesc&) override {
        return {nextShader_++, 1};
    }
    void destroyShaderEx(dayo::graphics::handles::ShaderHandle) override {}
    dayo::graphics::handles::PipelineHandle
    createComputePipelineEx(const dayo::graphics::ComputePipelineDescEx&) override {
        return {nextPipeline_++, 1};
    }
    void destroyPipelineEx(dayo::graphics::handles::PipelineHandle) override {}
    dayo::graphics::handles::PipelineHandle
    createRayTracingPipelineEx(const dayo::graphics::RayTracingPipelineDescEx&) override {
        return {nextPipeline_++, 1};
    }
    dayo::graphics::handles::ShaderBindingTableHandle
    createShaderBindingTable(const dayo::graphics::ShaderBindingTableDesc&) override {
        return {nextSbt_++, 1};
    }
    void destroyShaderBindingTable(dayo::graphics::handles::ShaderBindingTableHandle) override {}
    void uploadTextureEx(dayo::graphics::handles::TextureHandle, std::span<const std::uint8_t> bytes, std::uint32_t,
                         std::uint32_t) override {
        lastTextureUpload.assign(bytes.begin(), bytes.end());
    }
    void destroySamplerEx(dayo::graphics::handles::SamplerHandle) override {
        ++destroyedSamplers;
    }
    void uploadBufferEx(dayo::graphics::handles::BufferHandle handle, std::span<const std::byte> bytes,
                        std::size_t offset) override {
        ++uploadBufferCalls;
        auto it = typedBuffers_.find(handle);
        if (it == typedBuffers_.end() || offset > it->second.bytes.size() ||
            bytes.size() > it->second.bytes.size() - offset)
            throw std::out_of_range("mock typed buffer upload exceeds allocation");
        std::copy(bytes.begin(), bytes.end(), it->second.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    std::vector<std::byte> readbackBufferEx(dayo::graphics::handles::BufferHandle handle, std::size_t offset,
                                            std::size_t size) override {
        const auto it = typedBuffers_.find(handle);
        if (it == typedBuffers_.end() || offset > it->second.bytes.size() || size > it->second.bytes.size() - offset)
            throw std::out_of_range("mock typed buffer readback exceeds allocation");
        return {it->second.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                it->second.bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)};
    }

    dayo::graphics::DeviceCapabilities capabilities_{
        .gpuName = "mock",
        .driverName = "mock",
        .swapchain = true,
        .bufferDeviceAddress = true,
        .descriptorIndexing = true,
        .accelerationStructure = true,
        .rayTracingPipeline = true,
        .rayQuery = true,
        .fragmentShaderBarycentric = true,
    };
    dayo::graphics::GraphicsConvention convention_;
    dayo::graphics::BufferHandle nextHandle_{1};
    std::uint32_t nextTypedBuffer_{1};
    std::uint32_t nextTypedTexture_{1};
    std::uint32_t nextDescriptorLayout_{1};
    std::uint32_t nextDescriptorSet_{1};
    std::uint32_t nextPipelineLayout_{1};
    std::uint32_t nextSampler_{1};
    std::uint32_t nextShader_{1};
    std::uint32_t nextPipeline_{1};
    std::uint32_t nextSbt_{1};
    std::size_t destroyedBuffers{};
    std::size_t destroyedTextures{};
    std::size_t clearedTextures{};
    std::size_t destroyedDescriptorSets{};
    std::size_t destroyedDescriptorLayouts{};
    std::vector<dayo::graphics::PreviewVertex> previewVertices;
    std::size_t uploadBufferCalls{};
    std::size_t frameSlot{};
    dayo::graphics::RendererKind active{dayo::graphics::RendererKind::subayai};
    dayo::graphics::DescriptorSetLayoutDesc lastDescriptorLayout;
    std::vector<dayo::graphics::DescriptorBindingEx> lastDescriptorBindings;
    std::vector<std::vector<dayo::graphics::DescriptorBindingEx>> descriptorAllocations;
    std::vector<std::pair<dayo::graphics::handles::BufferHandle, dayo::graphics::ResourceUsage>> typedBufferUsages;
    std::vector<dayo::graphics::TextureResourceDesc> textureDescriptions;
    std::vector<dayo::graphics::SamplerResourceDesc> samplerDescriptions;
    std::vector<std::uint8_t> lastTextureUpload;
    std::size_t destroyedSamplers{};
    std::unordered_map<dayo::graphics::handles::DescriptorSetHandle, std::vector<dayo::graphics::DescriptorBindingEx>>
        descriptorBindings_;
    std::unordered_map<dayo::graphics::handles::BufferHandle, Buffer> typedBuffers_;
};

} // namespace dayo::test
