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

void VulkanCommandList::drawIndexedEx(const IndexedDrawEx& draw) {
    if (device_ == nullptr)
        throw std::logic_error("typed indexed draw requires a Vulkan device");
    device_->recordDrawIndexed(commandBuffer_, draw);
}

void VulkanCommandList::drawVertexBufferEx(const VertexDrawEx& draw) {
    if (device_ == nullptr)
        throw std::logic_error("typed vertex-buffer draw requires a Vulkan device");
    device_->recordDrawVertexBuffer(commandBuffer_, draw);
}

void VulkanCommandList::drawIndexedBufferlessEx(handles::BufferHandle indexBuffer, std::uint32_t indexCount,
                                                std::uint32_t instanceCount) {
    if (device_ == nullptr)
        throw std::logic_error("typed vertex-bufferless indexed draw requires a Vulkan device");
    device_->recordDrawIndexedBufferless(commandBuffer_, indexBuffer, indexCount, instanceCount);
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
    if (device_ == nullptr)
        throw std::logic_error("typed pipeline bind requires a Vulkan device");
    device_->recordBindPipeline(commandBuffer_, pipeline);
    pipeline_ = pipeline;
}

void VulkanCommandList::bindDescriptorSetEx(handles::DescriptorSetHandle set, std::uint32_t setIndex) {
    if (device_ == nullptr || !pipeline_.valid())
        throw std::logic_error("typed descriptor set requires a bound pipeline");
    device_->recordBindDescriptorSet(commandBuffer_, pipeline_, set, setIndex);
}

void VulkanCommandList::pushConstantsEx(std::span<const std::byte> bytes) {
    if (device_ == nullptr || !pipeline_.valid())
        throw std::logic_error("typed push constants require a bound pipeline");
    device_->recordPushConstants(commandBuffer_, pipeline_, bytes);
}

void VulkanCommandList::beginRenderingEx(handles::TextureHandle target, bool clear) {
    if (device_ == nullptr)
        throw std::logic_error("typed rendering requires a Vulkan device");
    if (!renderingTargets_.empty())
        throw std::logic_error("typed rendering is already active on this command list");
    device_->recordBeginRendering(commandBuffer_, target, clear);
    renderingTargets_.push_back({.texture = target, .mipLevel = 0});
}

void VulkanCommandList::beginRenderingEx(const RenderingInfoEx& info) {
    if (device_ == nullptr)
        throw std::logic_error("typed rendering requires a Vulkan device");
    if (!renderingTargets_.empty())
        throw std::logic_error("typed rendering is already active on this command list");
    device_->recordBeginRendering(commandBuffer_, info);
    renderingTargets_.reserve(info.colors.size() + (info.depth.has_value() ? 1U : 0U));
    for (const auto& color : info.colors)
        renderingTargets_.push_back({.texture = color.texture, .mipLevel = color.mipLevel});
    if (info.depth.has_value())
        renderingTargets_.push_back({.texture = info.depth->texture, .mipLevel = info.depth->mipLevel});
}

void VulkanCommandList::endRenderingEx() {
    if (device_ == nullptr || renderingTargets_.empty())
        throw std::logic_error("typed rendering is not active on this command list");
    device_->recordEndRendering(commandBuffer_, renderingTargets_);
    renderingTargets_.clear();
}

void VulkanCommandList::memoryBarrierEx() {
    if (device_ == nullptr)
        throw std::logic_error("typed memory barrier requires a Vulkan device");
    device_->recordMemoryBarrier(commandBuffer_);
}

void VulkanCommandList::transferBarrierEx() {
    if (device_ == nullptr)
        throw std::logic_error("typed transfer barrier requires a Vulkan device");
    device_->recordTransferBarrier(commandBuffer_);
}

void VulkanCommandList::accelerationStructureBarrierEx() {
    if (device_ == nullptr)
        throw std::logic_error("acceleration barrier requires a Vulkan device");
    device_->recordAccelerationStructureBarrier(commandBuffer_);
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

void VulkanCommandList::buildBlasEx(handles::AccelerationStructureHandle blas, const BlasGeometryDesc& geometry,
                                    bool update) {
    if (device_ == nullptr)
        throw std::logic_error("typed BLAS build requires a Vulkan device");
    if (!update)
        throw std::invalid_argument("Vulkan command-list BLAS recording only supports updates");
    device_->recordBlasUpdate(commandBuffer_, blas, geometry);
}

void VulkanCommandList::buildTlasEx(handles::AccelerationStructureHandle tlas,
                                    std::span<const AccelerationInstanceDesc> instances, bool update) {
    if (device_ == nullptr)
        throw std::logic_error("typed TLAS build requires a Vulkan device");
    if (!update)
        throw std::invalid_argument("Vulkan command-list TLAS recording only supports updates");
    device_->recordTlasUpdate(commandBuffer_, tlas, instances);
}

void VulkanCommandList::copyTextureEx(handles::TextureHandle source, handles::TextureHandle destination) {
    if (device_ == nullptr)
        throw std::logic_error("typed texture copy requires a Vulkan device");
    device_->recordCopyTexture(commandBuffer_, source, destination);
}

void VulkanCommandList::blitTextureEx(handles::TextureHandle source, handles::TextureHandle destination,
                                      std::array<std::uint32_t, 4> sourceRect) {
    if (device_ == nullptr)
        throw std::logic_error("typed texture blit requires a Vulkan device");
    device_->recordBlitTexture(commandBuffer_, source, destination, sourceRect);
}

void VulkanCommandList::clearTextureEx(handles::TextureHandle texture) {
    clearTextureEx(texture, {0.0F, 0.0F, 0.0F, 0.0F});
}

void VulkanCommandList::clearTextureEx(handles::TextureHandle texture, const std::array<float, 4>& value) {
    if (device_ == nullptr)
        throw std::logic_error("typed texture clear requires a Vulkan device");
    device_->recordClearTexture(commandBuffer_, texture, value);
}

void VulkanCommandList::clearBufferEx(handles::BufferHandle buffer, std::uint32_t value) {
    if (device_ == nullptr)
        throw std::logic_error("typed buffer clear requires a Vulkan device");
    device_->recordClearBuffer(commandBuffer_, buffer, value);
}

void VulkanCommandList::clearBufferEx(handles::BufferHandle buffer, const std::array<float, 4>& value) {
    if (device_ == nullptr)
        throw std::logic_error("typed buffer clear requires a Vulkan device");
    device_->recordClearBuffer(commandBuffer_, buffer, value);
}

void VulkanCommandList::generateMipmapsEx(handles::TextureHandle texture) {
    if (device_ == nullptr)
        throw std::logic_error("typed mipmap generation requires a Vulkan device");
    device_->recordGenerateMipmaps(commandBuffer_, texture);
}

void VulkanCommandList::copyBufferEx(handles::BufferHandle source, handles::BufferHandle destination) {
    if (device_ == nullptr)
        throw std::logic_error("typed buffer copy requires a Vulkan device");
    device_->recordCopyBuffer(commandBuffer_, source, destination);
}

void VulkanCommandList::uploadBufferEx(handles::BufferHandle destination, std::span<const std::byte> bytes,
                                       std::size_t offset) {
    if (device_ == nullptr)
        throw std::logic_error("typed command-list buffer upload requires a Vulkan device");
    device_->recordUploadBuffer(commandBuffer_, destination, bytes, offset);
}

void VulkanCommandList::flushAndWaitForHostReadbackEx() {
    if (device_ == nullptr || !renderingTargets_.empty())
        throw std::logic_error("host readback requires an idle Vulkan rendering scope");
    device_->flushCommandBufferForHostReadback(commandBuffer_);
    pipeline_ = {};
}

} // namespace dayo::graphics
