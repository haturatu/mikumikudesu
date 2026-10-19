#include "graphics/vulkan/vulkan_device.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace dayo::graphics {
namespace {
void checkTemporal(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}
constexpr VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceRange depthRange{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
} // namespace

void VulkanDevice::ensurePreviewTemporalHistory(VkCommandBuffer command, VkExtent2D extent) {
    auto& history = previewTemporalHistory_;
    if (history.textures[0].valid() && history.extent.width == extent.width && history.extent.height == extent.height)
        return;
    const auto old = history.textures;
    for (const auto texture : old)
        if (texture.valid())
            destroyTextureEx(texture);
    history.textures = {};
    history.valid = false;
    history.phase = 0;
    history.extent = extent;
    const std::array formats{PixelFormat::rgba16Float, PixelFormat::depth32Float,
                             dofCocFormat_ == VK_FORMAT_R16_SFLOAT ? PixelFormat::r16Float : PixelFormat::r32Float,
                             PixelFormat::r32Uint};
    std::array<VkImageMemoryBarrier2, 4> barriers{};
    for (std::size_t i = 0; i < history.textures.size(); ++i) {
        history.textures[i] = createTextureEx({.dimension = TextureDimension::d2,
                                               .extent = {extent.width, extent.height, 1},
                                               .format = formats[i],
                                               .mipLevels = 1,
                                               .arrayLayers = 1,
                                               .usage = ResourceUsage::sampledRead | ResourceUsage::transferDst,
                                               .lifetime = ResourceLifetime::persistent});
        auto& texture = typedTextures_.at(history.textures[i]);
        barriers[i] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                       .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                       .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                       .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                       .image = texture.resource.image,
                       .subresourceRange = i == 1 ? depthRange : colorRange};
        texture.mipLayouts[0] = VK_IMAGE_LAYOUT_GENERAL;
    }
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size()),
                                      .pImageMemoryBarriers = barriers.data()};
    vkCmdPipelineBarrier2(command, &dependency);
}

void VulkanDevice::preparePreviewTemporal(VkCommandBuffer command, Frame& frame, VkExtent2D extent, bool available) {
    auto& motion = frame.motion;
    const auto effects = previewResolvedEffects();
    auto quality = previewRenderSettings_.dof.quality;
    if (quality == PreviewDofQuality::automatic)
        quality = static_cast<PreviewDofQuality>(
            previewEffectivePreset(previewRenderSettings_.preset, options_.quality, previewIntegrated()));
    motion.enabled = available && previewRenderSettings_.dof.temporal && effects.depthOfField &&
                     effects.depthOfFieldIntensity > 0.0F && quality != PreviewDofQuality::performance &&
                     !previewStillQuality_ && previewGpuScene_.view.perspective && previewVertexSize_ != 0;
    auto& history = previewTemporalHistory_;
    if (!motion.enabled) {
        motion.valid = false;
        history.valid = false;
        // This frame slot's fence has completed. Drop the extra full-size
        // attachments when returning to the APU path instead of retaining
        // temporal VRAM until the next viewport resize.
        auto& ao = frame.previewAo;
        if (ao.motionView != VK_NULL_HANDLE)
            vkDestroyImageView(device_, ao.motionView, nullptr);
        if (ao.motionImage != VK_NULL_HANDLE)
            vkDestroyImage(device_, ao.motionImage, nullptr);
        if (ao.motionMemory != VK_NULL_HANDLE)
            vkFreeMemory(device_, ao.motionMemory, nullptr);
        ao.motionView = VK_NULL_HANDLE;
        ao.motionImage = VK_NULL_HANDLE;
        ao.motionMemory = VK_NULL_HANDLE;
        ao.motionInitialized = false;
        if (!focusPickRequest_) {
            if (ao.focusIdView != VK_NULL_HANDLE)
                vkDestroyImageView(device_, ao.focusIdView, nullptr);
            if (ao.focusIdImage != VK_NULL_HANDLE)
                vkDestroyImage(device_, ao.focusIdImage, nullptr);
            if (ao.focusIdMemory != VK_NULL_HANDLE)
                vkFreeMemory(device_, ao.focusIdMemory, nullptr);
            ao.focusIdView = VK_NULL_HANDLE;
            ao.focusIdImage = VK_NULL_HANDLE;
            ao.focusIdMemory = VK_NULL_HANDLE;
            ao.focusIdInitialized = false;
        }
        if (history.textures[0].valid() && (history.extent.width != 1U || history.extent.height != 1U))
            ensurePreviewTemporalHistory(command, {1, 1});
        return;
    }
    ensurePreviewTemporalHistory(command, extent);
    const auto& current = previewGpuScene_.view;
    const auto& previous = history.scene;
    const float delta = current.animationTimeSeconds - previous.animationTimeSeconds;
    if (delta < 0.0F || delta > 0.25F || std::abs(current.verticalFovRadians - previous.verticalFovRadians) > 0.15F ||
        std::abs(current.cameraDistance - previous.cameraDistance) > std::max(0.5F, previous.cameraDistance * 0.25F) ||
        std::abs(current.focusDistance - previous.focusDistance) > std::max(0.25F, previous.focusDistance * 0.25F))
        history.valid = false;
    for (std::size_t axis = 0; axis < 3; ++axis)
        if (std::abs(current.cameraRotation[axis] - previous.cameraRotation[axis]) > 0.5F ||
            std::abs(current.target[axis] - previous.target[axis]) > std::max(0.5F, previous.cameraDistance * 0.25F))
            history.valid = false;
    const VkDeviceSize bytes = previewVertexSize_ / sizeof(PreviewVertex) * sizeof(PreviewDeformedVertex);
    if (history.capacity < bytes) {
        const auto oldBuffer = history.positions;
        const auto oldMemory = history.positionsMemory;
        retirePreviewResource([this, oldBuffer, oldMemory] {
            if (oldBuffer != VK_NULL_HANDLE)
                vkDestroyBuffer(device_, oldBuffer, nullptr);
            if (oldMemory != VK_NULL_HANDLE)
                vkFreeMemory(device_, oldMemory, nullptr);
        });
        const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size = bytes,
                                      .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        allocateBufferMemory(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, history.positions, history.positionsMemory);
        history.capacity = bytes;
        history.valid = false;
    }
    motion.valid = history.valid;
    if (!motion.valid)
        history.phase = 0;
    if (motion.cameraBuffer == VK_NULL_HANDLE) {
        const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size = sizeof(float) * 16,
                                      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        allocateBufferMemory(info, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             motion.cameraBuffer, motion.cameraMemory);
        checkTemporal(vkMapMemory(device_, motion.cameraMemory, 0, info.size, 0, &motion.mappedCamera),
                      "map motion camera");
        const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool = previewDescriptorPool_,
                                                   .descriptorSetCount = 1,
                                                   .pSetLayouts = &previewSkinningDescriptorSetLayout_};
        checkTemporal(vkAllocateDescriptorSets(device_, &allocate, &motion.descriptor), "allocate motion camera set");
        const VkDescriptorBufferInfo buffer{motion.cameraBuffer, 0, info.size};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = motion.descriptor,
                                         .dstBinding = 0,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         .pBufferInfo = &buffer};
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
    const auto& camera = motion.valid ? history.scene : current;
    std::array<float, 16> constants{};
    std::copy_n(camera.cameraRotation, 3, constants.begin());
    constants[3] = camera.cameraDistance;
    std::copy_n(camera.target, 3, constants.begin() + 4);
    constants[7] = camera.verticalFovRadians;
    constants[8] = static_cast<float>(extent.width);
    constants[9] = static_cast<float>(extent.height);
    constants[10] = motion.valid ? history.jitter[0] : previewJitter_[0];
    constants[11] = motion.valid ? history.jitter[1] : previewJitter_[1];
    constants[12] = motion.valid ? 1.0F : 0.0F;
    std::memcpy(motion.mappedCamera, constants.data(), sizeof(constants));
    ensurePreviewAoResources(frame, extent);
    ensurePreviewFocusIdResource(frame, extent);
    auto& ao = frame.previewAo;
    if (ao.motionImage == VK_NULL_HANDLE) {
        const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                     .imageType = VK_IMAGE_TYPE_2D,
                                     .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                     .extent = {extent.width, extent.height, 1},
                                     .mipLevels = 1,
                                     .arrayLayers = 1,
                                     .samples = VK_SAMPLE_COUNT_1_BIT,
                                     .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        allocateImageMemory(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, ao.motionImage, ao.motionMemory);
        const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                         .image = ao.motionImage,
                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                         .format = info.format,
                                         .subresourceRange = colorRange};
        checkTemporal(vkCreateImageView(device_, &view, nullptr, &ao.motionView), "create preview motion view");
    }
}

void VulkanDevice::recordPreviewTemporalHistory(VkCommandBuffer command, Frame& frame, VkExtent2D extent) {
    if (!frame.motion.enabled)
        return;
    auto& history = previewTemporalHistory_;
    const std::array sources{frame.previewDof.images[10], frame.previewAo.depthImage, frame.previewDof.images[2],
                             frame.previewAo.focusIdImage};
    std::array<VkImageMemoryBarrier2, 8> barriers{};
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const auto range = i == 1 ? depthRange : colorRange;
        const auto sourceLayout = i == 1 || i == 3 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        barriers[i * 2] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                           .oldLayout = sourceLayout,
                           .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                           .image = sources[i],
                           .subresourceRange = range};
        barriers[i * 2 + 1] = barriers[i * 2];
        auto& destination = barriers[i * 2 + 1];
        destination.image = typedTextures_.at(history.textures[i]).resource.image;
        destination.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        destination.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        destination.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                .imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size()),
                                .pImageMemoryBarriers = barriers.data()};
    vkCmdPipelineBarrier2(command, &dependency);
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const auto aspect = i == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        const VkImageCopy copy{.srcSubresource = {static_cast<VkImageAspectFlags>(aspect), 0, 0, 1},
                               .dstSubresource = {static_cast<VkImageAspectFlags>(aspect), 0, 0, 1},
                               .extent = {extent.width, extent.height, 1}};
        vkCmdCopyImage(command, sources[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       typedTextures_.at(history.textures[i]).resource.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    }
    for (std::size_t i = 0; i < barriers.size(); ++i) {
        auto& barrier = barriers[i];
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = i % 2 == 0 ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        if (i % 2 == 0)
            std::swap(barrier.oldLayout, barrier.newLayout);
    }
    vkCmdPipelineBarrier2(command, &dependency);
    const VkMemoryBarrier2 toCopy{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    dependency.imageMemoryBarrierCount = 0;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &toCopy;
    vkCmdPipelineBarrier2(command, &dependency);
    const VkBufferCopy positions{0, 0, previewVertexSize_ / sizeof(PreviewVertex) * sizeof(PreviewDeformedVertex)};
    vkCmdCopyBuffer(command, frame.previewDeformedBuffer, history.positions, 1, &positions);
    const VkMemoryBarrier2 toVertex{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                                    .dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT};
    dependency.pMemoryBarriers = &toVertex;
    vkCmdPipelineBarrier2(command, &dependency);
    history.scene = previewGpuScene_.view;
    history.jitter = previewJitter_;
    history.valid = true;
    history.phase = (history.phase + 1U) % 4U;
}

void VulkanDevice::destroyPreviewTemporalHistory() noexcept {
    auto& history = previewTemporalHistory_;
    for (const auto texture : history.textures)
        if (texture.valid()) {
            try {
                destroyTextureEx(texture);
            } catch (...) {
            }
        }
    if (history.positions != VK_NULL_HANDLE)
        vkDestroyBuffer(device_, history.positions, nullptr);
    if (history.positionsMemory != VK_NULL_HANDLE)
        vkFreeMemory(device_, history.positionsMemory, nullptr);
    history = {};
}
} // namespace dayo::graphics
