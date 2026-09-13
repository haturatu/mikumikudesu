#include "graphics/vulkan/vulkan_command_list.hpp"

#include "graphics/vulkan/vulkan_device.hpp"

#include <stdexcept>

namespace dayo::graphics {

VulkanCommandList::VulkanCommandList(VulkanDevice& device, VkCommandBuffer commandBuffer)
    : device_(&device), commandBuffer_(commandBuffer) {
    if (commandBuffer_ == VK_NULL_HANDLE)
        throw std::invalid_argument("Vulkan command list requires a command buffer");
}

void VulkanCommandList::transition(TextureHandle) {
    throw std::logic_error("VulkanCommandList legacy texture transitions are not implemented");
}

void VulkanCommandList::bindPipeline(PipelineHandle) {
    throw std::logic_error("VulkanCommandList legacy pipelines are not implemented");
}

void VulkanCommandList::draw(std::uint32_t vertexCount, std::uint32_t instanceCount) {
    vkCmdDraw(commandBuffer_, vertexCount, instanceCount, 0, 0);
}

void VulkanCommandList::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    vkCmdDispatch(commandBuffer_, x, y, z);
}

void VulkanCommandList::traceRays(std::uint32_t, std::uint32_t) {
    throw std::logic_error("VulkanCommandList traceRays requires an SBT and bound RT pipeline");
}

void VulkanCommandList::traceRays(handles::ShaderBindingTableHandle sbt, std::uint32_t width, std::uint32_t height,
                                  std::uint32_t depth) {
    if (device_ == nullptr || !pipeline_.valid())
        throw std::logic_error("VulkanCommandList traceRays has no bound RT pipeline");
    device_->recordTraceRays(commandBuffer_, pipeline_, sbt, width, height, depth);
}

void VulkanCommandList::bindResources(std::span<const DescriptorBinding>) {
    throw std::logic_error("VulkanCommandList legacy descriptor bindings are not implemented");
}

void VulkanCommandList::pushConstants(std::span<const std::byte>) {
    throw std::logic_error("VulkanCommandList push constants require a typed pipeline layout");
}

void VulkanCommandList::copyTexture(TextureHandle, TextureHandle) {
    throw std::logic_error("VulkanCommandList legacy texture copy is not implemented");
}

void VulkanCommandList::clearTexture(TextureHandle) {
    throw std::logic_error("VulkanCommandList legacy texture clear is not implemented");
}

void VulkanCommandList::generateMipmaps(TextureHandle) {
    throw std::logic_error("VulkanCommandList legacy mipmaps are not implemented");
}

void VulkanCommandList::buildAccelerationStructure(AccelerationStructureHandle) {
    throw std::logic_error("VulkanCommandList legacy acceleration structures are not implemented");
}

void VulkanCommandList::bindPipelineEx(handles::PipelineHandle pipeline) {
    if (!pipeline.valid())
        throw std::invalid_argument("typed pipeline handle is invalid");
    pipeline_ = pipeline;
}

void VulkanCommandList::bindDescriptorSetEx(handles::DescriptorSetHandle set) {
    if (device_ == nullptr || !pipeline_.valid())
        throw std::logic_error("typed descriptor set requires a bound pipeline");
    device_->recordBindDescriptorSet(commandBuffer_, pipeline_, set);
}

void VulkanCommandList::pushConstantsEx(std::span<const std::byte> bytes) {
    if (device_ == nullptr || !pipeline_.valid())
        throw std::logic_error("typed push constants require a bound pipeline");
    device_->recordPushConstants(commandBuffer_, pipeline_, bytes);
}

void VulkanCommandList::transitionEx(handles::TextureHandle texture) {
    if (device_ == nullptr)
        throw std::logic_error("typed transition requires a Vulkan device");
    device_->recordTransitionTexture(commandBuffer_, texture);
}

void VulkanCommandList::traceRaysEx(handles::PipelineHandle pipeline, handles::ShaderBindingTableHandle sbt,
                                    std::uint32_t width, std::uint32_t height, std::uint32_t depth) {
    bindPipelineEx(pipeline);
    traceRays(sbt, width, height, depth);
}

} // namespace dayo::graphics
