#pragma once

#include "graphics/device.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace dayo::graphics {

class VulkanDevice;

// Narrow command-list adapter for native FX/RT recording. Preview continues
// to record directly in VulkanDevice; this class makes the SBT-bearing
// traceRays contract available to native runtimes without exposing Vulkan
// handles through the backend-neutral Device interface.
class VulkanCommandList final : public CommandList {
  public:
    VulkanCommandList(VulkanDevice& device, VkCommandBuffer commandBuffer);

    void transition(TextureHandle) override;
    void bindPipeline(PipelineHandle) override;
    void draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1) override;
    void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) override;
    void traceRays(std::uint32_t width, std::uint32_t height) override;
    void traceRays(handles::ShaderBindingTableHandle sbt, std::uint32_t width, std::uint32_t height,
                   std::uint32_t depth = 1) override;
    void bindResources(std::span<const DescriptorBinding>) override;
    void pushConstants(std::span<const std::byte>) override;
    void copyTexture(TextureHandle, TextureHandle) override;
    void clearTexture(TextureHandle) override;
    void generateMipmaps(TextureHandle) override;
    void buildAccelerationStructure(AccelerationStructureHandle) override;
    void bindPipelineEx(handles::PipelineHandle pipeline) override;
    void bindDescriptorSetEx(handles::DescriptorSetHandle set, std::uint32_t setIndex = 0) override;
    void pushConstantsEx(std::span<const std::byte> bytes) override;
    void memoryBarrierEx() override;
    void accelerationStructureBarrierEx() override;
    void transitionEx(handles::TextureHandle texture) override;
    void traceRaysEx(handles::PipelineHandle pipeline, handles::ShaderBindingTableHandle sbt, std::uint32_t width,
                     std::uint32_t height, std::uint32_t depth = 1) override;
    void buildBlasEx(handles::AccelerationStructureHandle blas, const BlasGeometryDesc& geometry,
                     bool update) override;
    void buildTlasEx(handles::AccelerationStructureHandle tlas, std::span<const AccelerationInstanceDesc> instances,
                     bool update) override;
    void copyTextureEx(handles::TextureHandle source, handles::TextureHandle destination) override;
    void clearTextureEx(handles::TextureHandle texture) override;
    void generateMipmapsEx(handles::TextureHandle texture) override;

    void bindRayTracingPipeline(handles::PipelineHandle pipeline) noexcept {
        pipeline_ = pipeline;
    }
    [[nodiscard]] VkCommandBuffer commandBuffer() const noexcept {
        return commandBuffer_;
    }

  private:
    VulkanDevice* device_{};
    VkCommandBuffer commandBuffer_{};
    handles::PipelineHandle pipeline_{};
};

} // namespace dayo::graphics
