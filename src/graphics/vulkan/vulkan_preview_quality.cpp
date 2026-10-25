#include "graphics/preview_color.hpp"
#include "graphics/vulkan/vulkan_device.hpp"
#include <smaa/AreaTex.h>
#include <smaa/SearchTex.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace dayo::graphics {
namespace {
void checkQuality(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(operation);
}
struct QualityConstants {
    std::array<std::uint32_t, 2> size{}, outputSize{};
    std::array<float, 4> projection{}, light{}, effects{}, temporal{}, color{}, post{}, misc{};
};
static_assert(sizeof(QualityConstants) == 128);
QualityConstants qualityConstants(const PreviewScene& scene, const ResolvedPreviewEffects& effects, VkExtent2D extent,
                                  bool integrated) {
    QualityConstants c;
    c.size = c.outputSize = {extent.width, extent.height};
    c.projection = {scene.perspective
                        ? 1.0F / std::tan(scene.verticalFovRadians * 0.5F)
                        : std::max(scene.cameraDistance, 0.1F) * std::tan(scene.verticalFovRadians * 0.5F),
                    static_cast<float>(extent.width) / static_cast<float>(extent.height), 0.05F, 100.0F};
    std::array<float, 3> l{-scene.lightDirection[0], -scene.lightDirection[1], -scene.lightDirection[2]};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const std::size_t a = axis == 0 ? 1 : 0, b = axis == 2 ? 1 : 2;
        const float angle = scene.cameraRotation[axis], cosine = std::cos(angle), sine = std::sin(angle);
        const float x = l[a], y = l[b];
        l[a] = axis == 1 ? cosine * x + sine * y : cosine * x - sine * y;
        l[b] = axis == 1 ? -sine * x + cosine * y : sine * x + cosine * y;
    }
    c.light = {l[0], l[1], l[2], effects.contactShadowDistance};
    c.effects = {effects.reflectionIntensity, effects.refractionIntensity, effects.refractionIor,
                 effects.refractionThickness};
    c.temporal = {effects.temporalHistoryWeight, 0, scene.perspective ? 1.0F : 0.0F, integrated ? 16.0F : 32.0F};
    c.color = previewColorAdjustment(effects.exposure, effects.whiteBalance);
    c.post = {static_cast<float>(effects.viewTransform), effects.bloomIntensity, effects.reflectionDistance, 0};
    return c;
}
void stampQuality(VkCommandBuffer command, VkQueryPool pool, PreviewQualityTimestamp slot) {
    if (pool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, slot);
}
void qualityBarrier(VkCommandBuffer command, VkPipelineStageFlags2 source, VkAccessFlags2 sourceAccess,
                    VkPipelineStageFlags2 destination, VkAccessFlags2 destinationAccess) {
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = source,
                                   .srcAccessMask = sourceAccess,
                                   .dstStageMask = destination,
                                   .dstAccessMask = destinationAccess};
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(command, &dependency);
}
} // namespace

void VulkanDevice::ensurePreviewQualityPipelines() {
    if (qualityPipelineLayout_ != VK_NULL_HANDLE)
        return;
    const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                      .magFilter = VK_FILTER_NEAREST,
                                      .minFilter = VK_FILTER_NEAREST,
                                      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                      .maxLod = 0};
    checkQuality(vkCreateSampler(device_, &sampler, nullptr, &qualityPointSampler_), "create quality point sampler");
    std::array<VkDescriptorSetLayoutBinding, 14> bindings{};
    for (std::uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i] = {i,
                       i < 10   ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                       : i < 12 ? VK_DESCRIPTOR_TYPE_SAMPLER
                                : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                       1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    bindings[10].pImmutableSamplers = &previewClampSampler_;
    bindings[11].pImmutableSamplers = &qualityPointSampler_;
    const VkDescriptorSetLayoutCreateInfo descriptor{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                     .bindingCount = static_cast<std::uint32_t>(bindings.size()),
                                                     .pBindings = bindings.data()};
    checkQuality(vkCreateDescriptorSetLayout(device_, &descriptor, nullptr, &qualityDescriptorLayout_),
                 "create quality descriptor layout");
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(QualityConstants)};
    const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                            .setLayoutCount = 1,
                                            .pSetLayouts = &qualityDescriptorLayout_,
                                            .pushConstantRangeCount = 1,
                                            .pPushConstantRanges = &push};
    checkQuality(vkCreatePipelineLayout(device_, &layout, nullptr, &qualityPipelineLayout_),
                 "create quality pipeline layout");
    const std::array programs{std::pair{DAYO_PREVIEW_CONTACT_SHADOW_SPV, "ContactShadow"},
                              std::pair{DAYO_PREVIEW_CONTACT_BLUR_SPV, "ContactBlur"},
                              std::pair{DAYO_PREVIEW_SCREEN_REFLECTION_SPV, "ScreenReflection"},
                              std::pair{DAYO_PREVIEW_SCREEN_COMPOSITE_SPV, "ScreenComposite"},
                              std::pair{DAYO_PREVIEW_TEMPORAL_RESOLVE_SPV, "TemporalResolve"},
                              std::pair{DAYO_PREVIEW_PREPARE_DISPLAY_SPV, "PrepareDisplay"},
                              std::pair{DAYO_PREVIEW_FXAA_RESOLVE_SPV, "FxaaResolve"},
                              std::pair{DAYO_PREVIEW_SMAA_EDGES_SPV, "SmaaEdges"},
                              std::pair{DAYO_PREVIEW_SMAA_WEIGHTS_SPV, "SmaaWeights"},
                              std::pair{DAYO_PREVIEW_SMAA_NEIGHBORHOOD_SPV, "SmaaNeighborhood"}};
    for (std::size_t i = 0; i < programs.size(); ++i) {
        std::ifstream file(programs[i].first, std::ios::binary | std::ios::ate);
        if (!file)
            throw std::runtime_error("cannot open preview quality SPIR-V");
        const auto bytes = static_cast<std::size_t>(file.tellg());
        std::vector<std::uint32_t> words(bytes / sizeof(std::uint32_t));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(bytes));
        if (!file || words.empty() || bytes % sizeof(std::uint32_t) != 0)
            throw std::runtime_error("invalid preview quality SPIR-V");
        const VkShaderModuleCreateInfo moduleInfo{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = words.data()};
        VkShaderModule module{};
        checkQuality(vkCreateShaderModule(device_, &moduleInfo, nullptr, &module), "create quality shader");
        const VkComputePipelineCreateInfo pipeline{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = module,
                      .pName = programs[i].second},
            .layout = qualityPipelineLayout_};
        const auto result =
            vkCreateComputePipelines(device_, pipelineCache_, 1, &pipeline, nullptr, &qualityPipelines_[i]);
        vkDestroyShaderModule(device_, module, nullptr);
        checkQuality(result, "create quality compute pipeline");
    }
}

handles::TextureHandle VulkanDevice::previewQualityTarget(Frame& frame, std::size_t slot, VkExtent2D extent) {
    auto& resource = frame.quality;
    if (resource.textures[slot].valid() && resource.extents[slot].width == extent.width &&
        resource.extents[slot].height == extent.height)
        return resource.textures[slot];
    if (resource.textures[slot].valid())
        destroyTextureEx(resource.textures[slot]);
    resource.textures[slot] =
        createTextureEx({.dimension = TextureDimension::d2,
                         .extent = {extent.width, extent.height, 1},
                         .format = PixelFormat::rgba16Float,
                         .mipLevels = 1,
                         .arrayLayers = 1,
                         .usage = ResourceUsage::sampledRead | ResourceUsage::storageWrite | ResourceUsage::transferSrc,
                         .lifetime = ResourceLifetime::persistent});
    resource.extents[slot] = extent;
    return resource.textures[slot];
}

void VulkanDevice::dispatchPreviewQuality(VkCommandBuffer command, Frame& frame, std::size_t pass, std::size_t target,
                                          VkExtent2D extent, std::span<const VkImageView> inputs,
                                          std::span<const std::byte> constants, std::optional<std::size_t> geometry) {
    ensurePreviewQualityPipelines();
    const auto output = previewQualityTarget(frame, target, extent);
    const auto extra = geometry ? previewQualityTarget(frame, *geometry, extent) : output;
    const auto transition = [&](handles::TextureHandle handle) {
        auto& texture = typedTextures_.at(handle);
        if (texture.mipLayouts[0] != VK_IMAGE_LAYOUT_UNDEFINED)
            return;
        const VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .image = texture.resource.image,
                                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
        vkCmdPipelineBarrier2(command, &dependency);
        texture.mipLayouts[0] = VK_IMAGE_LAYOUT_GENERAL;
    };
    transition(output);
    if (extra != output)
        transition(extra);
    auto& set = frame.quality.sets[pass];
    if (set == VK_NULL_HANDLE) {
        const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool = previewDescriptorPool_,
                                                   .descriptorSetCount = 1,
                                                   .pSetLayouts = &qualityDescriptorLayout_};
        checkQuality(vkAllocateDescriptorSets(device_, &allocate, &set), "allocate quality descriptor");
    }
    std::array<VkDescriptorImageInfo, 12> images{};
    std::array<VkWriteDescriptorSet, 12> writes{};
    const auto fallback = typedTextures_.at(previewFallbackAoTexture_).view;
    for (std::uint32_t i = 0; i < images.size(); ++i) {
        const auto binding = i < 10 ? i : i + 2;
        const auto view = i < 10 ? (i < inputs.size() && inputs[i] != VK_NULL_HANDLE ? inputs[i] : fallback)
                                 : typedTextures_.at(i == 10 ? output : extra).view;
        VkImageLayout imageLayout = i < 10 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        if (i < 10) {
            for (const auto& [handle, texture] : typedTextures_) {
                static_cast<void>(handle);
                if (texture.view == view) {
                    imageLayout = texture.mipLayouts[0];
                    break;
                }
            }
        }
        if (i < 10 && (view == frame.previewAo.motionView ||
                       std::ranges::find(frame.previewDof.views, view) != std::end(frame.previewDof.views) ||
                       std::ranges::find(frame.previewBloom.views, view) != std::end(frame.previewBloom.views)))
            imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        images[i] = {VK_NULL_HANDLE, view, imageLayout};
        writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = set,
                     .dstBinding = binding,
                     .descriptorCount = 1,
                     .descriptorType = i < 10 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     .pImageInfo = &images[i]};
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, qualityPipelines_[pass]);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, qualityPipelineLayout_, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(command, qualityPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       static_cast<std::uint32_t>(constants.size()), constants.data());
    vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
    qualityBarrier(command, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                       VK_ACCESS_2_TRANSFER_READ_BIT);
}

void VulkanDevice::recordPreviewContactShadows(VkCommandBuffer command, Frame& frame, VkExtent2D extent) {
    const auto effects = previewResolvedEffects();
    if (effects.contactShadowIntensity <= 0)
        for (std::size_t slot = 0; slot < 2; ++slot)
            if (frame.quality.textures[slot].valid()) {
                destroyTextureEx(frame.quality.textures[slot]);
                frame.quality.textures[slot] = {};
                frame.quality.extents[slot] = {};
            }
    if (!frame.previewAo.initialized)
        return;
    VkImageView view = typedTextures_.at(previewFallbackAoTexture_).view;
    if (effects.contactShadowIntensity > 0) {
        auto constants = qualityConstants(previewGpuScene_.view, effects, extent, previewIntegrated());
        stampQuality(command, frame.timestampQueryPool, previewContactBegin);
        const VkExtent2D half{(extent.width + 1) / 2, (extent.height + 1) / 2};
        constants.outputSize = {half.width, half.height};
        constants.temporal[3] = previewIntegrated() ? 6.0F : 10.0F;
        std::array<VkImageView, 10> inputs{};
        inputs[1] = frame.previewAo.depthView;
        inputs[2] = frame.previewAo.normalView;
        dispatchPreviewQuality(command, frame, 0, 0, half, inputs, std::as_bytes(std::span{&constants, 1}));
        inputs[7] = typedTextures_.at(frame.quality.textures[0]).view;
        dispatchPreviewQuality(command, frame, 1, 1, half, inputs, std::as_bytes(std::span{&constants, 1}));
        stampQuality(command, frame.timestampQueryPool, previewContactEnd);
        view = typedTextures_.at(frame.quality.textures[1]).view;
    }
    const VkDescriptorImageInfo image{VK_NULL_HANDLE, view,
                                      effects.contactShadowIntensity > 0 ? VK_IMAGE_LAYOUT_GENERAL
                                                                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .dstSet = frame.previewAo.sampleSet,
                                     .dstBinding = 2,
                                     .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                     .pImageInfo = &image};
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void VulkanDevice::recordPreviewQuality(VkCommandBuffer command, Frame& frame, VkExtent2D extent) {
    frame.quality.displayView = VK_NULL_HANDLE;
    frame.quality.displayPrepared = false;
    auto effects = previewResolvedEffects();
    const bool screen = !previewFinalHdr_.valid() && frame.previewAo.initialized &&
                        (effects.reflectionIntensity > 0 || effects.refractionIntensity > 0);
    auto aa = effects.antiAliasing;
    if (aa == PreviewAntiAliasing::taa && !frame.motion.enabled)
        aa = PreviewAntiAliasing::smaa;
    // This frame slot's fence has completed. Release disabled full-size targets
    // through the texture retirement queue instead of retaining High-preset VRAM.
    const auto release = [&](std::size_t slot) {
        auto& texture = frame.quality.textures[slot];
        if (texture.valid()) {
            destroyTextureEx(texture);
            texture = {};
            frame.quality.extents[slot] = {};
        }
    };
    if (!screen) {
        release(2);
        release(3);
    }
    if (aa != PreviewAntiAliasing::smaa && aa != PreviewAntiAliasing::taa) {
        release(5);
        release(6);
    }
    if (aa == PreviewAntiAliasing::off) {
        release(4);
        release(7);
    }
    if (aa != PreviewAntiAliasing::taa) {
        release(8);
        release(9);
        for (auto& texture : previewAaHistory_.textures)
            if (texture.valid()) {
                destroyTextureEx(texture);
                texture = {};
            }
        previewAaHistory_.valid = false;
    }
    if (!screen && aa == PreviewAntiAliasing::off)
        return;
    ensurePreviewQualityPipelines();
    auto constants = qualityConstants(previewGpuScene_.view, effects, extent, previewIntegrated());
    std::array<VkImageView, 10> inputs{};
    inputs[0] = previewFinalHdr_.valid()  ? typedTextures_.at(previewFinalHdr_).view
                : frame.previewDof.active ? frame.previewDof.views[frame.previewDof.output]
                                          : frame.previewHdr.view;
    if (frame.previewAo.initialized) {
        inputs[1] = frame.previewAo.depthView;
        inputs[2] = frame.previewAo.normalView;
    }
    if (frame.previewHdr.surfaceTexture.valid())
        inputs[3] = typedTextures_.at(frame.previewHdr.surfaceTexture).view;
    if (screen) {
        stampQuality(command, frame.timestampQueryPool, previewReflectionBegin);
        if (effects.reflectionIntensity > 0) {
            const VkExtent2D half{(extent.width + 1) / 2, (extent.height + 1) / 2};
            constants.outputSize = {half.width, half.height};
            dispatchPreviewQuality(command, frame, 2, 2, half, inputs, std::as_bytes(std::span{&constants, 1}));
            inputs[7] = typedTextures_.at(frame.quality.textures[2]).view;
        }
        constants.outputSize = constants.size;
        dispatchPreviewQuality(command, frame, 3, 3, extent, inputs, std::as_bytes(std::span{&constants, 1}));
        inputs[0] = typedTextures_.at(frame.quality.textures[3]).view;
        stampQuality(command, frame.timestampQueryPool, previewReflectionEnd);
    }
    if (aa == PreviewAntiAliasing::taa) {
        stampQuality(command, frame.timestampQueryPool, previewTemporalBegin);
        auto& history = previewAaHistory_;
        if (history.extent.width != extent.width || history.extent.height != extent.height) {
            for (auto handle : history.textures)
                if (handle.valid())
                    destroyTextureEx(handle);
            history = {};
            history.extent = extent;
        }
        for (auto& handle : history.textures) {
            if (handle.valid())
                continue;
            handle = createTextureEx({.dimension = TextureDimension::d2,
                                      .extent = {extent.width, extent.height, 1},
                                      .format = PixelFormat::rgba16Float,
                                      .mipLevels = 1,
                                      .arrayLayers = 1,
                                      .usage = ResourceUsage::sampledRead | ResourceUsage::transferDst,
                                      .lifetime = ResourceLifetime::persistent});
            auto& texture = typedTextures_.at(handle);
            const VkImageMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = texture.resource.image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                              .imageMemoryBarrierCount = 1,
                                              .pImageMemoryBarriers = &barrier};
            vkCmdPipelineBarrier2(command, &dependency);
            texture.mipLayouts[0] = VK_IMAGE_LAYOUT_GENERAL;
        }
        inputs[4] = frame.previewAo.motionView;
        inputs[5] = typedTextures_.at(history.textures[0]).view;
        inputs[6] = typedTextures_.at(history.textures[1]).view;
        constants.temporal[1] = history.valid && frame.motion.valid ? 1.0F : 0.0F;
        dispatchPreviewQuality(command, frame, 4, 9, extent, inputs, std::as_bytes(std::span{&constants, 1}), 8);
        qualityBarrier(command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkImageCopy copy{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .extent = {extent.width, extent.height, 1}};
        for (std::size_t i = 0; i < 2; ++i) {
            const auto source = frame.quality.textures[i == 0 ? 9 : 8];
            vkCmdCopyImage(command, typedTextures_.at(source).resource.image, VK_IMAGE_LAYOUT_GENERAL,
                           typedTextures_.at(history.textures[i]).resource.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        }
        qualityBarrier(command, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        history.valid = true;
        inputs[0] = typedTextures_.at(frame.quality.textures[9]).view;
        stampQuality(command, frame.timestampQueryPool, previewTemporalEnd);
    } else
        previewAaHistory_.valid = false;
    if (aa == PreviewAntiAliasing::off) {
        // Screen-space shading remains scene-referred for the final tone map.
        frame.quality.displayView = inputs[0];
        return;
    }
    stampQuality(command, frame.timestampQueryPool, previewSpatialBegin);
    frame.quality.displayPrepared = true;
    inputs[7] = frame.previewBloom.initialized ? frame.previewBloom.views[0] : VK_NULL_HANDLE;
    if (!effects.bloom || !frame.previewBloom.initialized)
        constants.post[1] = 0;
    dispatchPreviewQuality(command, frame, 5, 4, extent, inputs, std::as_bytes(std::span{&constants, 1}));
    inputs[0] = typedTextures_.at(frame.quality.textures[4]).view;
    if (aa == PreviewAntiAliasing::fxaa) {
        dispatchPreviewQuality(command, frame, 6, 7, extent, inputs, std::as_bytes(std::span{&constants, 1}));
        frame.quality.displayView = typedTextures_.at(frame.quality.textures[7]).view;
    } else if (aa == PreviewAntiAliasing::smaa || aa == PreviewAntiAliasing::taa) {
        if (!smaaLookup_[0].valid()) {
            std::vector<std::uint8_t> area(static_cast<std::size_t>(AREATEX_WIDTH) * AREATEX_HEIGHT * 4U, 255);
            for (std::size_t pixel = 0; pixel < area.size() / 4; ++pixel) {
                area[pixel * 4] = areaTexBytes[pixel * 2];
                area[pixel * 4 + 1] = areaTexBytes[pixel * 2 + 1];
                area[pixel * 4 + 2] = 0;
            }
            smaaLookup_[0] = createTextureEx({.dimension = TextureDimension::d2,
                                              .extent = {AREATEX_WIDTH, AREATEX_HEIGHT, 1},
                                              .format = PixelFormat::rgba8Unorm,
                                              .mipLevels = 1,
                                              .arrayLayers = 1,
                                              .usage = ResourceUsage::sampledRead | ResourceUsage::transferDst,
                                              .lifetime = ResourceLifetime::persistent});
            uploadTextureEx(smaaLookup_[0], area, 0, 0);
            smaaLookup_[1] = createTextureEx({.dimension = TextureDimension::d2,
                                              .extent = {SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1},
                                              .format = PixelFormat::r8Unorm,
                                              .mipLevels = 1,
                                              .arrayLayers = 1,
                                              .usage = ResourceUsage::sampledRead | ResourceUsage::transferDst,
                                              .lifetime = ResourceLifetime::persistent});
            uploadTextureEx(smaaLookup_[1], searchTexBytes, 0, 0);
        }
        inputs[8] = typedTextures_.at(smaaLookup_[0]).view;
        inputs[9] = typedTextures_.at(smaaLookup_[1]).view;
        dispatchPreviewQuality(command, frame, 7, 5, extent, inputs, std::as_bytes(std::span{&constants, 1}));
        inputs[0] = typedTextures_.at(frame.quality.textures[5]).view;
        dispatchPreviewQuality(command, frame, 8, 6, extent, inputs, std::as_bytes(std::span{&constants, 1}));
        inputs[0] = typedTextures_.at(frame.quality.textures[4]).view;
        inputs[7] = typedTextures_.at(frame.quality.textures[6]).view;
        dispatchPreviewQuality(command, frame, 9, 7, extent, inputs, std::as_bytes(std::span{&constants, 1}));
        frame.quality.displayView = typedTextures_.at(frame.quality.textures[7]).view;
    } else
        frame.quality.displayView = inputs[0];
}

void VulkanDevice::ensurePreviewSurfaceResource(Frame& frame, VkExtent2D extent) {
    if (frame.previewHdr.surfaceTexture.valid())
        return;
    frame.previewHdr.surfaceTexture =
        createTextureEx({.dimension = TextureDimension::d2,
                         .extent = {extent.width, extent.height, 1},
                         .format = PixelFormat::rgba16Float,
                         .mipLevels = 1,
                         .arrayLayers = 1,
                         .usage = ResourceUsage::colorAttachment | ResourceUsage::sampledRead,
                         .lifetime = ResourceLifetime::persistent});
}
void VulkanDevice::destroyPreviewQualityResource(Frame& frame) noexcept {
    for (auto& texture : frame.quality.textures)
        if (texture.valid()) {
            try {
                destroyTextureEx(texture);
            } catch (...) {
            }
        }
    for (auto set : frame.quality.sets)
        if (set != VK_NULL_HANDLE)
            vkFreeDescriptorSets(device_, previewDescriptorPool_, 1, &set);
    frame.quality = {};
}
void VulkanDevice::destroyPreviewQualityPipelines() noexcept {
    for (auto& frame : frames_)
        destroyPreviewQualityResource(frame);
    for (auto texture : previewAaHistory_.textures)
        if (texture.valid()) {
            try {
                destroyTextureEx(texture);
            } catch (...) {
            }
        }
    for (auto texture : smaaLookup_)
        if (texture.valid()) {
            try {
                destroyTextureEx(texture);
            } catch (...) {
            }
        }
    for (auto pipeline : qualityPipelines_)
        if (pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device_, pipeline, nullptr);
    if (qualityPipelineLayout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, qualityPipelineLayout_, nullptr);
    if (qualityDescriptorLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, qualityDescriptorLayout_, nullptr);
    if (qualityPointSampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device_, qualityPointSampler_, nullptr);
    qualityPipelines_ = {};
    qualityPipelineLayout_ = VK_NULL_HANDLE;
    qualityDescriptorLayout_ = VK_NULL_HANDLE;
    qualityPointSampler_ = VK_NULL_HANDLE;
    previewAaHistory_ = {};
    smaaLookup_ = {};
}
} // namespace dayo::graphics
