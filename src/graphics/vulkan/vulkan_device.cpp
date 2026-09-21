#include "graphics/vulkan/vulkan_device.hpp"
#include "graphics/vulkan/vulkan_command_list.hpp"
#include "graphics/vulkan/vulkan_upload_context.hpp"

#include "core/log.hpp"
#include "graphics/subayai_deform.hpp"
#include "graphics/subayai_environment.hpp"
#include "graphics/timestamp.hpp"
#include "platform/window.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"

#if DAYO_ENABLE_VMA
#include <vk_mem_alloc.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#if DAYO_HAS_IMGUI
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dayo::graphics {
namespace {

void check(VkResult result, std::string_view operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
    }
}

#if DAYO_HAS_IMGUI
template <typename Descriptor> std::uint64_t descriptorId(Descriptor descriptor) noexcept {
    if constexpr (std::is_pointer_v<Descriptor>) {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(descriptor));
    } else {
        static_assert(sizeof(Descriptor) <= sizeof(std::uint64_t));
        return static_cast<std::uint64_t>(descriptor);
    }
}
#endif

VkDeviceSize growPreviewCapacity(VkDeviceSize required) {
    if (required == 0)
        return 0;
    VkDeviceSize capacity = 1;
    while (capacity < required) {
        if (capacity > std::numeric_limits<VkDeviceSize>::max() / 2)
            return required;
        capacity *= 2;
    }
    return capacity;
}

bool samePreviewMaterial(const PreviewMaterial& left, const PreviewMaterial& right) {
    return std::equal(std::begin(left.diffuse), std::end(left.diffuse), std::begin(right.diffuse)) &&
           std::equal(std::begin(left.ambient), std::end(left.ambient), std::begin(right.ambient)) &&
           left.shininess == right.shininess &&
           std::equal(std::begin(left.specular), std::end(left.specular), std::begin(right.specular)) &&
           std::equal(std::begin(left.textureMultiply), std::end(left.textureMultiply),
                      std::begin(right.textureMultiply)) &&
           std::equal(std::begin(left.textureAdd), std::end(left.textureAdd), std::begin(right.textureAdd)) &&
           std::equal(std::begin(left.sphereMultiply), std::end(left.sphereMultiply),
                      std::begin(right.sphereMultiply)) &&
           std::equal(std::begin(left.sphereAdd), std::end(left.sphereAdd), std::begin(right.sphereAdd)) &&
           std::equal(std::begin(left.toonMultiply), std::end(left.toonMultiply), std::begin(right.toonMultiply)) &&
           std::equal(std::begin(left.toonAdd), std::end(left.toonAdd), std::begin(right.toonAdd)) &&
           std::equal(std::begin(left.edgeColor), std::end(left.edgeColor), std::begin(right.edgeColor)) &&
           left.edgeSize == right.edgeSize && left.doubleSided == right.doubleSided &&
           left.edgeEnabled == right.edgeEnabled && left.textureSlot == right.textureSlot &&
           left.toonTextureSlot == right.toonTextureSlot && left.sphereTextureSlot == right.sphereTextureSlot &&
           left.toonMode == right.toonMode && left.sphereMode == right.sphereMode;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        log::error("Vulkan validation: ", data->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        log::warn("Vulkan validation: ", data->pMessage);
    } else {
        log::debug("Vulkan validation: ", data->pMessage);
    }
    return VK_FALSE;
}

std::vector<std::byte> readBinary(const char* filename) {
    std::ifstream input(filename, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error(std::string("cannot open shader: ") + filename);
    const auto end = input.tellg();
    if (end <= 0)
        throw std::runtime_error(std::string("empty shader: ") + filename);
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input)
        throw std::runtime_error(std::string("cannot read shader: ") + filename);
    return bytes;
}

bool hasName(std::span<const VkExtensionProperties> values, const char* name) {
    return std::ranges::any_of(
        values, [name](const VkExtensionProperties& value) { return std::strcmp(value.extensionName, name) == 0; });
}

bool hasLayer(std::span<const VkLayerProperties> values, const char* name) {
    return std::ranges::any_of(
        values, [name](const VkLayerProperties& value) { return std::strcmp(value.layerName, name) == 0; });
}

VkFormat toVkFormat(TextureDesc::Format format) {
    switch (format) {
    case TextureDesc::Format::rgba8Unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureDesc::Format::rgba8Srgb:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case TextureDesc::Format::rgba16Float:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureDesc::Format::rgba32Float:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TextureDesc::Format::depth32Float:
        return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkBufferUsageFlags toVkUsage(BufferDesc::Usage usage) {
    switch (usage) {
    case BufferDesc::Usage::vertex:
        return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    case BufferDesc::Usage::index:
        return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    case BufferDesc::Usage::uniform:
        return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    case BufferDesc::Usage::storage:
        return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    case BufferDesc::Usage::accelerationStructure:
        return VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    return 0;
}

VkFormat toVkFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::r8Unorm:
        return VK_FORMAT_R8_UNORM;
    case PixelFormat::r16Float:
        return VK_FORMAT_R16_SFLOAT;
    case PixelFormat::r16g16Float:
        return VK_FORMAT_R16G16_SFLOAT;
    case PixelFormat::r32Float:
        return VK_FORMAT_R32_SFLOAT;
    case PixelFormat::r32g32Float:
        return VK_FORMAT_R32G32_SFLOAT;
    case PixelFormat::rgba8Unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case PixelFormat::rgba8Srgb:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case PixelFormat::rgba16Float:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case PixelFormat::rgba32Float:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case PixelFormat::depth32Float:
        return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkCullModeFlags toVkCullMode(CullModeEx mode) {
    switch (mode) {
    case CullModeEx::none:
        return VK_CULL_MODE_NONE;
    case CullModeEx::front:
        return VK_CULL_MODE_FRONT_BIT;
    case CullModeEx::back:
        return VK_CULL_MODE_BACK_BIT;
    }
    throw std::invalid_argument("unknown native cull mode");
}

VkFrontFace toVkFrontFace(FrontFaceEx frontFace) {
    switch (frontFace) {
    case FrontFaceEx::counterClockwise:
        return VK_FRONT_FACE_COUNTER_CLOCKWISE;
    case FrontFaceEx::clockwise:
        return VK_FRONT_FACE_CLOCKWISE;
    }
    throw std::invalid_argument("unknown native front-face mode");
}

VkCompareOp toVkCompareOp(CompareOpEx compare) {
    switch (compare) {
    case CompareOpEx::never:
        return VK_COMPARE_OP_NEVER;
    case CompareOpEx::less:
        return VK_COMPARE_OP_LESS;
    case CompareOpEx::equal:
        return VK_COMPARE_OP_EQUAL;
    case CompareOpEx::lessOrEqual:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOpEx::greater:
        return VK_COMPARE_OP_GREATER;
    case CompareOpEx::notEqual:
        return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOpEx::greaterOrEqual:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOpEx::always:
        return VK_COMPARE_OP_ALWAYS;
    }
    throw std::invalid_argument("unknown native depth compare operation");
}

VkBlendFactor toVkBlendFactor(BlendFactorEx factor) {
    switch (factor) {
    case BlendFactorEx::zero:
        return VK_BLEND_FACTOR_ZERO;
    case BlendFactorEx::one:
        return VK_BLEND_FACTOR_ONE;
    case BlendFactorEx::srcColor:
        return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactorEx::oneMinusSrcColor:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactorEx::dstColor:
        return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactorEx::oneMinusDstColor:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactorEx::srcAlpha:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactorEx::oneMinusSrcAlpha:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactorEx::dstAlpha:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactorEx::oneMinusDstAlpha:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactorEx::srcAlphaSaturate:
        return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }
    throw std::invalid_argument("unknown native blend factor");
}

VkBlendOp toVkBlendOp(BlendOpEx operation) {
    switch (operation) {
    case BlendOpEx::add:
        return VK_BLEND_OP_ADD;
    case BlendOpEx::subtract:
        return VK_BLEND_OP_SUBTRACT;
    case BlendOpEx::reverseSubtract:
        return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOpEx::min:
        return VK_BLEND_OP_MIN;
    case BlendOpEx::max:
        return VK_BLEND_OP_MAX;
    }
    throw std::invalid_argument("unknown native blend operation");
}

VkImageUsageFlags toVkUsage(ResourceUsage usage) {
    VkImageUsageFlags flags = 0;
    const auto bits = toBits(usage);
    if ((bits & toBits(ResourceUsage::sampledRead)) != 0U)
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((bits & (toBits(ResourceUsage::storageRead) | toBits(ResourceUsage::storageWrite) |
                 toBits(ResourceUsage::storageReadWrite))) != 0U)
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if ((bits & toBits(ResourceUsage::colorAttachment)) != 0U)
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((bits & (toBits(ResourceUsage::depthRead) | toBits(ResourceUsage::depthWrite))) != 0U)
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((bits & toBits(ResourceUsage::transferSrc)) != 0U)
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((bits & toBits(ResourceUsage::transferDst)) != 0U)
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return flags;
}

VkBufferUsageFlags toVkUsage(ResourceUsage usage, bool bufferDeviceAddress) {
    VkBufferUsageFlags flags = 0;
    const auto bits = toBits(usage);
    if ((bits & toBits(ResourceUsage::uniformRead)) != 0U)
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if ((bits & (toBits(ResourceUsage::storageRead) | toBits(ResourceUsage::storageWrite) |
                 toBits(ResourceUsage::storageReadWrite))) != 0U)
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if ((bits & toBits(ResourceUsage::vertexRead)) != 0U)
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if ((bits & toBits(ResourceUsage::indexRead)) != 0U)
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if ((bits & toBits(ResourceUsage::indirectRead)) != 0U)
        flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if ((bits & toBits(ResourceUsage::transferSrc)) != 0U)
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if ((bits & toBits(ResourceUsage::transferDst)) != 0U)
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if ((bits & toBits(ResourceUsage::asBuildRead)) != 0U)
        flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    if ((bits & toBits(ResourceUsage::asBuildWrite)) != 0U)
        flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
    if (bufferDeviceAddress)
        flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    return flags;
}

VkImageType toVkImageType(TextureDimension dimension) {
    switch (dimension) {
    case TextureDimension::d1:
        return VK_IMAGE_TYPE_1D;
    case TextureDimension::d2:
    case TextureDimension::cube:
        return VK_IMAGE_TYPE_2D;
    case TextureDimension::d3:
        return VK_IMAGE_TYPE_3D;
    }
    return VK_IMAGE_TYPE_2D;
}

VkImageViewType toVkImageViewType(TextureDimension dimension, std::uint32_t arrayLayers) {
    switch (dimension) {
    case TextureDimension::d1:
        return arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case TextureDimension::d2:
        return arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    case TextureDimension::d3:
        return VK_IMAGE_VIEW_TYPE_3D;
    case TextureDimension::cube:
        return arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}

std::uint32_t imageLayerCount(const TextureResourceDesc& desc) noexcept {
    return desc.dimension == TextureDimension::cube ? desc.arrayLayers * 6U : desc.arrayLayers;
}

VkImageAspectFlags imageAspect(PixelFormat format) noexcept {
    return format == PixelFormat::depth32Float ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

VkExtent3D mipExtent(const TextureResourceDesc& desc, std::uint32_t mipLevel) noexcept {
    VkExtent3D extent{desc.extent.width, desc.extent.height, desc.extent.depth};
    for (std::uint32_t level = 0; level < mipLevel; ++level) {
        extent.width = extent.width > 1U ? extent.width / 2U : 1U;
        extent.height = extent.height > 1U ? extent.height / 2U : 1U;
        extent.depth = extent.depth > 1U ? extent.depth / 2U : 1U;
    }
    return extent;
}

std::size_t mipBytes(const TextureResourceDesc& desc, std::uint32_t mipLevel) {
    const auto extent = mipExtent(desc, mipLevel);
    auto bytes = checkedResourceMul(static_cast<std::size_t>(extent.width), static_cast<std::size_t>(extent.height));
    bytes = checkedResourceMul(bytes, static_cast<std::size_t>(extent.depth));
    return checkedResourceMul(bytes, pixelFormatByteSize(desc.format));
}

VkImageLayout layoutForUsage(ResourceUsage usage) noexcept {
    const auto bits = toBits(usage);
    const auto storageBits = toBits(ResourceUsage::storageRead) | toBits(ResourceUsage::storageWrite) |
                             toBits(ResourceUsage::storageReadWrite);
    if ((bits & storageBits) != 0U)
        return VK_IMAGE_LAYOUT_GENERAL;
    if ((bits & toBits(ResourceUsage::sampledRead)) != 0U)
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if ((bits & toBits(ResourceUsage::colorAttachment)) != 0U)
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if ((bits & (toBits(ResourceUsage::depthRead) | toBits(ResourceUsage::depthWrite))) != 0U)
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if ((bits & toBits(ResourceUsage::transferDst)) != 0U)
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if ((bits & toBits(ResourceUsage::transferSrc)) != 0U)
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VkShaderStageFlags toVkShaderStages(ShaderStageMask stages) {
    const auto bits = static_cast<std::uint32_t>(stages);
    VkShaderStageFlags flags = 0;
    if ((bits & static_cast<std::uint32_t>(ShaderStageMask::vertex)) != 0U)
        flags |= VK_SHADER_STAGE_VERTEX_BIT;
    if ((bits & static_cast<std::uint32_t>(ShaderStageMask::fragment)) != 0U)
        flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if ((bits & static_cast<std::uint32_t>(ShaderStageMask::compute)) != 0U)
        flags |= VK_SHADER_STAGE_COMPUTE_BIT;
    if ((bits & static_cast<std::uint32_t>(ShaderStageMask::rayGeneration)) != 0U)
        flags |= VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    if ((bits & static_cast<std::uint32_t>(ShaderStageMask::miss)) != 0U)
        flags |= VK_SHADER_STAGE_MISS_BIT_KHR;
    if ((bits & static_cast<std::uint32_t>(ShaderStageMask::closestHit)) != 0U)
        flags |= VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    if ((bits & static_cast<std::uint32_t>(ShaderStageMask::anyHit)) != 0U)
        flags |= VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    if ((bits & static_cast<std::uint32_t>(ShaderStageMask::intersection)) != 0U)
        flags |= VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
    if ((bits & static_cast<std::uint32_t>(ShaderStageMask::callable)) != 0U)
        flags |= VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    return flags;
}

VkShaderStageFlagBits toVkShaderStage(ShaderStageMask stage) {
    switch (stage) {
    case ShaderStageMask::vertex:
        return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderStageMask::fragment:
        return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderStageMask::compute:
        return VK_SHADER_STAGE_COMPUTE_BIT;
    case ShaderStageMask::rayGeneration:
        return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    case ShaderStageMask::miss:
        return VK_SHADER_STAGE_MISS_BIT_KHR;
    case ShaderStageMask::closestHit:
        return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    case ShaderStageMask::anyHit:
        return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    case ShaderStageMask::intersection:
        return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
    case ShaderStageMask::callable:
        return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    case ShaderStageMask::none:
        break;
    }
    throw std::invalid_argument("shader descriptor has no single shader stage");
}

VkDeviceSize alignDeviceAddress(VkDeviceSize value, VkDeviceSize alignment) {
    if (alignment <= 1)
        return value;
    const VkDeviceSize remainder = value % alignment;
    if (remainder == 0)
        return value;
    const VkDeviceSize delta = alignment - remainder;
    if (value > std::numeric_limits<VkDeviceSize>::max() - delta)
        throw std::overflow_error("Vulkan address alignment overflow");
    return value + delta;
}

} // namespace

VulkanDevice::VulkanDevice(platform::Window& window, bool validation)
    : window_(window), validation_(validation), accelerationBackend_(*this) {
    createInstance(validation);
    createSurface();
    selectPhysicalDevice();
    queryCapabilities();
    createLogicalDevice();
    createTypedDescriptorPool();
    createPipelineCache();
    uploadContext_ = std::make_unique<VulkanUploadContext>(device_, physicalDevice_, queue_, queueFamily_,
                                                           timelineSemaphore_, nextTimelineValue_);
    createSwapchain();
    createPreviewDescriptors();
    createPipeline();
    createNativeOutputPipeline();
    createNativeDeformPipeline();
    createNativeEnvironmentPipelines();
    createFrames();
    createUi();
    const std::array<PreviewVertex, 3> fallbackVertices{{
        {{0.0F, -0.65F, 0.0F}, {}, {}},
        {{0.65F, 0.55F, 0.0F}, {}, {}},
        {{-0.65F, 0.55F, 0.0F}, {}, {}},
    }};
    const std::array<std::uint32_t, 3> fallbackIndices{0, 1, 2};
    uploadPreviewMesh(fallbackVertices, fallbackIndices);
    const std::array<PreviewBoneTransform, 1> identityBones{};
    updatePreviewBones(identityBones);
    uploadPreviewMorphDeltas(std::span<const PreviewMorphDelta>{});
    updatePreviewMorphWeights(std::span<const float>{});
    uploadPreviewTextures(std::span<const PreviewTexture>{});
    updatePreviewMaterials(std::span<const PreviewMaterial>{});
    log::info("Vulkan device ready: ", capabilities_.gpuName, " (", capabilities_.driverName, ")");
}

VulkanDevice::~VulkanDevice() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        reclaimAllAccelerationScratch();
    }
    uploadContext_.reset();
    destroyNativeEnvironmentPipelines();
    destroyNativeDeformPipeline();
    destroyTypedResources();
    destroyViewportResources();
    destroyUi();
    destroyOffscreenResource();
    destroyPreviewMesh();
    destroyPreviewBones();
    destroyPreviewMorphs();
    destroyPreviewBackground();
    destroyPreviewTextures();
    for (const auto& [handle, resource] : textures_) {
        static_cast<void>(handle);
#if DAYO_ENABLE_VMA
        if (allocator_ != VK_NULL_HANDLE && resource.allocation != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_, resource.image, resource.allocation);
            continue;
        }
#endif
        if (resource.image != VK_NULL_HANDLE)
            vkDestroyImage(device_, resource.image, nullptr);
        if (resource.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, resource.memory, nullptr);
    }
    for (const auto& [handle, resource] : buffers_) {
        static_cast<void>(handle);
#if DAYO_ENABLE_VMA
        if (allocator_ != VK_NULL_HANDLE && resource.allocation != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator_, resource.buffer, resource.allocation);
            continue;
        }
#endif
        if (resource.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, resource.buffer, nullptr);
        if (resource.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, resource.memory, nullptr);
    }
    destroyFrames();
    destroyNativeOutputPipeline();
    destroyPipeline();
    savePipelineCache();
    destroyPipelineCache();
    destroyPreviewDescriptors();
    destroySwapchain();
    if (timelineSemaphore_ != VK_NULL_HANDLE)
        vkDestroySemaphore(device_, timelineSemaphore_, nullptr);
#if DAYO_ENABLE_VMA
    if (allocator_ != VK_NULL_HANDLE)
        vmaDestroyAllocator(allocator_);
#endif
    if (device_ != VK_NULL_HANDLE)
        vkDestroyDevice(device_, nullptr);
    if (surface_ != VK_NULL_HANDLE)
        SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
    if (debugMessenger_ != VK_NULL_HANDLE) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy != nullptr)
            destroy(instance_, debugMessenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE)
        vkDestroyInstance(instance_, nullptr);
}

void VulkanDevice::createInstance(bool validation) {
    std::uint32_t layerCount = 0;
    check(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "enumerate instance layers");
    std::vector<VkLayerProperties> layers(layerCount);
    check(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()), "enumerate instance layers");

    std::vector<const char*> enabledLayers;
    if (validation && hasLayer(layers, "VK_LAYER_KHRONOS_validation")) {
        enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
    } else if (validation) {
        validation_ = false;
        log::warn("VK_LAYER_KHRONOS_validation is unavailable; continuing without validation");
    }

    std::uint32_t extensionCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    if (sdlExtensions == nullptr) {
        throw std::runtime_error(std::string("SDL Vulkan extensions failed: ") + SDL_GetError());
    }
    std::vector<const char*> extensions(sdlExtensions, sdlExtensions + extensionCount);
    if (validation_)
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    const VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "mikumikudesu",
        .applicationVersion = VK_MAKE_API_VERSION(0, 1, 20, 0),
        .pEngineName = "dayo-native",
        .engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = static_cast<std::uint32_t>(enabledLayers.size()),
        .ppEnabledLayerNames = enabledLayers.data(),
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    check(vkCreateInstance(&createInfo, nullptr, &instance_), "create Vulkan instance");

    if (validation_) {
        const VkDebugUtilsMessengerCreateInfoEXT debugInfo{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debugCallback,
        };
        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create != nullptr)
            check(create(instance_, &debugInfo, nullptr, &debugMessenger_), "create Vulkan debug messenger");
    }
}

void VulkanDevice::createSurface() {
    if (!SDL_Vulkan_CreateSurface(window_.sdlHandle(), instance_, nullptr, &surface_)) {
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
    }
}

void VulkanDevice::selectPhysicalDevice() {
    std::uint32_t deviceCount = 0;
    check(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr), "enumerate physical devices");
    if (deviceCount == 0)
        throw std::runtime_error("no Vulkan physical device found");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    check(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()), "enumerate physical devices");

    int bestScore = -1;
    for (const auto candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3)
            continue;

        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        for (std::uint32_t i = 0; i < familyCount; ++i) {
            VkBool32 present = VK_FALSE;
            check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &present), "query surface support");
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 || present == VK_FALSE)
                continue;
            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 1000
                              : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 500
                                                                                                : 0;
            if (score > bestScore) {
                bestScore = score;
                physicalDevice_ = candidate;
                physicalProperties_ = properties;
                queueFamily_ = i;
                timestampValidBits_ = families[i].timestampValidBits;
            }
        }
    }
    if (physicalDevice_ == VK_NULL_HANDLE) {
        throw std::runtime_error("no Vulkan 1.3 graphics/present device found");
    }
}

void VulkanDevice::queryCapabilities() {
    std::uint32_t extensionCount = 0;
    check(vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount, nullptr),
          "enumerate device extensions");
    std::vector<VkExtensionProperties> extensions(extensionCount);
    check(vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount, extensions.data()),
          "enumerate device extensions");

    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR,
    };
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
        .pNext = &barycentric,
    };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayPipeline{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .pNext = &rayQuery,
    };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &rayPipeline,
    };
    VkPhysicalDeviceVulkan12Features vulkan12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &acceleration,
    };
    VkPhysicalDeviceFeatures2 features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vulkan12,
    };
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &features);

    rayTracingPipelineProperties_ = {};
    rayTracingPipelineProperties_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceDriverProperties driver{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
        .pNext = &rayTracingPipelineProperties_,
    };
    rayTracingPipelineProperties_.pNext = nullptr;
    VkPhysicalDeviceProperties2 properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &driver,
    };
    vkGetPhysicalDeviceProperties2(physicalDevice_, &properties);

    capabilities_.gpuName = physicalProperties_.deviceName;
    capabilities_.driverName = driver.driverName[0] == '\0' ? "unknown" : driver.driverName;
    capabilities_.vendorId = physicalProperties_.vendorID;
    capabilities_.apiVersion = physicalProperties_.apiVersion;
    capabilities_.discreteGpu = physicalProperties_.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    capabilities_.swapchain = hasName(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    capabilities_.timelineSemaphore = vulkan12.timelineSemaphore == VK_TRUE;
    capabilities_.bufferDeviceAddress = vulkan12.bufferDeviceAddress == VK_TRUE;
    capabilities_.descriptorIndexing =
        vulkan12.runtimeDescriptorArray == VK_TRUE && vulkan12.descriptorBindingPartiallyBound == VK_TRUE;
    previewBindlessSupported_ = vulkan12.runtimeDescriptorArray == VK_TRUE &&
                                vulkan12.descriptorBindingVariableDescriptorCount == VK_TRUE &&
                                features.features.shaderSampledImageArrayDynamicIndexing == VK_TRUE;
    capabilities_.accelerationStructure = acceleration.accelerationStructure == VK_TRUE &&
                                          hasName(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    capabilities_.rayTracingPipeline =
        rayPipeline.rayTracingPipeline == VK_TRUE && hasName(extensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    capabilities_.rayQuery = rayQuery.rayQuery == VK_TRUE && hasName(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    capabilities_.fragmentShaderBarycentric = barycentric.fragmentShaderBarycentric == VK_TRUE &&
                                              hasName(extensions, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);

    if (!capabilities_.swapchain)
        throw std::runtime_error("selected GPU lacks VK_KHR_swapchain");
    if (!capabilities_.timelineSemaphore)
        throw std::runtime_error("selected GPU lacks timelineSemaphore");
}

void VulkanDevice::createLogicalDevice() {
    const float priority = 1.0F;
    const VkDeviceQueueCreateInfo queueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queueFamily_,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };

    std::vector<const char*> extensions{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    };
    const bool rtBase = capabilities_.accelerationStructure && capabilities_.bufferDeviceAddress;
    if (rtBase) {
        extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    }
    if (capabilities_.rayTracingPipeline)
        extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    if (capabilities_.rayQuery)
        extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    if (capabilities_.fragmentShaderBarycentric) {
        extensions.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
    }

    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR,
        .fragmentShaderBarycentric = capabilities_.fragmentShaderBarycentric,
    };
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
        .pNext = &barycentric,
        .rayQuery = capabilities_.rayQuery,
    };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayPipeline{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .pNext = &rayQuery,
        .rayTracingPipeline = capabilities_.rayTracingPipeline,
    };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &rayPipeline,
        .accelerationStructure = capabilities_.accelerationStructure,
    };
    VkPhysicalDeviceVulkan13Features vulkan13{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &acceleration,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features vulkan12{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &vulkan13,
        .descriptorIndexing = capabilities_.descriptorIndexing || previewBindlessSupported_,
        .descriptorBindingPartiallyBound = capabilities_.descriptorIndexing,
        .descriptorBindingVariableDescriptorCount = previewBindlessSupported_,
        .runtimeDescriptorArray = capabilities_.descriptorIndexing || previewBindlessSupported_,
        .timelineSemaphore = capabilities_.timelineSemaphore,
        .bufferDeviceAddress = capabilities_.bufferDeviceAddress,
    };
    VkPhysicalDeviceFeatures coreFeatures{};
    coreFeatures.shaderSampledImageArrayDynamicIndexing = previewBindlessSupported_;
    const VkPhysicalDeviceFeatures2 features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vulkan12,
        .features = coreFeatures,
    };
    const VkDeviceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    check(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "create logical device");
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    const VkSemaphoreTypeCreateInfo timelineType{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    const VkSemaphoreCreateInfo timelineInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timelineType,
    };
    check(vkCreateSemaphore(device_, &timelineInfo, nullptr, &timelineSemaphore_), "create Vulkan timeline semaphore");
#if DAYO_ENABLE_VMA
    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.instance = instance_;
    allocatorInfo.physicalDevice = physicalDevice_;
    allocatorInfo.device = device_;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    if (capabilities_.bufferDeviceAddress)
        allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    check(vmaCreateAllocator(&allocatorInfo, &allocator_), "create Vulkan memory allocator");
#endif
}

void VulkanDevice::createSwapchain() {
    VkSurfaceCapabilitiesKHR surfaceCapabilities{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &surfaceCapabilities),
          "query surface capabilities");
    std::uint32_t formatCount = 0;
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr),
          "query surface formats");
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data()),
          "query surface formats");
    if (formats.empty())
        throw std::runtime_error("surface exposes no Vulkan formats");

    VkSurfaceFormatKHR selected = formats.front();
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            selected = format;
            break;
        }
    }
    swapchainFormat_ = selected.format;
    if (surfaceCapabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        swapchainExtent_ = surfaceCapabilities.currentExtent;
    } else {
        swapchainExtent_.width = std::clamp(window_.pixelWidth(), surfaceCapabilities.minImageExtent.width,
                                            surfaceCapabilities.maxImageExtent.width);
        swapchainExtent_.height = std::clamp(window_.pixelHeight(), surfaceCapabilities.minImageExtent.height,
                                             surfaceCapabilities.maxImageExtent.height);
    }

    std::uint32_t imageCount = std::max(surfaceCapabilities.minImageCount, 2U);
    if (surfaceCapabilities.maxImageCount != 0) {
        imageCount = std::min(imageCount, surfaceCapabilities.maxImageCount);
    }
    const VkSwapchainCreateInfoKHR createInfo{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface_,
        .minImageCount = imageCount,
        .imageFormat = selected.format,
        .imageColorSpace = selected.colorSpace,
        .imageExtent = swapchainExtent_,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = surfaceCapabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    check(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_), "create swapchain");
    check(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr), "get swapchain images");
    swapchainImages_.resize(imageCount);
    check(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data()), "get swapchain images");
    swapchainViews_.resize(imageCount);
    swapchainInitialized_.assign(imageCount, false);
    for (std::size_t i = 0; i < swapchainImages_.size(); ++i) {
        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = swapchainImages_[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = swapchainFormat_,
            .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        check(vkCreateImageView(device_, &viewInfo, nullptr, &swapchainViews_[i]), "create swapchain image view");
    }
    swapchainDepth_.resize(imageCount);
    for (auto& depth : swapchainDepth_) {
        const VkImageCreateInfo depthInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_D32_SFLOAT,
            .extent = {swapchainExtent_.width, swapchainExtent_.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        check(vkCreateImage(device_, &depthInfo, nullptr, &depth.image), "create swapchain depth image");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, depth.image, &requirements);
        const VkMemoryAllocateInfo allocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };
        check(vkAllocateMemory(device_, &allocation, nullptr, &depth.memory), "allocate swapchain depth image");
        check(vkBindImageMemory(device_, depth.image, depth.memory, 0), "bind swapchain depth image");
        const VkImageViewCreateInfo depthView{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = depth.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_D32_SFLOAT,
            .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
        };
        check(vkCreateImageView(device_, &depthView, nullptr, &depth.view), "create swapchain depth view");
    }
}

void VulkanDevice::destroySwapchain() {
    for (const auto& depth : swapchainDepth_) {
        if (depth.view != VK_NULL_HANDLE)
            vkDestroyImageView(device_, depth.view, nullptr);
        if (depth.image != VK_NULL_HANDLE)
            vkDestroyImage(device_, depth.image, nullptr);
        if (depth.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, depth.memory, nullptr);
    }
    swapchainDepth_.clear();
    for (const auto view : swapchainViews_)
        vkDestroyImageView(device_, view, nullptr);
    swapchainViews_.clear();
    swapchainImages_.clear();
    swapchainInitialized_.clear();
    if (swapchain_ != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void VulkanDevice::createPipelineCache() {
    constexpr std::size_t maxCacheBytes = 64U * 1024U * 1024U;
    std::vector<std::byte> initialData;
    std::ifstream input("pipeline.cache", std::ios::binary | std::ios::ate);
    if (input) {
        const auto end = input.tellg();
        if (end > 0 && static_cast<std::uintmax_t>(end) <= maxCacheBytes) {
            initialData.resize(static_cast<std::size_t>(end));
            input.seekg(0);
            input.read(reinterpret_cast<char*>(initialData.data()), static_cast<std::streamsize>(initialData.size()));
            if (!input)
                initialData.clear();
        }
    }

    VkPipelineCacheCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = initialData.size(),
        .pInitialData = initialData.data(),
    };
    auto result = vkCreatePipelineCache(device_, &createInfo, nullptr, &pipelineCache_);
    if (result != VK_SUCCESS && !initialData.empty()) {
        log::warn("Ignoring incompatible Vulkan pipeline cache");
        createInfo.initialDataSize = 0;
        createInfo.pInitialData = nullptr;
        result = vkCreatePipelineCache(device_, &createInfo, nullptr, &pipelineCache_);
    }
    check(result, "create Vulkan pipeline cache");
}

void VulkanDevice::savePipelineCache() const noexcept {
    if (pipelineCache_ == VK_NULL_HANDLE)
        return;
    std::size_t size = 0;
    if (vkGetPipelineCacheData(device_, pipelineCache_, &size, nullptr) != VK_SUCCESS || size == 0)
        return;
    std::vector<std::byte> data(size);
    if (vkGetPipelineCacheData(device_, pipelineCache_, &size, data.data()) != VK_SUCCESS)
        return;
    std::ofstream output("pipeline.cache", std::ios::binary | std::ios::trunc);
    if (!output)
        return;
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(size));
}

void VulkanDevice::destroyPipelineCache() {
    if (pipelineCache_ != VK_NULL_HANDLE)
        vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
    pipelineCache_ = VK_NULL_HANDLE;
}

void VulkanDevice::createPipeline() {
    const auto vertexCode = readBinary(DAYO_PREVIEW_VERTEX_SPV);
    const auto edgeVertexCode = readBinary(DAYO_PREVIEW_EDGE_VERTEX_SPV);
    const auto fragmentCode = readBinary(DAYO_PREVIEW_FRAGMENT_SPV);
    const VkShaderModuleCreateInfo vertexInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vertexCode.size(),
        .pCode = reinterpret_cast<const std::uint32_t*>(vertexCode.data()),
    };
    const VkShaderModuleCreateInfo fragmentInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fragmentCode.size(),
        .pCode = reinterpret_cast<const std::uint32_t*>(fragmentCode.data()),
    };
    const VkShaderModuleCreateInfo edgeVertexInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = edgeVertexCode.size(),
        .pCode = reinterpret_cast<const std::uint32_t*>(edgeVertexCode.data()),
    };
    VkShaderModule vertex{};
    VkShaderModule edgeVertex{};
    VkShaderModule fragment{};
    check(vkCreateShaderModule(device_, &vertexInfo, nullptr, &vertex), "create vertex shader");
    try {
        check(vkCreateShaderModule(device_, &edgeVertexInfo, nullptr, &edgeVertex), "create edge vertex shader");
        check(vkCreateShaderModule(device_, &fragmentInfo, nullptr, &fragment), "create fragment shader");
    } catch (...) {
        if (edgeVertex != VK_NULL_HANDLE)
            vkDestroyShaderModule(device_, edgeVertex, nullptr);
        vkDestroyShaderModule(device_, vertex, nullptr);
        throw;
    }

    const std::array stages{
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertex,
            .pName = "VS",
        },
        VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment,
            .pName = "PS",
        },
    };
    auto edgeStages = stages;
    edgeStages[0].module = edgeVertex;
    edgeStages[0].pName = "EdgeVS";
    const VkVertexInputBindingDescription vertexBinding{
        .binding = 0,
        .stride = sizeof(PreviewVertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    const std::array vertexAttributes{
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PreviewVertex, position)},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PreviewVertex, normal)},
        VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(PreviewVertex, uv)},
        VkVertexInputAttributeDescription{3, 0, VK_FORMAT_R32G32B32A32_SINT, offsetof(PreviewVertex, bones)},
        VkVertexInputAttributeDescription{4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PreviewVertex, weights)},
        VkVertexInputAttributeDescription{5, 0, VK_FORMAT_R32_UINT, offsetof(PreviewVertex, skinningType)},
        VkVertexInputAttributeDescription{6, 0, VK_FORMAT_R32_UINT, offsetof(PreviewVertex, gpuSkinning)},
        VkVertexInputAttributeDescription{7, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PreviewVertex, sdefC)},
        VkVertexInputAttributeDescription{8, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PreviewVertex, sdefHalfDelta)},
        VkVertexInputAttributeDescription{9, 0, VK_FORMAT_R32_SFLOAT, offsetof(PreviewVertex, edgeScale)},
        VkVertexInputAttributeDescription{10, 0, VK_FORMAT_R32_UINT, offsetof(PreviewVertex, morphStart)},
        VkVertexInputAttributeDescription{11, 0, VK_FORMAT_R32_UINT, offsetof(PreviewVertex, morphCount)},
    };
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vertexBinding,
        .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(vertexAttributes.size()),
        .pVertexAttributeDescriptions = vertexAttributes.data(),
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo rasterizer{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        // preview.hlsl flips clip-space Y, which reverses the projected winding.
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0F,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState opaqueBlendAttachment{
        .blendEnable = VK_FALSE,
        .colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendAttachmentState transparentBlendAttachment{
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo opaqueBlend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &opaqueBlendAttachment,
    };
    const VkPipelineColorBlendStateCreateInfo transparentBlend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &transparentBlendAttachment,
    };
    const VkPipelineDepthStencilStateCreateInfo depthStencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    const VkPipelineDepthStencilStateCreateInfo backgroundDepthStencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_FALSE,
        .depthWriteEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
    };
    const VkPipelineDepthStencilStateCreateInfo edgeDepthStencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
    };
    const VkPipelineDepthStencilStateCreateInfo transparentDepthStencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    const VkPipelineRasterizationStateCreateInfo edgeRasterizer{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_FRONT_BIT,
        // EdgeVS uses the same clip-space Y flip as the main Preview vertex shader.
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0F,
    };
    const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data(),
    };
    const VkPushConstantRange pushConstant{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PreviewPushConstants),
    };
    const std::array descriptorLayouts{
        previewDescriptorSetLayout_,         previewSkinningDescriptorSetLayout_, previewMaterialDescriptorSetLayout_,
        previewBindlessDescriptorSetLayout_, previewMorphDescriptorSetLayout_,
    };
    const VkPipelineLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = static_cast<std::uint32_t>(descriptorLayouts.size()),
        .pSetLayouts = descriptorLayouts.data(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstant,
    };
    check(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_), "create pipeline layout");
    const VkPipelineRenderingCreateInfo renderingInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &swapchainFormat_,
        .depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
    };
    const VkGraphicsPipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &renderingInfo,
        .stageCount = static_cast<std::uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &opaqueBlend,
        .pDynamicState = &dynamic,
        .layout = pipelineLayout_,
    };
    const auto result = vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pipelineInfo, nullptr, &pipeline_);
    auto transparentPipelineInfo = pipelineInfo;
    transparentPipelineInfo.pDepthStencilState = &transparentDepthStencil;
    transparentPipelineInfo.pColorBlendState = &transparentBlend;
    const auto transparentResult =
        vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &transparentPipelineInfo, nullptr, &transparentPipeline_);
    auto backgroundPipelineInfo = pipelineInfo;
    backgroundPipelineInfo.pDepthStencilState = &backgroundDepthStencil;
    const auto backgroundResult =
        vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &backgroundPipelineInfo, nullptr, &backgroundPipeline_);
    auto edgePipelineInfo = pipelineInfo;
    edgePipelineInfo.pStages = edgeStages.data();
    edgePipelineInfo.pDepthStencilState = &edgeDepthStencil;
    edgePipelineInfo.pRasterizationState = &edgeRasterizer;
    edgePipelineInfo.pColorBlendState = &transparentBlend;
    const auto edgeResult =
        vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &edgePipelineInfo, nullptr, &edgePipeline_);
    vkDestroyShaderModule(device_, fragment, nullptr);
    vkDestroyShaderModule(device_, edgeVertex, nullptr);
    vkDestroyShaderModule(device_, vertex, nullptr);
    check(result, "create graphics pipeline");
    check(transparentResult, "create transparent graphics pipeline");
    check(backgroundResult, "create background graphics pipeline");
    check(edgeResult, "create edge graphics pipeline");
}

void VulkanDevice::destroyPipeline() {
    if (edgePipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, edgePipeline_, nullptr);
    if (backgroundPipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, backgroundPipeline_, nullptr);
    if (transparentPipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, transparentPipeline_, nullptr);
    if (pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    backgroundPipeline_ = VK_NULL_HANDLE;
    edgePipeline_ = VK_NULL_HANDLE;
    transparentPipeline_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
}

void VulkanDevice::createNativeOutputPipeline() {
    const auto vertexCode = readBinary(DAYO_NATIVE_OUTPUT_VERTEX_SPV);
    const auto fragmentCode = readBinary(DAYO_NATIVE_OUTPUT_FRAGMENT_SPV);
    const auto createShader = [this](const std::vector<std::byte>& code, VkShaderModule& module) {
        const VkShaderModuleCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = code.size(),
            .pCode = reinterpret_cast<const std::uint32_t*>(code.data()),
        };
        check(vkCreateShaderModule(device_, &info, nullptr, &module), "create native output shader");
    };

    VkShaderModule vertex{};
    VkShaderModule fragment{};
    try {
        createShader(vertexCode, vertex);
        createShader(fragmentCode, fragment);

        const std::array descriptorBindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        };
        const VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = static_cast<std::uint32_t>(descriptorBindings.size()),
            .pBindings = descriptorBindings.data(),
        };
        check(vkCreateDescriptorSetLayout(device_, &descriptorLayoutInfo, nullptr, &nativeOutputDescriptorSetLayout_),
              "create native output descriptor set layout");

        const std::array descriptorPoolSizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 2},
        };
        const VkDescriptorPoolCreateInfo descriptorPoolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = static_cast<std::uint32_t>(nativeOutputDescriptors_.size()),
            .poolSizeCount = static_cast<std::uint32_t>(descriptorPoolSizes.size()),
            .pPoolSizes = descriptorPoolSizes.data(),
        };
        check(vkCreateDescriptorPool(device_, &descriptorPoolInfo, nullptr, &nativeOutputDescriptorPool_),
              "create native output descriptor pool");
        const std::array layouts{nativeOutputDescriptorSetLayout_, nativeOutputDescriptorSetLayout_};
        const VkDescriptorSetAllocateInfo allocateInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = nativeOutputDescriptorPool_,
            .descriptorSetCount = static_cast<std::uint32_t>(layouts.size()),
            .pSetLayouts = layouts.data(),
        };
        check(vkAllocateDescriptorSets(device_, &allocateInfo, nativeOutputDescriptors_.data()),
              "allocate native output descriptor sets");

        const std::array stages{
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                            VK_SHADER_STAGE_VERTEX_BIT, vertex, "VS", nullptr},
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                            VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "PS", nullptr},
        };
        const VkPipelineVertexInputStateCreateInfo vertexInput{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        };
        const VkPipelineInputAssemblyStateCreateInfo assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        const VkPipelineViewportStateCreateInfo viewport{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
        };
        const VkPipelineRasterizationStateCreateInfo rasterization{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1.0F,
        };
        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        const VkPipelineColorBlendAttachmentState blendAttachment{
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                              VK_COLOR_COMPONENT_A_BIT,
        };
        const VkPipelineColorBlendStateCreateInfo blend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &blendAttachment,
        };
        const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        const VkPipelineDynamicStateCreateInfo dynamic{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
            .pDynamicStates = dynamicStates.data(),
        };
        const VkPipelineLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &nativeOutputDescriptorSetLayout_,
        };
        check(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &nativeOutputPipelineLayout_),
              "create native output pipeline layout");
        const VkPipelineRenderingCreateInfo rendering{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &swapchainFormat_,
        };
        const VkGraphicsPipelineCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &rendering,
            .stageCount = static_cast<std::uint32_t>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &assembly,
            .pViewportState = &viewport,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pColorBlendState = &blend,
            .pDynamicState = &dynamic,
            .layout = nativeOutputPipelineLayout_,
        };
        check(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &createInfo, nullptr, &nativeOutputPipeline_),
              "create native output pipeline");
    } catch (...) {
        if (fragment != VK_NULL_HANDLE)
            vkDestroyShaderModule(device_, fragment, nullptr);
        if (vertex != VK_NULL_HANDLE)
            vkDestroyShaderModule(device_, vertex, nullptr);
        destroyNativeOutputPipeline();
        throw;
    }
    vkDestroyShaderModule(device_, fragment, nullptr);
    vkDestroyShaderModule(device_, vertex, nullptr);

    const auto words = std::span<const std::uint32_t>(reinterpret_cast<const std::uint32_t*>(vertexCode.data()),
                                                      vertexCode.size() / sizeof(std::uint32_t));
    try {
        nativeFullscreenVertexShader_ =
            createShaderEx({.spirv = words, .entryPoint = "VS", .stage = ShaderStageMask::vertex});
    } catch (...) {
        destroyNativeOutputPipeline();
        throw;
    }
}

void VulkanDevice::destroyNativeOutputPipeline() noexcept {
    if (nativeOutputPipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, nativeOutputPipeline_, nullptr);
    if (nativeOutputPipelineLayout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, nativeOutputPipelineLayout_, nullptr);
    if (nativeOutputDescriptorPool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device_, nativeOutputDescriptorPool_, nullptr);
    if (nativeOutputDescriptorSetLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, nativeOutputDescriptorSetLayout_, nullptr);
    nativeOutputPipeline_ = VK_NULL_HANDLE;
    nativeOutputPipelineLayout_ = VK_NULL_HANDLE;
    nativeOutputDescriptorPool_ = VK_NULL_HANDLE;
    nativeOutputDescriptorSetLayout_ = VK_NULL_HANDLE;
    nativeOutputDescriptors_.fill(VK_NULL_HANDLE);
}

void VulkanDevice::createNativeDeformPipeline() {
    const auto code = readBinary(DAYO_NATIVE_DEFORM_SPV);
    try {
        nativeDeformDescriptorLayout_ = createDescriptorSetLayoutEx(graphics::nativeDeformDescriptorLayout());
        nativeDeformPipelineLayout_ = createPipelineLayoutEx(nativeDeformPipelineLayout(nativeDeformDescriptorLayout_));
        const auto words = std::span<const std::uint32_t>(reinterpret_cast<const std::uint32_t*>(code.data()),
                                                          code.size() / sizeof(std::uint32_t));
        nativeDeformShader_ =
            createShaderEx({.spirv = words, .entryPoint = "NativeDeform", .stage = ShaderStageMask::compute});
        nativeDeformPipeline_ =
            createComputePipelineEx({.layout = nativeDeformPipelineLayout_, .shaders = {nativeDeformShader_}});
    } catch (...) {
        destroyNativeDeformPipeline();
        throw;
    }
}

void VulkanDevice::destroyNativeDeformPipeline() noexcept {
    if (nativeDeformPipeline_.valid()) {
        try {
            destroyPipelineEx(nativeDeformPipeline_);
        } catch (...) {
        }
    }
    if (nativeDeformShader_.valid()) {
        try {
            destroyShaderEx(nativeDeformShader_);
        } catch (...) {
        }
    }
    if (nativeDeformPipelineLayout_.valid()) {
        try {
            destroyPipelineLayoutEx(nativeDeformPipelineLayout_);
        } catch (...) {
        }
    }
    if (nativeDeformDescriptorLayout_.valid()) {
        try {
            destroyDescriptorSetLayoutEx(nativeDeformDescriptorLayout_);
        } catch (...) {
        }
    }
    nativeDeformPipeline_ = {};
    nativeDeformShader_ = {};
    nativeDeformPipelineLayout_ = {};
    nativeDeformDescriptorLayout_ = {};
}

void VulkanDevice::createNativeEnvironmentPipelines() {
    const auto equirectCode = readBinary(DAYO_NATIVE_ENVIRONMENT_EQUIRECT_SPV);
    const auto prefilterCode = readBinary(DAYO_NATIVE_ENVIRONMENT_PREFILTER_SPV);
    try {
        nativeEnvironmentEquirectLayout_ = createDescriptorSetLayoutEx(nativeEnvironmentPassLayout());
        nativeEnvironmentPrefilterLayout_ =
            createDescriptorSetLayoutEx(::dayo::graphics::nativeEnvironmentPrefilterLayout());
        const PipelineLayoutDesc equirectPipelineLayout{
            .setLayouts = {nativeEnvironmentEquirectLayout_},
            .pushConstants = {{ShaderStageMask::compute, 0, sizeof(NativeEnvironmentPushConstants)}},
        };
        const PipelineLayoutDesc prefilterPipelineLayout{
            .setLayouts = {nativeEnvironmentPrefilterLayout_},
            .pushConstants = {{ShaderStageMask::compute, 0, sizeof(NativeEnvironmentPushConstants)}},
        };
        nativeEnvironmentEquirectPipelineLayout_ = createPipelineLayoutEx(equirectPipelineLayout);
        nativeEnvironmentPrefilterPipelineLayout_ = createPipelineLayoutEx(prefilterPipelineLayout);
        const auto equirectWords = std::span<const std::uint32_t>(
            reinterpret_cast<const std::uint32_t*>(equirectCode.data()), equirectCode.size() / sizeof(std::uint32_t));
        const auto prefilterWords = std::span<const std::uint32_t>(
            reinterpret_cast<const std::uint32_t*>(prefilterCode.data()), prefilterCode.size() / sizeof(std::uint32_t));
        nativeEnvironmentEquirectShader_ =
            createShaderEx({.spirv = equirectWords, .entryPoint = "EquirectToCube", .stage = ShaderStageMask::compute});
        nativeEnvironmentPrefilterShader_ =
            createShaderEx({.spirv = prefilterWords, .entryPoint = "PrefilterCube", .stage = ShaderStageMask::compute});
        nativeEnvironmentEquirectPipeline_ = createComputePipelineEx(
            {.layout = nativeEnvironmentEquirectPipelineLayout_, .shaders = {nativeEnvironmentEquirectShader_}});
        nativeEnvironmentPrefilterPipeline_ = createComputePipelineEx(
            {.layout = nativeEnvironmentPrefilterPipelineLayout_, .shaders = {nativeEnvironmentPrefilterShader_}});
    } catch (...) {
        destroyNativeEnvironmentPipelines();
        throw;
    }
}

void VulkanDevice::destroyNativeEnvironmentPipelines() noexcept {
    if (nativeEnvironmentPrefilterPipeline_.valid()) {
        try {
            destroyPipelineEx(nativeEnvironmentPrefilterPipeline_);
        } catch (...) {
        }
    }
    if (nativeEnvironmentEquirectPipeline_.valid()) {
        try {
            destroyPipelineEx(nativeEnvironmentEquirectPipeline_);
        } catch (...) {
        }
    }
    if (nativeEnvironmentPrefilterShader_.valid()) {
        try {
            destroyShaderEx(nativeEnvironmentPrefilterShader_);
        } catch (...) {
        }
    }
    if (nativeEnvironmentEquirectShader_.valid()) {
        try {
            destroyShaderEx(nativeEnvironmentEquirectShader_);
        } catch (...) {
        }
    }
    if (nativeEnvironmentPrefilterPipelineLayout_.valid()) {
        try {
            destroyPipelineLayoutEx(nativeEnvironmentPrefilterPipelineLayout_);
        } catch (...) {
        }
    }
    if (nativeEnvironmentEquirectPipelineLayout_.valid()) {
        try {
            destroyPipelineLayoutEx(nativeEnvironmentEquirectPipelineLayout_);
        } catch (...) {
        }
    }
    if (nativeEnvironmentPrefilterLayout_.valid()) {
        try {
            destroyDescriptorSetLayoutEx(nativeEnvironmentPrefilterLayout_);
        } catch (...) {
        }
    }
    if (nativeEnvironmentEquirectLayout_.valid()) {
        try {
            destroyDescriptorSetLayoutEx(nativeEnvironmentEquirectLayout_);
        } catch (...) {
        }
    }
    nativeEnvironmentPrefilterPipeline_ = {};
    nativeEnvironmentEquirectPipeline_ = {};
    nativeEnvironmentPrefilterShader_ = {};
    nativeEnvironmentEquirectShader_ = {};
    nativeEnvironmentPrefilterPipelineLayout_ = {};
    nativeEnvironmentEquirectPipelineLayout_ = {};
    nativeEnvironmentPrefilterLayout_ = {};
    nativeEnvironmentEquirectLayout_ = {};
}

void VulkanDevice::createPreviewDescriptors() {
    if (!previewBindlessSupported_)
        throw std::runtime_error("preview bindless texture table requires sampled image array indexing");
    constexpr std::uint32_t fixedSampledImageCount = 3;
    const auto maxDescriptorSetSampledImages = physicalProperties_.limits.maxDescriptorSetSampledImages;
    const auto maxPerStageDescriptorSampledImages = physicalProperties_.limits.maxPerStageDescriptorSampledImages;
    if (maxDescriptorSetSampledImages <= fixedSampledImageCount ||
        maxPerStageDescriptorSampledImages <= fixedSampledImageCount)
        throw std::runtime_error("Vulkan device exposes no sampled image descriptors for preview textures");
    previewBindlessTextureCapacity_ = std::min(maxDescriptorSetSampledImages - fixedSampledImageCount,
                                               maxPerStageDescriptorSampledImages - fixedSampledImageCount);

    const std::array textureBindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(textureBindings.size()),
        .pBindings = textureBindings.data(),
    };
    check(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &previewDescriptorSetLayout_),
          "create preview descriptor layout");
    const VkDescriptorSetLayoutBinding skinningBinding{
        0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr,
    };
    const VkDescriptorSetLayoutCreateInfo skinningLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &skinningBinding,
    };
    check(vkCreateDescriptorSetLayout(device_, &skinningLayoutInfo, nullptr, &previewSkinningDescriptorSetLayout_),
          "create preview skinning descriptor layout");
    const VkDescriptorSetLayoutBinding materialBinding{
        0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr,
    };
    const VkDescriptorSetLayoutCreateInfo materialLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &materialBinding,
    };
    check(vkCreateDescriptorSetLayout(device_, &materialLayoutInfo, nullptr, &previewMaterialDescriptorSetLayout_),
          "create preview material descriptor layout");
    const std::array morphBindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo morphLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(morphBindings.size()),
        .pBindings = morphBindings.data(),
    };
    check(vkCreateDescriptorSetLayout(device_, &morphLayoutInfo, nullptr, &previewMorphDescriptorSetLayout_),
          "create preview morph descriptor layout");
    const std::array<VkDescriptorSetLayoutBinding, 3> bindlessBindings{{
        {0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, previewBindlessTextureCapacity_, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    }};
    const std::array<VkDescriptorBindingFlags, 3> bindlessBindingFlags{
        0,
        0,
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT,
    };
    const VkDescriptorSetLayoutBindingFlagsCreateInfo bindlessBindingFlagsInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(bindlessBindingFlags.size()),
        .pBindingFlags = bindlessBindingFlags.data(),
    };
    const VkDescriptorSetLayoutCreateInfo bindlessLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &bindlessBindingFlagsInfo,
        .bindingCount = static_cast<std::uint32_t>(bindlessBindings.size()),
        .pBindings = bindlessBindings.data(),
    };
    check(vkCreateDescriptorSetLayout(device_, &bindlessLayoutInfo, nullptr, &previewBindlessDescriptorSetLayout_),
          "create preview bindless descriptor layout");
    constexpr std::uint32_t legacySampledImagePoolSize = 32768U;
    const auto sampledImagePoolSize = legacySampledImagePoolSize + previewBindlessTextureCapacity_;
    const std::array poolSizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampledImagePoolSize},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 16384},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128},
    };
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 8192,
        .poolSizeCount = static_cast<std::uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data(),
    };
    check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &previewDescriptorPool_),
          "create preview descriptor pool");
    const VkSamplerCreateInfo samplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxAnisotropy = 1.0F,
        .maxLod = 0.0F,
    };
    check(vkCreateSampler(device_, &samplerInfo, nullptr, &previewSampler_), "create preview sampler");
    auto clampSamplerInfo = samplerInfo;
    clampSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    clampSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(device_, &clampSamplerInfo, nullptr, &previewClampSampler_), "create preview clamp sampler");
}

void VulkanDevice::destroyPreviewDescriptors() {
    destroyPreviewMaterialDescriptors();
    destroyPreviewBindlessDescriptor();
    if (previewClampSampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device_, previewClampSampler_, nullptr);
    if (previewSampler_ != VK_NULL_HANDLE)
        vkDestroySampler(device_, previewSampler_, nullptr);
    if (previewDescriptorPool_ != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(device_, previewDescriptorPool_, nullptr);
    if (previewDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, previewDescriptorSetLayout_, nullptr);
    }
    if (previewSkinningDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, previewSkinningDescriptorSetLayout_, nullptr);
    }
    if (previewMaterialDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, previewMaterialDescriptorSetLayout_, nullptr);
    }
    if (previewMorphDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, previewMorphDescriptorSetLayout_, nullptr);
    }
    if (previewBindlessDescriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, previewBindlessDescriptorSetLayout_, nullptr);
    }
    previewClampSampler_ = VK_NULL_HANDLE;
    previewSampler_ = VK_NULL_HANDLE;
    previewDescriptorPool_ = VK_NULL_HANDLE;
    previewDescriptorSetLayout_ = VK_NULL_HANDLE;
    previewSkinningDescriptorSetLayout_ = VK_NULL_HANDLE;
    previewMaterialDescriptorSetLayout_ = VK_NULL_HANDLE;
    previewMorphDescriptorSetLayout_ = VK_NULL_HANDLE;
    previewBindlessDescriptorSetLayout_ = VK_NULL_HANDLE;
    previewBindlessTextureCapacity_ = 0;
}

void VulkanDevice::destroyPreviewTextures() {
    destroyPreviewMaterialDescriptors();
    destroyPreviewBindlessDescriptor();
    for (auto& texture : previewTextures_)
        destroyPreviewTextureResource(texture);
    previewTextures_.clear();
}

void VulkanDevice::createPreviewTexture(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba) {
    previewTextures_.push_back(createPreviewTextureResource(width, height, rgba));
}

void VulkanDevice::destroyPreviewTextureResource(PreviewTextureResource& texture) {
    if (texture.descriptor != VK_NULL_HANDLE && previewDescriptorPool_ != VK_NULL_HANDLE) {
        check(vkFreeDescriptorSets(device_, previewDescriptorPool_, 1, &texture.descriptor),
              "free preview texture descriptor");
    }
    if (texture.view != VK_NULL_HANDLE)
        vkDestroyImageView(device_, texture.view, nullptr);
    if (texture.image != VK_NULL_HANDLE)
        vkDestroyImage(device_, texture.image, nullptr);
    if (texture.memory != VK_NULL_HANDLE)
        vkFreeMemory(device_, texture.memory, nullptr);
    texture = {};
}

VulkanDevice::PreviewTextureResource VulkanDevice::createPreviewTextureResource(std::uint32_t width,
                                                                                std::uint32_t height,
                                                                                std::span<const std::uint8_t> rgba) {
    const auto byteSize = static_cast<VkDeviceSize>(width) * height * 4U;
    if (width == 0 || height == 0 || rgba.size_bytes() != byteSize) {
        throw std::invalid_argument("invalid preview texture data");
    }
    PreviewTextureResource texture;
    std::uint64_t uploadValue = 0;
    try {
        const VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_SRGB,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        check(vkCreateImage(device_, &imageInfo, nullptr, &texture.image), "create preview texture");
        VkMemoryRequirements imageRequirements{};
        vkGetImageMemoryRequirements(device_, texture.image, &imageRequirements);
        const VkMemoryAllocateInfo imageAllocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = imageRequirements.size,
            .memoryTypeIndex = findMemoryType(imageRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };
        check(vkAllocateMemory(device_, &imageAllocation, nullptr, &texture.memory), "allocate preview texture");
        check(vkBindImageMemory(device_, texture.image, texture.memory, 0), "bind preview texture");

        uploadContext_->begin();
        const auto slice = uploadContext_->allocate(byteSize, 4);
        std::memcpy(slice.mapped, rgba.data(), rgba.size_bytes());
        const auto command = uploadContext_->commandBuffer();
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkImageMemoryBarrier2 toTransfer{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture.image,
            .subresourceRange = range,
        };
        const VkDependencyInfo transferDependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &toTransfer,
        };
        vkCmdPipelineBarrier2(command, &transferDependency);
        const VkBufferImageCopy copy{
            .bufferOffset = slice.offset,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {width, height, 1},
        };
        vkCmdCopyBufferToImage(command, slice.buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        const VkImageMemoryBarrier2 toShader{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture.image,
            .subresourceRange = range,
        };
        const VkDependencyInfo shaderDependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &toShader,
        };
        vkCmdPipelineBarrier2(command, &shaderDependency);
        uploadValue = uploadContext_->submit();

        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = texture.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_SRGB,
            .subresourceRange = range,
        };
        check(vkCreateImageView(device_, &viewInfo, nullptr, &texture.view), "create preview texture view");
        const VkDescriptorSetAllocateInfo setInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = previewDescriptorPool_,
            .descriptorSetCount = 1,
            .pSetLayouts = &previewDescriptorSetLayout_,
        };
        check(vkAllocateDescriptorSets(device_, &setInfo, &texture.descriptor), "allocate preview texture descriptor");
        const VkDescriptorImageInfo descriptorImage{
            .imageView = texture.view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        const VkDescriptorImageInfo descriptorSampler{.sampler = previewSampler_};
        const VkDescriptorImageInfo descriptorClampSampler{.sampler = previewClampSampler_};
        const std::array writes{
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = texture.descriptor,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo = &descriptorImage,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = texture.descriptor,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo = &descriptorImage,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = texture.descriptor,
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo = &descriptorImage,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = texture.descriptor,
                .dstBinding = 3,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .pImageInfo = &descriptorSampler,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = texture.descriptor,
                .dstBinding = 4,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .pImageInfo = &descriptorClampSampler,
            },
        };
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        return texture;
    } catch (...) {
        uploadContext_->abort();
        if (uploadValue != 0)
            uploadContext_->wait(uploadValue);
        destroyPreviewTextureResource(texture);
        throw;
    }
}

VulkanDevice::PreviewTextureResource VulkanDevice::createEmptyPreviewTextureResource(std::uint32_t width,
                                                                                     std::uint32_t height) {
    PreviewTextureResource texture;
    const VkImageCreateInfo imageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    check(vkCreateImage(device_, &imageInfo, nullptr, &texture.image), "create streaming background image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, texture.image, &requirements);
    const VkMemoryAllocateInfo allocation{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    check(vkAllocateMemory(device_, &allocation, nullptr, &texture.memory), "allocate streaming background image");
    check(vkBindImageMemory(device_, texture.image, texture.memory, 0), "bind streaming background image");
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkImageViewCreateInfo viewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_SRGB,
        .subresourceRange = range,
    };
    check(vkCreateImageView(device_, &viewInfo, nullptr, &texture.view), "create streaming background view");
    const VkDescriptorSetAllocateInfo setInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = previewDescriptorPool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &previewDescriptorSetLayout_,
    };
    check(vkAllocateDescriptorSets(device_, &setInfo, &texture.descriptor), "allocate streaming background descriptor");
    const VkDescriptorImageInfo descriptorImage{
        .imageView = texture.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkDescriptorImageInfo descriptorSampler{.sampler = previewSampler_};
    const VkDescriptorImageInfo descriptorClampSampler{.sampler = previewClampSampler_};
    const std::array writes{
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = texture.descriptor,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pImageInfo = &descriptorImage,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = texture.descriptor,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pImageInfo = &descriptorImage,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = texture.descriptor,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pImageInfo = &descriptorImage,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = texture.descriptor,
            .dstBinding = 3,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .pImageInfo = &descriptorSampler,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = texture.descriptor,
            .dstBinding = 4,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .pImageInfo = &descriptorClampSampler,
        },
    };
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return texture;
}

void VulkanDevice::createPreviewBackgroundStream(std::uint32_t width, std::uint32_t height) {
    const std::array vertices{
        PreviewVertex{{-1.0F, -1.0F, 0.0F}, {}, {0.0F, 1.0F}},
        PreviewVertex{{1.0F, -1.0F, 0.0F}, {}, {1.0F, 1.0F}},
        PreviewVertex{{1.0F, 1.0F, 0.0F}, {}, {1.0F, 0.0F}},
        PreviewVertex{{-1.0F, 1.0F, 0.0F}, {}, {0.0F, 0.0F}},
    };
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
    uploadPreviewBuffer(vertices.data(), sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        previewBackgroundVertexBuffer_, previewBackgroundVertexMemory_);
    uploadPreviewBuffer(indices.data(), sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                        previewBackgroundIndexBuffer_, previewBackgroundIndexMemory_);
    previewBackgroundIndexCount_ = static_cast<std::uint32_t>(indices.size());
    previewBackgroundTexture_ = createEmptyPreviewTextureResource(width, height);
    previewBackgroundExtent_ = {width, height};
    previewBackgroundByteSize_ = static_cast<VkDeviceSize>(width) * height * 4U;
    for (auto& frame : frames_) {
        const VkBufferCreateInfo bufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = previewBackgroundByteSize_,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        check(vkCreateBuffer(device_, &bufferInfo, nullptr, &frame.backgroundStagingBuffer),
              "create streaming background staging buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, frame.backgroundStagingBuffer, &requirements);
        const VkMemoryAllocateInfo allocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
        };
        check(vkAllocateMemory(device_, &allocation, nullptr, &frame.backgroundStagingMemory),
              "allocate streaming background staging memory");
        check(vkBindBufferMemory(device_, frame.backgroundStagingBuffer, frame.backgroundStagingMemory, 0),
              "bind streaming background staging memory");
        check(vkMapMemory(device_, frame.backgroundStagingMemory, 0, previewBackgroundByteSize_, 0,
                          &frame.mappedBackgroundStaging),
              "persistently map streaming background");
    }
    log::info("Created streaming preview background: ", width, "x", height, ", staging buffers=", frames_.size());
}

void VulkanDevice::recordPreviewBackgroundUpload(VkCommandBuffer command, Frame& frame) {
    if (!frame.backgroundUploadPending)
        return;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkImageMemoryBarrier2 toTransfer{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask =
            previewBackgroundInitialized_ ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = previewBackgroundInitialized_ ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0U,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout =
            previewBackgroundInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = previewBackgroundTexture_.image,
        .subresourceRange = range,
    };
    const VkDependencyInfo transferDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toTransfer,
    };
    vkCmdPipelineBarrier2(command, &transferDependency);
    const VkBufferImageCopy copy{
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {previewBackgroundExtent_.width, previewBackgroundExtent_.height, 1},
    };
    vkCmdCopyBufferToImage(command, frame.backgroundStagingBuffer, previewBackgroundTexture_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    const VkImageMemoryBarrier2 toShader{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = previewBackgroundTexture_.image,
        .subresourceRange = range,
    };
    const VkDependencyInfo shaderDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toShader,
    };
    vkCmdPipelineBarrier2(command, &shaderDependency);
    frame.backgroundUploadPending = false;
    previewBackgroundInitialized_ = true;
}

void VulkanDevice::createFrames() {
    for (auto& frame : frames_) {
        const VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queueFamily_,
        };
        check(vkCreateCommandPool(device_, &poolInfo, nullptr, &frame.commandPool), "create command pool");
        const VkCommandBufferAllocateInfo commandInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = frame.commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        check(vkAllocateCommandBuffers(device_, &commandInfo, &frame.commandBuffer), "allocate command buffer");
        const VkSemaphoreCreateInfo semaphoreInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.imageAvailable), "create image semaphore");
        check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.renderFinished), "create render semaphore");
        const VkFenceCreateInfo fenceInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        check(vkCreateFence(device_, &fenceInfo, nullptr, &frame.inFlight), "create frame fence");
        if (timestampValidBits_ != 0 && physicalProperties_.limits.timestampPeriod > 0.0F) {
            const VkQueryPoolCreateInfo queryInfo{
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = 2,
            };
            check(vkCreateQueryPool(device_, &queryInfo, nullptr, &frame.timestampQueryPool),
                  "create preview timestamp query pool");
        }
    }
}

void VulkanDevice::destroyFrames() {
    if (device_ == VK_NULL_HANDLE)
        return;
    destroyPreviewMaterialBuffers();
    destroyPreviewMorphs();
    destroyPreviewIndirectBuffers();
    for (auto& frame : frames_) {
        destroyNativeUploadBuffers(frame);
        if (frame.timestampQueryPool != VK_NULL_HANDLE)
            vkDestroyQueryPool(device_, frame.timestampQueryPool, nullptr);
        if (frame.inFlight != VK_NULL_HANDLE)
            vkDestroyFence(device_, frame.inFlight, nullptr);
        if (frame.renderFinished != VK_NULL_HANDLE)
            vkDestroySemaphore(device_, frame.renderFinished, nullptr);
        if (frame.imageAvailable != VK_NULL_HANDLE)
            vkDestroySemaphore(device_, frame.imageAvailable, nullptr);
        if (frame.commandPool != VK_NULL_HANDLE)
            vkDestroyCommandPool(device_, frame.commandPool, nullptr);
        frame = {};
    }
}

void VulkanDevice::resetNativeUploadBuffers(Frame& frame) noexcept {
    for (auto& upload : frame.nativeUploadBuffers)
        upload.offset = 0;
}

void VulkanDevice::destroyNativeUploadBuffers(Frame& frame) noexcept {
    for (auto& upload : frame.nativeUploadBuffers) {
        if (upload.mapped != nullptr)
            vkUnmapMemory(device_, upload.memory);
        if (upload.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, upload.memory, nullptr);
        if (upload.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, upload.buffer, nullptr);
    }
    frame.nativeUploadBuffers.clear();
}

VulkanDevice::Frame* VulkanDevice::frameForCommandBuffer(VkCommandBuffer commandBuffer) noexcept {
    const auto found = std::find_if(frames_.begin(), frames_.end(), [commandBuffer](const auto& frame) {
        return frame.commandBuffer == commandBuffer;
    });
    return found == frames_.end() ? nullptr : &*found;
}

VulkanDevice::Frame::NativeUploadBuffer& VulkanDevice::allocateNativeUploadBuffer(Frame& frame, VkDeviceSize size,
                                                                                  VkDeviceSize alignment) {
    for (auto& upload : frame.nativeUploadBuffers) {
        const auto offset = alignDeviceAddress(upload.offset, alignment);
        if (offset <= upload.capacity && size <= upload.capacity - offset) {
            upload.offset = offset + size;
            return upload;
        }
    }

    const VkDeviceSize capacity = std::max<VkDeviceSize>(growPreviewCapacity(size), 64ULL * 1024ULL);
    Frame::NativeUploadBuffer upload;
    try {
        const VkBufferCreateInfo bufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = capacity,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        check(vkCreateBuffer(device_, &bufferInfo, nullptr, &upload.buffer), "create native frame upload buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, upload.buffer, &requirements);
        const VkMemoryAllocateInfo allocationInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
        };
        check(vkAllocateMemory(device_, &allocationInfo, nullptr, &upload.memory),
              "allocate native frame upload memory");
        check(vkBindBufferMemory(device_, upload.buffer, upload.memory, 0), "bind native frame upload memory");
        check(vkMapMemory(device_, upload.memory, 0, capacity, 0, &upload.mapped), "map native frame upload memory");
        upload.capacity = capacity;
        upload.offset = size;
        frame.nativeUploadBuffers.push_back(upload);
    } catch (...) {
        if (upload.mapped != nullptr)
            vkUnmapMemory(device_, upload.memory);
        if (upload.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, upload.memory, nullptr);
        if (upload.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, upload.buffer, nullptr);
        throw;
    }
    return frame.nativeUploadBuffers.back();
}

void VulkanDevice::resolveTimestampQuery(Frame& frame) noexcept {
    previewGpuNanoseconds_ = 0;
    if (frame.timestampQueryPool == VK_NULL_HANDLE || timestampValidBits_ == 0 ||
        physicalProperties_.limits.timestampPeriod <= 0.0F)
        return;
    std::array<std::uint64_t, 2> timestamps{};
    const auto result = vkGetQueryPoolResults(device_, frame.timestampQueryPool, 0, 2, sizeof(timestamps),
                                              timestamps.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS)
        return;
    const auto ticks = timestampDelta(timestamps[0], timestamps[1], timestampValidBits_);
    previewGpuNanoseconds_ = static_cast<std::uint64_t>(
        static_cast<double>(ticks) * static_cast<double>(physicalProperties_.limits.timestampPeriod));
}

void VulkanDevice::createUi() {
#if DAYO_HAS_IMGUI
    const std::array<VkDescriptorPoolSize, 1> poolSizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024},
    }};
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1024,
        .poolSizeCount = static_cast<std::uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data(),
    };
    check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &imguiDescriptorPool_), "create ImGui descriptor pool");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDpiScaleFonts = true;
    const float displayScale = std::max(SDL_GetWindowDisplayScale(window_.sdlHandle()), 1.0F);
    ui::applyEditorTheme(displayScale);
    ui::loadEditorFonts();
    ImGui::GetStyle().FontScaleDpi = displayScale;
    if (!ImGui_ImplSDL3_InitForVulkan(window_.sdlHandle())) {
        throw std::runtime_error("ImGui SDL3 initialization failed");
    }
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = instance_;
    info.PhysicalDevice = physicalDevice_;
    info.Device = device_;
    info.QueueFamily = queueFamily_;
    info.Queue = queue_;
    info.DescriptorPool = imguiDescriptorPool_;
    info.MinImageCount = static_cast<std::uint32_t>(swapchainImages_.size());
    info.ImageCount = static_cast<std::uint32_t>(swapchainImages_.size());
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchainFormat_;
    if (!ImGui_ImplVulkan_Init(&info))
        throw std::runtime_error("ImGui Vulkan initialization failed");
    uiInitialized_ = true;
#endif
}

void VulkanDevice::destroyUi() {
#if DAYO_HAS_IMGUI
    if (uiInitialized_) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        uiInitialized_ = false;
    }
    if (imguiDescriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, imguiDescriptorPool_, nullptr);
        imguiDescriptorPool_ = VK_NULL_HANDLE;
    }
#endif
}

void VulkanDevice::selectRenderer(RendererKind requested) {
    if (capabilities_.supports(requested)) {
        activeRenderer_ = requested;
        log::info("Renderer selected: ", toString(activeRenderer_));
        return;
    }
    activeRenderer_ = RendererKind::preview;
    log::warn(toString(requested), " disabled; missing: ", capabilities_.missingFeatures(requested),
              ". Falling back to Preview.");
}

void VulkanDevice::setNativeFrameRecorder(NativeFrameRecorder recorder) {
    nativeFrameRecorder_ = std::move(recorder);
}

void VulkanDevice::setNativeRendererAvailability(bool subayai, bool bdpt) {
    capabilities_.nativeSubayai = subayai;
    capabilities_.nativeBdpt = bdpt;
}

void VulkanDevice::resize() {
    swapchainDirty_ = true;
}

void VulkanDevice::recreateSwapchain() {
    if (window_.pixelWidth() == 0 || window_.pixelHeight() == 0)
        return;
    check(vkDeviceWaitIdle(device_), "wait before swapchain recreation");
    destroyViewportResources();
    destroyUi();
    destroyOffscreenResource();
    destroyPipeline();
    destroySwapchain();
    createSwapchain();
    createPipeline();
    createUi();
    viewportRequested_ = false;
    requestedViewportExtent_ = {};
    swapchainDirty_ = false;
}

void VulkanDevice::beginUiFrame() {
    if (swapchainDirty_)
        recreateSwapchain();
#if DAYO_HAS_IMGUI
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
#endif
}

PreviewRenderPlan VulkanDevice::buildPreviewRenderPlan(bool includeUi) const noexcept {
    const bool hasModelData = previewIndexBuffer_ != VK_NULL_HANDLE && previewIndexCount_ != 0;
    const bool hasMaterialDraws = !previewGpuScene_.materials.empty() && !previewGpuScene_.draws.empty();
    return {
        .background = previewGpuScene_.view.backgroundEnabled &&
                      (previewGpuScene_.view.screenSource == PreviewScene::ScreenSource::backgroundImage ||
                       previewGpuScene_.view.screenSource == PreviewScene::ScreenSource::backgroundVideo) &&
                      previewBackgroundTexture_.descriptor != VK_NULL_HANDLE && previewBackgroundIndexCount_ != 0,
        .model = hasModelData,
        .transparent = hasModelData && hasMaterialDraws,
        .edge =
            hasModelData && hasMaterialDraws && previewGpuScene_.view.outlineEnabled && edgePipeline_ != VK_NULL_HANDLE,
        .ui = includeUi,
    };
}

void VulkanDevice::recordPreviewModel(VkCommandBuffer command, const PreviewPushConstants& constants,
                                      const PreviewRenderPlan& plan) {
    auto& frame = frames_[frameIndex_];
    if (!plan.model || frame.previewMaterialDescriptor == VK_NULL_HANDLE)
        return;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1,
                            &frame.previewMaterialDescriptor, 0, nullptr);
    if (previewBindlessDescriptor_ != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 3, 1,
                                &previewBindlessDescriptor_, 0, nullptr);
    if (frame.previewMorphDescriptor != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 4, 1,
                                &frame.previewMorphDescriptor, 0, nullptr);

    constexpr auto indirectMaterialSentinel = std::numeric_limits<std::uint32_t>::max();
    const auto validDraw = [&](const PreviewDraw& item) {
        return item.materialIndex < previewGpuScene_.materials.size() && item.indexCount != 0 &&
               item.firstIndex < previewIndexCount_;
    };
    const auto drawDirect = [&](const PreviewDraw& item, VkPipeline pipeline) {
        if (!validDraw(item))
            return;
        if (previewTextures_.empty())
            return;
        const auto descriptor = previewTextures_.front().descriptor;
        if (descriptor == VK_NULL_HANDLE)
            return;
        auto drawConstants = constants;
        drawConstants.materialIndex = item.materialIndex;
        drawConstants.instanceCount = std::max(item.instanceCount, 1U);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptor, 0,
                                nullptr);
        vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(drawConstants), &drawConstants);
        const auto count = std::min(item.indexCount, previewIndexCount_ - item.firstIndex);
        vkCmdDrawIndexed(command, count, drawConstants.instanceCount, item.firstIndex, 0, 0);
    };
    const auto drawIndirectBatch = [&](std::size_t begin, std::size_t end, VkPipeline pipeline) {
        if (begin == end || frame.previewIndirectBuffer == VK_NULL_HANDLE || previewTextures_.empty())
            return;
        const auto descriptor = previewTextures_.front().descriptor;
        if (descriptor == VK_NULL_HANDLE)
            return;
        auto drawConstants = constants;
        drawConstants.materialIndex = indirectMaterialSentinel;
        drawConstants.instanceCount = 1;
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptor, 0,
                                nullptr);
        vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(drawConstants), &drawConstants);
        const auto drawCount = static_cast<std::uint32_t>(end - begin);
        vkCmdDrawIndexedIndirect(command, frame.previewIndirectBuffer,
                                 static_cast<VkDeviceSize>(begin) * sizeof(VkDrawIndexedIndirectCommand), drawCount,
                                 sizeof(VkDrawIndexedIndirectCommand));
    };
    const auto drawPass = [&](VkPipeline pipeline) {
        std::size_t indirectBegin = previewGpuScene_.draws.size();
        const auto flushIndirect = [&](std::size_t end) {
            if (indirectBegin != previewGpuScene_.draws.size()) {
                drawIndirectBatch(indirectBegin, end, pipeline);
                indirectBegin = previewGpuScene_.draws.size();
            }
        };
        for (std::size_t index = 0; index < previewGpuScene_.draws.size(); ++index) {
            const auto& item = previewGpuScene_.draws[index];
            const bool canBatch = frame.previewIndirectBuffer != VK_NULL_HANDLE && validDraw(item) &&
                                  std::max(item.instanceCount, 1U) == 1U;
            if (canBatch) {
                if (indirectBegin == previewGpuScene_.draws.size())
                    indirectBegin = index;
                continue;
            }
            flushIndirect(index);
            drawDirect(item, pipeline);
        }
        flushIndirect(previewGpuScene_.draws.size());
    };

    if (!plan.transparent) {
        if (!previewTextures_.empty()) {
            const auto descriptor = previewTextures_.front().descriptor;
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptor, 0,
                                    nullptr);
            vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(constants), &constants);
            vkCmdDrawIndexed(command, previewIndexCount_, 1, 0, 0, 0);
        }
        return;
    }

    // Keep file order for alpha blending while coalescing adjacent single-instance draws.
    drawPass(transparentPipeline_);

    if (!plan.edge)
        return;
    drawPass(edgePipeline_);
}

void VulkanDevice::recordPreviewPass(VkCommandBuffer command, Frame& frame, VkImage colorImage, VkImageView colorView,
                                     DepthResource& depth, VkExtent2D extent, bool colorInitialized,
                                     VkImageLayout previousColorLayout, VkPipelineStageFlags2 previousColorStage,
                                     VkAccessFlags2 previousColorAccess, bool preservePreviousFrame) {
    const VkImageMemoryBarrier2 toColor{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = colorInitialized ? previousColorStage : VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = colorInitialized ? previousColorAccess : 0U,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = colorInitialized ? previousColorLayout : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = colorImage,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkImageMemoryBarrier2 toDepth{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = depth.initialized ? VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT : VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = depth.initialized ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0U,
        .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = depth.initialized ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = depth.image,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    const std::array renderBarriers{toColor, toDepth};
    const VkDependencyInfo renderDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<std::uint32_t>(renderBarriers.size()),
        .pImageMemoryBarriers = renderBarriers.data(),
    };
    vkCmdPipelineBarrier2(command, &renderDependency);

    VkClearValue clear{};
    clear.color = !previewGpuScene_.view.backgroundEnabled ||
                          previewGpuScene_.view.screenSource == PreviewScene::ScreenSource::white
                      ? VkClearColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}
                  : activeRenderer_ == RendererKind::preview ? VkClearColorValue{{0.025F, 0.035F, 0.055F, 1.0F}}
                                                             : VkClearColorValue{{0.055F, 0.025F, 0.045F, 1.0F}};
    const VkRenderingAttachmentInfo colorAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = colorView,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = preservePreviousFrame && colorInitialized && previewGpuScene_.view.backgroundEnabled &&
                          previewGpuScene_.view.screenSource == PreviewScene::ScreenSource::previousFrame
                      ? VK_ATTACHMENT_LOAD_OP_LOAD
                      : VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = clear,
    };
    VkClearValue depthClear{};
    depthClear.depthStencil = {1.0F, 0};
    const VkRenderingAttachmentInfo depthAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = depth.view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = depthClear,
    };
    const VkRenderingInfo renderingInfo{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachment,
        .pDepthAttachment = &depthAttachment,
    };
    vkCmdBeginRendering(command, &renderingInfo);
    depth.initialized = true;
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                            &frame.previewBoneDescriptor, 0, nullptr);
    if (frame.previewMaterialDescriptor != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1,
                                &frame.previewMaterialDescriptor, 0, nullptr);
    if (previewBindlessDescriptor_ != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 3, 1,
                                &previewBindlessDescriptor_, 0, nullptr);
    if (frame.previewMorphDescriptor != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 4, 1,
                                &frame.previewMorphDescriptor, 0, nullptr);
    const auto previewVertexBuffer =
        previewStaticVertexBuffer_ != VK_NULL_HANDLE ? previewStaticVertexBuffer_ : frame.previewVertexBuffer;
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(command, 0, 1, &previewVertexBuffer, &vertexOffset);
    vkCmdBindIndexBuffer(command, previewIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);

    PreviewPushConstants constants;
    std::copy_n(previewGpuScene_.view.cameraRotation, 3, constants.camera.begin());
    constants.camera[3] = previewGpuScene_.view.cameraDistance;
    std::copy_n(previewGpuScene_.view.target, 3, constants.target.begin());
    constants.target[3] = previewGpuScene_.view.perspective ? previewGpuScene_.view.verticalFovRadians
                                                            : -previewGpuScene_.view.verticalFovRadians;
    std::copy_n(previewGpuScene_.view.lightDirection, 3, constants.light.begin());
    constants.light[3] =
        extent.height == 0 ? 1.0F : static_cast<float>(extent.width) / static_cast<float>(extent.height);
    std::copy_n(previewGpuScene_.view.lightColor, 3, constants.lightColor.begin());
    constants.debug = {static_cast<float>(previewGpuScene_.view.debugMaterial),
                       static_cast<float>(previewGpuScene_.view.debugFlags), 0.0F, 0.0F};
    constants.viewport = {static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0F, 0.0F};
    const auto plan = buildPreviewRenderPlan(false);
    if (plan.background) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, backgroundPipeline_);
        const VkDeviceSize backgroundOffset = 0;
        vkCmdBindVertexBuffers(command, 0, 1, &previewBackgroundVertexBuffer_, &backgroundOffset);
        vkCmdBindIndexBuffer(command, previewBackgroundIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &previewBackgroundTexture_.descriptor, 0, nullptr);
        vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(constants), &constants);
        vkCmdDrawIndexed(command, previewBackgroundIndexCount_, 1, 0, 0, 0);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindVertexBuffers(command, 0, 1, &previewVertexBuffer, &vertexOffset);
        vkCmdBindIndexBuffer(command, previewIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    }
    recordPreviewModel(command, constants, plan);
    vkCmdEndRendering(command);
}

void VulkanDevice::renderFrame() {
    if (window_.pixelWidth() == 0 || window_.pixelHeight() == 0)
        return;
#if DAYO_HAS_IMGUI
    ImGui::Render();
#endif
    auto& frame = frames_[frameIndex_];
    check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait for frame");
    reclaimAccelerationScratch(frameIndex_);
    resetNativeUploadBuffers(frame);
    resolveTimestampQuery(frame);
    synchronizePreviewVertices(frame);
    synchronizePreviewBones(frame);
    synchronizePreviewMorphs(frame);
    synchronizePreviewMaterials(frame);
    synchronizePreviewIndirect(frame);

    std::uint32_t imageIndex = 0;
    const auto acquire =
        vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
        check(acquire, "acquire swapchain image");
    check(vkResetFences(device_, 1, &frame.inFlight), "reset frame fence");
    check(vkResetCommandPool(device_, frame.commandPool, 0), "reset command pool");
    const VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "begin command buffer");
    if (frame.timestampQueryPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(frame.commandBuffer, frame.timestampQueryPool, 0, 2);
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timestampQueryPool, 0);
    }
    recordPreviewBackgroundUpload(frame.commandBuffer, frame);

    std::optional<NativeFrameOutput> nativeOutput;
    if (nativeFrameRecorder_ && activeRenderer_ != RendererKind::preview) {
#if DAYO_HAS_IMGUI
        const auto& viewport = viewportResources_[frameIndex_];
        if (viewportRequested_ && viewport.colorImage != VK_NULL_HANDLE) {
            const RenderTargetDesc target{viewport.extent.width, viewport.extent.height};
            VulkanCommandList commands(*this, frame.commandBuffer);
            nativeOutput = nativeFrameRecorder_(commands, target);
        }
#else
        const RenderTargetDesc target{swapchainExtent_.width, swapchainExtent_.height};
        VulkanCommandList commands(*this, frame.commandBuffer);
        nativeOutput = nativeFrameRecorder_(commands, target);
#endif
        if (nativeOutput.has_value() && !nativeOutput->valid())
            throw std::invalid_argument("native frame recorder returned an invalid output");
    }

#if DAYO_HAS_IMGUI
    if (viewportRequested_) {
        auto& viewport = viewportResources_[frameIndex_];
        if (viewport.colorImage != VK_NULL_HANDLE) {
            if (nativeOutput.has_value()) {
                recordNativeOutputToImage(frame.commandBuffer, *nativeOutput, viewport.colorImage, viewport.colorView,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                          viewport.colorInitialized, viewport.extent);
                viewport.colorInitialized = true;
            } else {
                recordPreviewPass(frame.commandBuffer, frame, viewport.colorImage, viewport.colorView, viewport.depth,
                                  viewport.extent, viewport.colorInitialized, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, true);
                const VkImageMemoryBarrier2 toSample{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = viewport.colorImage,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                };
                const VkDependencyInfo toSampleDependency{
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .imageMemoryBarrierCount = 1,
                    .pImageMemoryBarriers = &toSample,
                };
                vkCmdPipelineBarrier2(frame.commandBuffer, &toSampleDependency);
                viewport.colorInitialized = true;
            }
        }
    }
    if (frame.timestampQueryPool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampQueryPool, 1);

    const VkImageMemoryBarrier2 toColor{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = swapchainInitialized_[imageIndex] ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                                          : VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = swapchainInitialized_[imageIndex] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapchainImages_[imageIndex],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo toColorDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toColor,
    };
    vkCmdPipelineBarrier2(frame.commandBuffer, &toColorDependency);

    VkClearValue clear{};
    clear.color = {{0.045F, 0.050F, 0.060F, 1.0F}};
    const VkRenderingAttachmentInfo attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = swapchainViews_[imageIndex],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = clear,
    };
    const VkRenderingInfo renderingInfo{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, swapchainExtent_},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    };
    vkCmdBeginRendering(frame.commandBuffer, &renderingInfo);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commandBuffer);
    vkCmdEndRendering(frame.commandBuffer);
#else
    if (nativeOutput.has_value()) {
        recordNativeOutputToImage(
            frame.commandBuffer, *nativeOutput, swapchainImages_[imageIndex], swapchainViews_[imageIndex],
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0U,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, swapchainInitialized_[imageIndex], swapchainExtent_);
    } else {
        recordPreviewPass(frame.commandBuffer, frame, swapchainImages_[imageIndex], swapchainViews_[imageIndex],
                          swapchainDepth_[imageIndex], swapchainExtent_, swapchainInitialized_[imageIndex],
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0U, true);
    }
    if (frame.timestampQueryPool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampQueryPool, 1);
#endif

    const VkImageMemoryBarrier2 toPresent{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapchainImages_[imageIndex],
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo toPresentDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toPresent,
    };
    vkCmdPipelineBarrier2(frame.commandBuffer, &toPresentDependency);
    check(vkEndCommandBuffer(frame.commandBuffer), "end command buffer");

    const auto uploadWaitValue = uploadContext_->lastSubmittedValue();
    const std::uint64_t signalValue = ++nextTimelineValue_;
    const std::array<std::uint64_t, 2> waitValues{0, uploadWaitValue};
    const std::array<std::uint64_t, 2> signalValues{0, signalValue};
    const std::array<VkSemaphore, 2> waitSemaphores{frame.imageAvailable, timelineSemaphore_};
    const std::array<VkPipelineStageFlags, 2> waitStages{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT};
    const std::array<VkSemaphore, 2> signalSemaphores{frame.renderFinished, timelineSemaphore_};
    const VkTimelineSemaphoreSubmitInfo timelineSubmit{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = uploadWaitValue == 0 ? 1U : static_cast<std::uint32_t>(waitValues.size()),
        .pWaitSemaphoreValues = waitValues.data(),
        .signalSemaphoreValueCount = static_cast<std::uint32_t>(signalValues.size()),
        .pSignalSemaphoreValues = signalValues.data(),
    };
    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timelineSubmit,
        .waitSemaphoreCount = uploadWaitValue == 0 ? 1U : static_cast<std::uint32_t>(waitSemaphores.size()),
        .pWaitSemaphores = waitSemaphores.data(),
        .pWaitDstStageMask = waitStages.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = &frame.commandBuffer,
        .signalSemaphoreCount = static_cast<std::uint32_t>(signalSemaphores.size()),
        .pSignalSemaphores = signalSemaphores.data(),
    };
    check(vkQueueSubmit(queue_, 1, &submitInfo, frame.inFlight), "submit frame");
    const VkPresentInfoKHR presentInfo{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &frame.renderFinished,
        .swapchainCount = 1,
        .pSwapchains = &swapchain_,
        .pImageIndices = &imageIndex,
    };
    const auto present = vkQueuePresentKHR(queue_, &presentInfo);
    if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR) {
        swapchainDirty_ = true;
    } else {
        check(present, "present frame");
    }
    swapchainInitialized_[imageIndex] = true;
    frameIndex_ = (frameIndex_ + 1) % frames_.size();
}

void VulkanDevice::destroyOffscreenResource() {
    if (device_ == VK_NULL_HANDLE)
        return;
    if (offscreen_.stagingBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, offscreen_.stagingBuffer, nullptr);
    }
    if (offscreen_.stagingMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, offscreen_.stagingMemory, nullptr);
    }
    if (offscreen_.colorView != VK_NULL_HANDLE)
        vkDestroyImageView(device_, offscreen_.colorView, nullptr);
    if (offscreen_.colorImage != VK_NULL_HANDLE)
        vkDestroyImage(device_, offscreen_.colorImage, nullptr);
    if (offscreen_.colorMemory != VK_NULL_HANDLE)
        vkFreeMemory(device_, offscreen_.colorMemory, nullptr);
    if (offscreen_.depth.view != VK_NULL_HANDLE)
        vkDestroyImageView(device_, offscreen_.depth.view, nullptr);
    if (offscreen_.depth.image != VK_NULL_HANDLE)
        vkDestroyImage(device_, offscreen_.depth.image, nullptr);
    if (offscreen_.depth.memory != VK_NULL_HANDLE)
        vkFreeMemory(device_, offscreen_.depth.memory, nullptr);
    offscreen_ = {};
}

void VulkanDevice::destroyViewportResource(ViewportResource& resource) {
    if (device_ == VK_NULL_HANDLE)
        return;
#if DAYO_HAS_IMGUI
    if (uiInitialized_ && resource.imguiDescriptor != VK_NULL_HANDLE)
        ImGui_ImplVulkan_RemoveTexture(resource.imguiDescriptor);
#endif
    if (resource.depth.view != VK_NULL_HANDLE)
        vkDestroyImageView(device_, resource.depth.view, nullptr);
    if (resource.depth.image != VK_NULL_HANDLE)
        vkDestroyImage(device_, resource.depth.image, nullptr);
    if (resource.depth.memory != VK_NULL_HANDLE)
        vkFreeMemory(device_, resource.depth.memory, nullptr);
    if (resource.colorView != VK_NULL_HANDLE)
        vkDestroyImageView(device_, resource.colorView, nullptr);
    if (resource.colorImage != VK_NULL_HANDLE)
        vkDestroyImage(device_, resource.colorImage, nullptr);
    if (resource.colorMemory != VK_NULL_HANDLE)
        vkFreeMemory(device_, resource.colorMemory, nullptr);
    resource = {};
}

void VulkanDevice::destroyViewportResources() {
    for (auto& resource : viewportResources_)
        destroyViewportResource(resource);
}

void VulkanDevice::createViewportResource(ViewportResource& resource, VkExtent2D extent) {
#if DAYO_HAS_IMGUI
    if (extent.width == 0 || extent.height == 0)
        throw std::invalid_argument("preview viewport is empty");
    destroyViewportResource(resource);
    try {
        const VkImageCreateInfo colorInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = swapchainFormat_,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        check(vkCreateImage(device_, &colorInfo, nullptr, &resource.colorImage), "create viewport color image");
        VkMemoryRequirements colorRequirements{};
        vkGetImageMemoryRequirements(device_, resource.colorImage, &colorRequirements);
        const VkMemoryAllocateInfo colorAllocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = colorRequirements.size,
            .memoryTypeIndex = findMemoryType(colorRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };
        check(vkAllocateMemory(device_, &colorAllocation, nullptr, &resource.colorMemory),
              "allocate viewport color memory");
        check(vkBindImageMemory(device_, resource.colorImage, resource.colorMemory, 0), "bind viewport color memory");
        const VkImageViewCreateInfo colorViewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = resource.colorImage,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = swapchainFormat_,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        check(vkCreateImageView(device_, &colorViewInfo, nullptr, &resource.colorView), "create viewport color view");

        const VkImageCreateInfo depthInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_D32_SFLOAT,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        check(vkCreateImage(device_, &depthInfo, nullptr, &resource.depth.image), "create viewport depth image");
        VkMemoryRequirements depthRequirements{};
        vkGetImageMemoryRequirements(device_, resource.depth.image, &depthRequirements);
        const VkMemoryAllocateInfo depthAllocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = depthRequirements.size,
            .memoryTypeIndex = findMemoryType(depthRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };
        check(vkAllocateMemory(device_, &depthAllocation, nullptr, &resource.depth.memory),
              "allocate viewport depth memory");
        check(vkBindImageMemory(device_, resource.depth.image, resource.depth.memory, 0), "bind viewport depth memory");
        const VkImageViewCreateInfo depthViewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = resource.depth.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_D32_SFLOAT,
            .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
        };
        check(vkCreateImageView(device_, &depthViewInfo, nullptr, &resource.depth.view), "create viewport depth view");
        resource.imguiDescriptor =
            ImGui_ImplVulkan_AddTexture(resource.colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (resource.imguiDescriptor == VK_NULL_HANDLE)
            throw std::runtime_error("create ImGui viewport descriptor failed");
        resource.extent = extent;
    } catch (...) {
        destroyViewportResource(resource);
        throw;
    }
#else
    static_cast<void>(resource);
    static_cast<void>(extent);
#endif
}

void VulkanDevice::setPreviewViewportExtent(const RenderTargetDesc& target) {
#if DAYO_HAS_IMGUI
    if (target.width == 0 || target.height == 0) {
        viewportRequested_ = false;
        return;
    }
    const auto maximum = physicalProperties_.limits.maxImageDimension2D;
    requestedViewportExtent_ = {std::min(target.width, maximum), std::min(target.height, maximum)};
    viewportRequested_ = true;
    auto& resource = viewportResources_[frameIndex_];
    if (resource.colorImage != VK_NULL_HANDLE && resource.extent.width == requestedViewportExtent_.width &&
        resource.extent.height == requestedViewportExtent_.height)
        return;
    auto& frame = frames_[frameIndex_];
    check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait before viewport resize");
    createViewportResource(resource, requestedViewportExtent_);
#else
    static_cast<void>(target);
#endif
}

PreviewViewport VulkanDevice::previewViewport() const noexcept {
#if DAYO_HAS_IMGUI
    if (!viewportRequested_)
        return {};
    const auto& resource = viewportResources_[frameIndex_];
    if (resource.imguiDescriptor == VK_NULL_HANDLE)
        return {};
    return {
        .textureId = descriptorId(resource.imguiDescriptor),
        .width = resource.extent.width,
        .height = resource.extent.height,
    };
#else
    return {};
#endif
}

void VulkanDevice::createOffscreenResource(VkExtent2D extent) {
    if (extent.width == 0 || extent.height == 0)
        throw std::invalid_argument("offscreen target is empty");
    if (offscreen_.extent.width == extent.width && offscreen_.extent.height == extent.height &&
        offscreen_.colorImage != VK_NULL_HANDLE)
        return;
    destroyOffscreenResource();

    const VkImageCreateInfo colorInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = swapchainFormat_,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    check(vkCreateImage(device_, &colorInfo, nullptr, &offscreen_.colorImage), "create offscreen color image");
    VkMemoryRequirements colorRequirements{};
    vkGetImageMemoryRequirements(device_, offscreen_.colorImage, &colorRequirements);
    const VkMemoryAllocateInfo colorAllocation{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = colorRequirements.size,
        .memoryTypeIndex = findMemoryType(colorRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    check(vkAllocateMemory(device_, &colorAllocation, nullptr, &offscreen_.colorMemory),
          "allocate offscreen color memory");
    check(vkBindImageMemory(device_, offscreen_.colorImage, offscreen_.colorMemory, 0), "bind offscreen color memory");
    const VkImageViewCreateInfo colorViewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = offscreen_.colorImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = swapchainFormat_,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    check(vkCreateImageView(device_, &colorViewInfo, nullptr, &offscreen_.colorView), "create offscreen color view");

    const VkImageCreateInfo depthInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    check(vkCreateImage(device_, &depthInfo, nullptr, &offscreen_.depth.image), "create offscreen depth image");
    VkMemoryRequirements depthRequirements{};
    vkGetImageMemoryRequirements(device_, offscreen_.depth.image, &depthRequirements);
    const VkMemoryAllocateInfo depthAllocation{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = depthRequirements.size,
        .memoryTypeIndex = findMemoryType(depthRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    check(vkAllocateMemory(device_, &depthAllocation, nullptr, &offscreen_.depth.memory),
          "allocate offscreen depth memory");
    check(vkBindImageMemory(device_, offscreen_.depth.image, offscreen_.depth.memory, 0),
          "bind offscreen depth memory");
    const VkImageViewCreateInfo depthViewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = offscreen_.depth.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    check(vkCreateImageView(device_, &depthViewInfo, nullptr, &offscreen_.depth.view), "create offscreen depth view");

    offscreen_.stagingSize = static_cast<VkDeviceSize>(extent.width) * extent.height * 4U;
    const VkBufferCreateInfo stagingInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = offscreen_.stagingSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    check(vkCreateBuffer(device_, &stagingInfo, nullptr, &offscreen_.stagingBuffer), "create offscreen staging buffer");
    VkMemoryRequirements stagingRequirements{};
    vkGetBufferMemoryRequirements(device_, offscreen_.stagingBuffer, &stagingRequirements);
    const VkMemoryAllocateInfo stagingAllocation{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = stagingRequirements.size,
        .memoryTypeIndex = findMemoryType(stagingRequirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    check(vkAllocateMemory(device_, &stagingAllocation, nullptr, &offscreen_.stagingMemory),
          "allocate offscreen staging memory");
    check(vkBindBufferMemory(device_, offscreen_.stagingBuffer, offscreen_.stagingMemory, 0),
          "bind offscreen staging memory");
    offscreen_.extent = extent;
}

core::ImageRgba8 VulkanDevice::renderToImage(const RenderTargetDesc& target) {
    if (target.width == 0 || target.height == 0)
        throw std::invalid_argument("video dimensions must be non-zero");
    const VkExtent2D extent{target.width, target.height};
    auto& frame = frames_[frameIndex_];
    check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait for offscreen frame slot");
    reclaimAccelerationScratch(frameIndex_);
    resetNativeUploadBuffers(frame);
    const auto uploadWaitValue = uploadContext_->lastSubmittedValue();
    if (uploadWaitValue != 0)
        uploadContext_->wait(uploadWaitValue);
    if (offscreen_.colorImage != VK_NULL_HANDLE &&
        (offscreen_.extent.width != extent.width || offscreen_.extent.height != extent.height)) {
        // The offscreen image is shared by the bounded readback path. A size
        // change destroys it, so wait for every prior submission before
        // replacing the resource.
        waitIdle();
    }
    createOffscreenResource(extent);
    resolveTimestampQuery(frame);
    synchronizePreviewVertices(frame);
    synchronizePreviewBones(frame);
    synchronizePreviewMorphs(frame);
    synchronizePreviewMaterials(frame);
    synchronizePreviewIndirect(frame);
    check(vkResetFences(device_, 1, &frame.inFlight), "reset offscreen fence");
    check(vkResetCommandPool(device_, frame.commandPool, 0), "reset offscreen command pool");
    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    check(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "begin offscreen command buffer");
    if (frame.timestampQueryPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(frame.commandBuffer, frame.timestampQueryPool, 0, 2);
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timestampQueryPool, 0);
    }
    recordPreviewBackgroundUpload(frame.commandBuffer, frame);

    std::optional<NativeFrameOutput> nativeOutput;
    if (nativeFrameRecorder_ && activeRenderer_ != RendererKind::preview) {
        VulkanCommandList commands(*this, frame.commandBuffer);
        nativeOutput = nativeFrameRecorder_(commands, target);
        if (nativeOutput.has_value() && !nativeOutput->valid())
            throw std::invalid_argument("native frame recorder returned an invalid output");
    }
    if (nativeOutput.has_value()) {
        recordNativeOutputToImage(frame.commandBuffer, *nativeOutput, offscreen_.colorImage, offscreen_.colorView,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                                  offscreen_.colorInitialized, extent);
    } else {
        recordPreviewPass(frame.commandBuffer, frame, offscreen_.colorImage, offscreen_.colorView, offscreen_.depth,
                          extent, offscreen_.colorInitialized, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false);
    }
    if (frame.timestampQueryPool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampQueryPool, 1);

    const VkImageMemoryBarrier2 toTransfer{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = offscreen_.colorImage,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo transferDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toTransfer,
    };
    vkCmdPipelineBarrier2(frame.commandBuffer, &transferDependency);
    const VkBufferImageCopy copy{
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {extent.width, extent.height, 1},
    };
    vkCmdCopyImageToBuffer(frame.commandBuffer, offscreen_.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           offscreen_.stagingBuffer, 1, &copy);
    offscreen_.colorInitialized = true;
    check(vkEndCommandBuffer(frame.commandBuffer), "end offscreen command buffer");
    const std::uint64_t signalValue = ++nextTimelineValue_;
    const VkPipelineStageFlags uploadWaitStage = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    const VkTimelineSemaphoreSubmitInfo timelineSubmit{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = uploadWaitValue == 0 ? 0U : 1U,
        .pWaitSemaphoreValues = &uploadWaitValue,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &signalValue,
    };
    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timelineSubmit,
        .waitSemaphoreCount = uploadWaitValue == 0 ? 0U : 1U,
        .pWaitSemaphores = &timelineSemaphore_,
        .pWaitDstStageMask = &uploadWaitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = &frame.commandBuffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &timelineSemaphore_,
    };
    check(vkQueueSubmit(queue_, 1, &submitInfo, frame.inFlight), "submit offscreen frame");
    check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait for offscreen frame");

    core::ImageRgba8 image;
    image.width = target.width;
    image.height = target.height;
    image.pixels.resize(static_cast<std::size_t>(offscreen_.stagingSize));
    void* mapped = nullptr;
    check(vkMapMemory(device_, offscreen_.stagingMemory, 0, offscreen_.stagingSize, 0, &mapped), "map offscreen frame");
    std::memcpy(image.pixels.data(), mapped, image.pixels.size());
    vkUnmapMemory(device_, offscreen_.stagingMemory);
    if (swapchainFormat_ == VK_FORMAT_B8G8R8A8_UNORM || swapchainFormat_ == VK_FORMAT_B8G8R8A8_SRGB) {
        for (std::size_t index = 0; index < image.pixels.size(); index += 4) {
            std::swap(image.pixels[index], image.pixels[index + 2]);
        }
    }
    frameIndex_ = (frameIndex_ + 1) % frames_.size();
    return image;
}

void VulkanDevice::waitIdle() {
    if (device_ != VK_NULL_HANDLE) {
        check(vkDeviceWaitIdle(device_), "wait for Vulkan device");
        reclaimAllAccelerationScratch();
        if (!frames_.empty()) {
            const auto previousFrame = (frameIndex_ + frames_.size() - 1U) % frames_.size();
            resolveTimestampQuery(frames_[previousFrame]);
        }
    }
}

void VulkanDevice::destroyPreviewMesh() {
    if (previewStaticVertexBuffer_ != VK_NULL_HANDLE)
        vkDestroyBuffer(device_, previewStaticVertexBuffer_, nullptr);
    if (previewStaticVertexMemory_ != VK_NULL_HANDLE)
        vkFreeMemory(device_, previewStaticVertexMemory_, nullptr);
    previewStaticVertexBuffer_ = VK_NULL_HANDLE;
    previewStaticVertexMemory_ = VK_NULL_HANDLE;
    if (previewIndexBuffer_ != VK_NULL_HANDLE)
        vkDestroyBuffer(device_, previewIndexBuffer_, nullptr);
    if (previewIndexMemory_ != VK_NULL_HANDLE)
        vkFreeMemory(device_, previewIndexMemory_, nullptr);
    for (auto& frame : frames_) {
        if (frame.mappedPreviewVertices != nullptr) {
            vkUnmapMemory(device_, frame.previewVertexMemory);
        }
        if (frame.previewVertexBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, frame.previewVertexBuffer, nullptr);
        }
        if (frame.previewVertexMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, frame.previewVertexMemory, nullptr);
        }
        frame.previewVertexBuffer = VK_NULL_HANDLE;
        frame.previewVertexMemory = VK_NULL_HANDLE;
        frame.mappedPreviewVertices = nullptr;
        frame.previewVertexGeneration = 0;
    }
    previewVertexSize_ = 0;
    previewVertexCapacity_ = 0;
    previewVertexGeneration_ = 0;
    previewIndexBuffer_ = VK_NULL_HANDLE;
    previewIndexMemory_ = VK_NULL_HANDLE;
    previewIndexCount_ = 0;
    previewVertexUpdateCount_ = 0;
}

void VulkanDevice::synchronizePreviewVertices(Frame& frame) {
    if (previewStaticVertexBuffer_ != VK_NULL_HANDLE || frame.previewVertexGeneration == previewVertexGeneration_ ||
        previewVertexSize_ == 0)
        return;
    const auto latest = std::find_if(frames_.begin(), frames_.end(), [this](const Frame& candidate) {
        return candidate.previewVertexGeneration == previewVertexGeneration_;
    });
    if (latest == frames_.end() || latest->mappedPreviewVertices == nullptr)
        return;
    check(vkWaitForFences(device_, 1, &latest->inFlight, VK_TRUE, UINT64_MAX), "wait for latest animated vertices");
    std::memcpy(frame.mappedPreviewVertices, latest->mappedPreviewVertices,
                static_cast<std::size_t>(previewVertexSize_));
    frame.previewVertexGeneration = previewVertexGeneration_;
}

void VulkanDevice::destroyPreviewBones() {
    for (auto& frame : frames_) {
        if (frame.previewBoneDescriptor != VK_NULL_HANDLE && previewDescriptorPool_ != VK_NULL_HANDLE) {
            check(vkFreeDescriptorSets(device_, previewDescriptorPool_, 1, &frame.previewBoneDescriptor),
                  "free preview bone descriptor");
        }
        if (frame.mappedPreviewBones != nullptr)
            vkUnmapMemory(device_, frame.previewBoneMemory);
        if (frame.previewBoneBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, frame.previewBoneBuffer, nullptr);
        }
        if (frame.previewBoneMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, frame.previewBoneMemory, nullptr);
        }
        frame.previewBoneBuffer = VK_NULL_HANDLE;
        frame.previewBoneMemory = VK_NULL_HANDLE;
        frame.mappedPreviewBones = nullptr;
        frame.previewBoneDescriptor = VK_NULL_HANDLE;
        frame.previewBoneGeneration = 0;
    }
    previewBoneSize_ = 0;
    previewBoneCapacity_ = 0;
    previewBoneGeneration_ = 0;
}

void VulkanDevice::synchronizePreviewBones(Frame& frame) {
    if (frame.previewBoneGeneration == previewBoneGeneration_ || previewBoneSize_ == 0)
        return;
    const auto latest = std::find_if(frames_.begin(), frames_.end(), [this](const Frame& candidate) {
        return candidate.previewBoneGeneration == previewBoneGeneration_;
    });
    if (latest == frames_.end() || latest->mappedPreviewBones == nullptr)
        return;
    check(vkWaitForFences(device_, 1, &latest->inFlight, VK_TRUE, UINT64_MAX), "wait for latest preview bones");
    std::memcpy(frame.mappedPreviewBones, latest->mappedPreviewBones, static_cast<std::size_t>(previewBoneSize_));
    frame.previewBoneGeneration = previewBoneGeneration_;
}

void VulkanDevice::destroyPreviewMorphs() {
    for (auto& frame : frames_) {
        if (frame.previewMorphDescriptor != VK_NULL_HANDLE && previewDescriptorPool_ != VK_NULL_HANDLE) {
            check(vkFreeDescriptorSets(device_, previewDescriptorPool_, 1, &frame.previewMorphDescriptor),
                  "free preview morph descriptor");
        }
        if (frame.mappedPreviewMorphDeltas != nullptr)
            vkUnmapMemory(device_, frame.previewMorphDeltaMemory);
        if (frame.mappedPreviewMorphWeights != nullptr)
            vkUnmapMemory(device_, frame.previewMorphWeightMemory);
        if (frame.previewMorphDeltaBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, frame.previewMorphDeltaBuffer, nullptr);
        if (frame.previewMorphDeltaMemory != VK_NULL_HANDLE)
            vkFreeMemory(device_, frame.previewMorphDeltaMemory, nullptr);
        if (frame.previewMorphWeightBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, frame.previewMorphWeightBuffer, nullptr);
        if (frame.previewMorphWeightMemory != VK_NULL_HANDLE)
            vkFreeMemory(device_, frame.previewMorphWeightMemory, nullptr);
        frame.previewMorphDeltaBuffer = VK_NULL_HANDLE;
        frame.previewMorphDeltaMemory = VK_NULL_HANDLE;
        frame.mappedPreviewMorphDeltas = nullptr;
        frame.previewMorphWeightBuffer = VK_NULL_HANDLE;
        frame.previewMorphWeightMemory = VK_NULL_HANDLE;
        frame.mappedPreviewMorphWeights = nullptr;
        frame.previewMorphDescriptor = VK_NULL_HANDLE;
        frame.previewMorphDeltaGeneration = 0;
        frame.previewMorphGeneration = 0;
    }
    previewMorphDeltaSize_ = 0;
    previewMorphDeltaCapacity_ = 0;
    previewMorphWeightSize_ = 0;
    previewMorphWeightCapacity_ = 0;
    previewMorphDeltaGeneration_ = 0;
    previewMorphGeneration_ = 0;
}

void VulkanDevice::rebuildPreviewMorphBuffers() {
    waitIdle();
    destroyPreviewMorphs();
    previewMorphDeltaSize_ = static_cast<VkDeviceSize>(previewGpuScene_.morphDeltas.size() * sizeof(PreviewMorphDelta));
    previewMorphWeightSize_ = static_cast<VkDeviceSize>(previewGpuScene_.morphWeights.size() * sizeof(float));
    previewMorphDeltaCapacity_ = growPreviewCapacity(previewMorphDeltaSize_);
    previewMorphWeightCapacity_ = growPreviewCapacity(previewMorphWeightSize_);
    ++previewMorphDeltaGeneration_;
    ++previewMorphGeneration_;
    for (auto& frame : frames_) {
        uploadPreviewBuffer(previewGpuScene_.morphDeltas.data(), previewMorphDeltaSize_,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, frame.previewMorphDeltaBuffer,
                            frame.previewMorphDeltaMemory, previewMorphDeltaCapacity_);
        uploadPreviewBuffer(previewGpuScene_.morphWeights.data(), previewMorphWeightSize_,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, frame.previewMorphWeightBuffer,
                            frame.previewMorphWeightMemory, previewMorphWeightCapacity_);
        check(vkMapMemory(device_, frame.previewMorphDeltaMemory, 0, previewMorphDeltaSize_, 0,
                          &frame.mappedPreviewMorphDeltas),
              "persistently map preview morph deltas");
        check(vkMapMemory(device_, frame.previewMorphWeightMemory, 0, previewMorphWeightSize_, 0,
                          &frame.mappedPreviewMorphWeights),
              "persistently map preview morph weights");
        const VkDescriptorSetAllocateInfo setInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = previewDescriptorPool_,
            .descriptorSetCount = 1,
            .pSetLayouts = &previewMorphDescriptorSetLayout_,
        };
        check(vkAllocateDescriptorSets(device_, &setInfo, &frame.previewMorphDescriptor),
              "allocate preview morph descriptor");
        const std::array<VkDescriptorBufferInfo, 2> buffers{{
            {frame.previewMorphDeltaBuffer, 0, previewMorphDeltaCapacity_},
            {frame.previewMorphWeightBuffer, 0, previewMorphWeightCapacity_},
        }};
        const std::array writes{
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = frame.previewMorphDescriptor,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = buffers.data(),
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = frame.previewMorphDescriptor,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = buffers.data() + 1,
            },
        };
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        frame.previewMorphDeltaGeneration = previewMorphDeltaGeneration_;
        frame.previewMorphGeneration = previewMorphGeneration_;
    }
}

void VulkanDevice::uploadPreviewMorphDeltas(std::span<const PreviewMorphDelta> deltas) {
    const PreviewMorphDelta fallbackDelta{};
    if (deltas.empty())
        deltas = std::span<const PreviewMorphDelta>(&fallbackDelta, 1);
    previewGpuScene_.morphDeltas.assign(deltas.begin(), deltas.end());
    const auto deltaSize = static_cast<VkDeviceSize>(deltas.size_bytes());
    if (deltaSize > previewMorphDeltaCapacity_ || frames_.front().previewMorphDeltaBuffer == VK_NULL_HANDLE) {
        if (previewGpuScene_.morphWeights.empty())
            previewGpuScene_.morphWeights.push_back(0.0F);
        rebuildPreviewMorphBuffers();
        return;
    }
    waitIdle();
    previewMorphDeltaSize_ = deltaSize;
    ++previewMorphDeltaGeneration_;
    for (auto& frame : frames_) {
        std::memcpy(frame.mappedPreviewMorphDeltas, deltas.data(), deltas.size_bytes());
        frame.previewMorphDeltaGeneration = previewMorphDeltaGeneration_;
    }
}

void VulkanDevice::updatePreviewMorphWeights(std::span<const float> weights) {
    const float fallbackWeight = 0.0F;
    if (weights.empty())
        weights = std::span<const float>(&fallbackWeight, 1);
    previewGpuScene_.morphWeights.assign(weights.begin(), weights.end());
    const auto weightSize = static_cast<VkDeviceSize>(weights.size_bytes());
    if (weightSize > previewMorphWeightCapacity_ || frames_.front().previewMorphWeightBuffer == VK_NULL_HANDLE) {
        if (previewGpuScene_.morphDeltas.empty())
            previewGpuScene_.morphDeltas.push_back(PreviewMorphDelta{});
        rebuildPreviewMorphBuffers();
        return;
    }
    previewMorphWeightSize_ = weightSize;
    auto& frame = frames_[frameIndex_];
    check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait for preview morph frame");
    std::memcpy(frame.mappedPreviewMorphWeights, weights.data(), weights.size_bytes());
    frame.previewMorphGeneration = ++previewMorphGeneration_;
}

void VulkanDevice::synchronizePreviewMorphs(Frame& frame) {
    if (frame.previewMorphGeneration == previewMorphGeneration_ || previewMorphWeightSize_ == 0)
        return;
    const auto latest = std::find_if(frames_.begin(), frames_.end(), [this](const Frame& candidate) {
        return candidate.previewMorphGeneration == previewMorphGeneration_;
    });
    if (latest == frames_.end() || latest->mappedPreviewMorphWeights == nullptr)
        return;
    check(vkWaitForFences(device_, 1, &latest->inFlight, VK_TRUE, UINT64_MAX), "wait for latest preview morphs");
    std::memcpy(frame.mappedPreviewMorphWeights, latest->mappedPreviewMorphWeights,
                static_cast<std::size_t>(previewMorphWeightSize_));
    frame.previewMorphGeneration = previewMorphGeneration_;
}

void VulkanDevice::destroyPreviewMaterialBuffers() {
    for (auto& frame : frames_) {
        if (frame.previewMaterialDescriptor != VK_NULL_HANDLE && previewDescriptorPool_ != VK_NULL_HANDLE) {
            check(vkFreeDescriptorSets(device_, previewDescriptorPool_, 1, &frame.previewMaterialDescriptor),
                  "free preview material descriptor");
        }
        if (frame.mappedPreviewMaterials != nullptr) {
            vkUnmapMemory(device_, frame.previewMaterialMemory);
        }
        if (frame.previewMaterialBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, frame.previewMaterialBuffer, nullptr);
        }
        if (frame.previewMaterialMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, frame.previewMaterialMemory, nullptr);
        }
        frame.previewMaterialBuffer = VK_NULL_HANDLE;
        frame.previewMaterialMemory = VK_NULL_HANDLE;
        frame.mappedPreviewMaterials = nullptr;
        frame.previewMaterialDescriptor = VK_NULL_HANDLE;
        frame.previewMaterialGeneration = 0;
    }
    previewMaterialSize_ = 0;
    previewMaterialCapacity_ = 0;
    previewMaterialGeneration_ = 0;
}

void VulkanDevice::synchronizePreviewMaterials(Frame& frame) {
    if (frame.previewMaterialGeneration == previewMaterialGeneration_ || previewMaterialSize_ == 0)
        return;
    const auto latest = std::find_if(frames_.begin(), frames_.end(), [this](const Frame& candidate) {
        return candidate.previewMaterialGeneration == previewMaterialGeneration_;
    });
    if (latest == frames_.end() || latest->mappedPreviewMaterials == nullptr)
        return;
    check(vkWaitForFences(device_, 1, &latest->inFlight, VK_TRUE, UINT64_MAX), "wait for latest preview materials");
    std::memcpy(frame.mappedPreviewMaterials, latest->mappedPreviewMaterials,
                static_cast<std::size_t>(previewMaterialSize_));
    frame.previewMaterialGeneration = previewMaterialGeneration_;
}

void VulkanDevice::destroyPreviewIndirectBuffers() {
    for (auto& frame : frames_) {
        if (frame.mappedPreviewIndirect != nullptr)
            vkUnmapMemory(device_, frame.previewIndirectMemory);
        if (frame.previewIndirectBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, frame.previewIndirectBuffer, nullptr);
        if (frame.previewIndirectMemory != VK_NULL_HANDLE)
            vkFreeMemory(device_, frame.previewIndirectMemory, nullptr);
        frame.previewIndirectBuffer = VK_NULL_HANDLE;
        frame.previewIndirectMemory = VK_NULL_HANDLE;
        frame.mappedPreviewIndirect = nullptr;
        frame.previewIndirectGeneration = 0;
    }
    previewIndirectSize_ = 0;
    previewIndirectCapacity_ = 0;
    previewIndirectGeneration_ = 0;
}

void VulkanDevice::synchronizePreviewIndirect(Frame& frame) {
    if (frame.previewIndirectGeneration == previewIndirectGeneration_ || previewIndirectSize_ == 0)
        return;
    const auto latest = std::find_if(frames_.begin(), frames_.end(), [this](const Frame& candidate) {
        return candidate.previewIndirectGeneration == previewIndirectGeneration_;
    });
    if (latest == frames_.end() || latest->mappedPreviewIndirect == nullptr)
        return;
    check(vkWaitForFences(device_, 1, &latest->inFlight, VK_TRUE, UINT64_MAX),
          "wait for latest preview indirect commands");
    std::memcpy(frame.mappedPreviewIndirect, latest->mappedPreviewIndirect,
                static_cast<std::size_t>(previewIndirectSize_));
    frame.previewIndirectGeneration = previewIndirectGeneration_;
}

void VulkanDevice::destroyPreviewMaterialDescriptors() {
    if (previewDescriptorPool_ != VK_NULL_HANDLE && !previewMaterialDescriptors_.empty()) {
        check(vkFreeDescriptorSets(device_, previewDescriptorPool_,
                                   static_cast<std::uint32_t>(previewMaterialDescriptors_.size()),
                                   previewMaterialDescriptors_.data()),
              "free preview material descriptors");
    }
    previewMaterialDescriptors_.clear();
    previewMaterialDescriptorKeys_.clear();
}

void VulkanDevice::refreshPreviewMaterialDescriptors(bool waitForGpu) {
    std::vector<std::array<std::uint32_t, 3>> keys;
    keys.reserve(previewGpuScene_.materials.size());
    for (const auto& material : previewGpuScene_.materials) {
        keys.push_back({material.textureSlot, material.toonTextureSlot, material.sphereTextureSlot});
    }
    if (keys == previewMaterialDescriptorKeys_ &&
        previewMaterialDescriptors_.size() == previewGpuScene_.materials.size())
        return;
    if (waitForGpu)
        waitIdle();
    destroyPreviewMaterialDescriptors();
    if (previewGpuScene_.materials.empty() || previewTextures_.empty())
        return;

    previewMaterialDescriptors_.resize(previewGpuScene_.materials.size());
    const auto layout = previewDescriptorSetLayout_;
    const VkDescriptorSetAllocateInfo setInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = previewDescriptorPool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    };
    for (std::size_t index = 0; index < previewGpuScene_.materials.size(); ++index) {
        auto& material = previewGpuScene_.materials[index];
        check(vkAllocateDescriptorSets(device_, &setInfo, &previewMaterialDescriptors_[index]),
              "allocate preview material descriptor");
        const auto textureView = [&](std::uint32_t slot) {
            const auto clamped = std::min<std::size_t>(slot, previewTextures_.size() - 1U);
            return previewTextures_[clamped].view;
        };
        const std::array<VkDescriptorImageInfo, 3> images{{
            {.imageView = textureView(material.textureSlot), .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {.imageView = textureView(material.toonTextureSlot),
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {.imageView = textureView(material.sphereTextureSlot),
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        }};
        const VkDescriptorImageInfo repeatSampler{.sampler = previewSampler_};
        const VkDescriptorImageInfo clampSampler{.sampler = previewClampSampler_};
        const std::array writes{
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = previewMaterialDescriptors_[index],
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo = images.data(),
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = previewMaterialDescriptors_[index],
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo = images.data() + 1,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = previewMaterialDescriptors_[index],
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .pImageInfo = images.data() + 2,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = previewMaterialDescriptors_[index],
                .dstBinding = 3,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .pImageInfo = &repeatSampler,
            },
            VkWriteDescriptorSet{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = previewMaterialDescriptors_[index],
                .dstBinding = 4,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .pImageInfo = &clampSampler,
            },
        };
        vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    previewMaterialDescriptorKeys_ = std::move(keys);
}

void VulkanDevice::destroyPreviewBindlessDescriptor() {
    if (previewBindlessDescriptor_ != VK_NULL_HANDLE && previewDescriptorPool_ != VK_NULL_HANDLE) {
        check(vkFreeDescriptorSets(device_, previewDescriptorPool_, 1, &previewBindlessDescriptor_),
              "free preview bindless descriptor");
    }
    previewBindlessDescriptor_ = VK_NULL_HANDLE;
}

void VulkanDevice::refreshPreviewBindlessDescriptor() {
    if (previewBindlessDescriptorSetLayout_ == VK_NULL_HANDLE || previewTextures_.empty())
        return;
    if (previewTextures_.size() > previewBindlessTextureCapacity_)
        throw std::runtime_error("preview texture table exceeds Vulkan descriptor capacity");
    destroyPreviewBindlessDescriptor();
    const auto count = static_cast<std::uint32_t>(previewTextures_.size());
    const VkDescriptorSetVariableDescriptorCountAllocateInfo variableCountInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .descriptorSetCount = 1,
        .pDescriptorCounts = &count,
    };
    const VkDescriptorSetAllocateInfo setInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = &variableCountInfo,
        .descriptorPool = previewDescriptorPool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &previewBindlessDescriptorSetLayout_,
    };
    check(vkAllocateDescriptorSets(device_, &setInfo, &previewBindlessDescriptor_),
          "allocate preview bindless descriptor");
    std::vector<VkDescriptorImageInfo> images;
    images.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto& texture = previewTextures_[index];
        images.push_back({.imageView = texture.view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    const VkDescriptorImageInfo repeatSampler{.sampler = previewSampler_};
    const VkDescriptorImageInfo clampSampler{.sampler = previewClampSampler_};
    const std::array writes{
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = previewBindlessDescriptor_,
            .dstBinding = 2,
            .descriptorCount = count,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pImageInfo = images.data(),
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = previewBindlessDescriptor_,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .pImageInfo = &repeatSampler,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = previewBindlessDescriptor_,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .pImageInfo = &clampSampler,
        },
    };
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void VulkanDevice::destroyPreviewBackground() {
    for (auto& frame : frames_) {
        if (frame.mappedBackgroundStaging != nullptr) {
            vkUnmapMemory(device_, frame.backgroundStagingMemory);
        }
        if (frame.backgroundStagingBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, frame.backgroundStagingBuffer, nullptr);
        }
        if (frame.backgroundStagingMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, frame.backgroundStagingMemory, nullptr);
        }
        frame.backgroundStagingBuffer = VK_NULL_HANDLE;
        frame.backgroundStagingMemory = VK_NULL_HANDLE;
        frame.mappedBackgroundStaging = nullptr;
        frame.backgroundUploadPending = false;
    }
    destroyPreviewTextureResource(previewBackgroundTexture_);
    if (previewBackgroundIndexBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, previewBackgroundIndexBuffer_, nullptr);
    }
    if (previewBackgroundIndexMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, previewBackgroundIndexMemory_, nullptr);
    }
    if (previewBackgroundVertexBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, previewBackgroundVertexBuffer_, nullptr);
    }
    if (previewBackgroundVertexMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, previewBackgroundVertexMemory_, nullptr);
    }
    previewBackgroundVertexBuffer_ = VK_NULL_HANDLE;
    previewBackgroundVertexMemory_ = VK_NULL_HANDLE;
    previewBackgroundIndexBuffer_ = VK_NULL_HANDLE;
    previewBackgroundIndexMemory_ = VK_NULL_HANDLE;
    previewBackgroundIndexCount_ = 0;
    previewBackgroundExtent_ = {};
    previewBackgroundByteSize_ = 0;
    previewBackgroundInitialized_ = false;
}

void VulkanDevice::uploadPreviewBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                                       VkDeviceMemory& memory, VkDeviceSize allocationSize) {
    if (data == nullptr || size == 0)
        throw std::invalid_argument("preview buffer is empty");
    const auto storageSize = allocationSize == 0 ? size : allocationSize;
    if (storageSize < size)
        throw std::invalid_argument("preview buffer allocation is smaller than its data");
    const VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = storageSize,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    check(vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer), "create preview mesh buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer, &requirements);
    const VkMemoryAllocateInfo allocationInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    check(vkAllocateMemory(device_, &allocationInfo, nullptr, &memory), "allocate preview mesh memory");
    check(vkBindBufferMemory(device_, buffer, memory, 0), "bind preview mesh memory");
    void* mapped = nullptr;
    check(vkMapMemory(device_, memory, 0, storageSize, 0, &mapped), "map preview mesh memory");
    std::memcpy(mapped, data, static_cast<std::size_t>(size));
    vkUnmapMemory(device_, memory);
}

void VulkanDevice::uploadPreviewDeviceLocalBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
                                                  VkBuffer& buffer, VkDeviceMemory& memory) {
    if (data == nullptr || size == 0)
        throw std::invalid_argument("preview device-local buffer is empty");

    try {
        const VkBufferCreateInfo bufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        check(vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer), "create device-local preview buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, buffer, &requirements);
        const VkMemoryAllocateInfo allocationInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };
        check(vkAllocateMemory(device_, &allocationInfo, nullptr, &memory), "allocate device-local preview buffer");
        check(vkBindBufferMemory(device_, buffer, memory, 0), "bind device-local preview buffer");

        uploadContext_->begin();
        const auto slice = uploadContext_->allocate(size);
        std::memcpy(slice.mapped, data, static_cast<std::size_t>(size));
        const VkBufferCopy copy{.srcOffset = slice.offset, .size = size};
        vkCmdCopyBuffer(uploadContext_->commandBuffer(), slice.buffer, buffer, 1, &copy);
        const VkBufferMemoryBarrier2 visible{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffer,
            .offset = 0,
            .size = size,
        };
        const VkDependencyInfo visibleDependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &visible,
        };
        vkCmdPipelineBarrier2(uploadContext_->commandBuffer(), &visibleDependency);
        static_cast<void>(uploadContext_->submit());
    } catch (...) {
        uploadContext_->abort();
        if (buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, buffer, nullptr);
        if (memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        throw;
    }
}

void VulkanDevice::uploadPreviewMesh(std::span<const PreviewVertex> vertices, std::span<const std::uint32_t> indices) {
    if (vertices.empty() || indices.empty())
        throw std::invalid_argument("preview mesh is empty");
    waitIdle();
    destroyPreviewMesh();

    try {
        uploadPreviewDeviceLocalBuffer(vertices.data(), vertices.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       previewStaticVertexBuffer_, previewStaticVertexMemory_);
        ++previewVertexGeneration_;
        for (auto& frame : frames_)
            frame.previewVertexGeneration = previewVertexGeneration_;
        previewVertexSize_ = vertices.size_bytes();
        previewVertexCapacity_ = vertices.size_bytes();
        uploadPreviewDeviceLocalBuffer(indices.data(), indices.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                       previewIndexBuffer_, previewIndexMemory_);
        previewIndexCount_ = static_cast<std::uint32_t>(indices.size());
    } catch (...) {
        uploadContext_->abort();
        const auto uploadValue = uploadContext_->lastSubmittedValue();
        if (uploadValue != 0)
            uploadContext_->wait(uploadValue);
        destroyPreviewMesh();
        throw;
    }
    log::info("Uploaded PMX preview mesh: ", vertices.size(), " vertices, ", indices.size() / 3, " triangles");
}

void VulkanDevice::uploadPreviewBackground(std::span<const PreviewTexture> textures) {
    if (textures.empty()) {
        if (previewBackgroundTexture_.image != VK_NULL_HANDLE) {
            waitIdle();
            destroyPreviewBackground();
        }
        return;
    }
    const auto& texture = textures.front();
    const auto byteSize = static_cast<VkDeviceSize>(texture.width) * texture.height * 4U;
    if (texture.width == 0 || texture.height == 0 || texture.rgba.size_bytes() != byteSize) {
        throw std::invalid_argument("preview background texture is empty");
    }
    if (previewBackgroundExtent_.width != texture.width || previewBackgroundExtent_.height != texture.height) {
        waitIdle();
        destroyPreviewBackground();
        try {
            createPreviewBackgroundStream(texture.width, texture.height);
        } catch (...) {
            destroyPreviewBackground();
            throw;
        }
    }
    auto& frame = frames_[frameIndex_];
    check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait for streaming background frame");
    std::memcpy(frame.mappedBackgroundStaging, texture.rgba.data(), texture.rgba.size_bytes());
    frame.backgroundUploadPending = true;
}

void VulkanDevice::updatePreviewVertices(std::span<const PreviewVertex> vertices) {
    if (vertices.empty() || vertices.size_bytes() > previewVertexCapacity_)
        throw std::invalid_argument("preview vertex update exceeds its capacity");
    previewVertexSize_ = vertices.size_bytes();

    if (previewStaticVertexBuffer_ != VK_NULL_HANDLE) {
        waitIdle();
        previewVertexCapacity_ = growPreviewCapacity(vertices.size_bytes());
        for (auto& frame : frames_) {
            uploadPreviewBuffer(vertices.data(), vertices.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                frame.previewVertexBuffer, frame.previewVertexMemory, previewVertexCapacity_);
            check(vkMapMemory(device_, frame.previewVertexMemory, 0, vertices.size_bytes(), 0,
                              &frame.mappedPreviewVertices),
                  "persistently map animated preview vertices");
            frame.previewVertexGeneration = previewVertexGeneration_;
        }
        vkDestroyBuffer(device_, previewStaticVertexBuffer_, nullptr);
        vkFreeMemory(device_, previewStaticVertexMemory_, nullptr);
        previewStaticVertexBuffer_ = VK_NULL_HANDLE;
        previewStaticVertexMemory_ = VK_NULL_HANDLE;
    }

    auto& frame = frames_[frameIndex_];
    if (frame.mappedPreviewVertices == nullptr)
        throw std::logic_error("preview vertex storage is unavailable");
    check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait for animated vertex frame");
    std::memcpy(frame.mappedPreviewVertices, vertices.data(), vertices.size_bytes());
    frame.previewVertexGeneration = ++previewVertexGeneration_;
    ++previewVertexUpdateCount_;
    if (previewVertexUpdateCount_ % 30 == 1) {
        const auto& vertex = vertices.front().position;
        log::debug("Updated animated preview vertex buffer: vertices=", vertices.size(), ", vertex0=(", vertex[0], ",",
                   vertex[1], ",", vertex[2], ")");
    }
}

void VulkanDevice::updatePreviewBones(std::span<const PreviewBoneTransform> bones) {
    const std::array<PreviewBoneTransform, 1> identity{};
    if (bones.empty())
        bones = identity;
    const auto byteSize = static_cast<VkDeviceSize>(bones.size_bytes());
    if (byteSize > previewBoneCapacity_ || frames_.front().previewBoneBuffer == VK_NULL_HANDLE) {
        waitIdle();
        destroyPreviewBones();
        previewBoneSize_ = byteSize;
        previewBoneCapacity_ = growPreviewCapacity(byteSize);
        ++previewBoneGeneration_;
        for (auto& frame : frames_) {
            uploadPreviewBuffer(bones.data(), byteSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, frame.previewBoneBuffer,
                                frame.previewBoneMemory, previewBoneCapacity_);
            check(vkMapMemory(device_, frame.previewBoneMemory, 0, byteSize, 0, &frame.mappedPreviewBones),
                  "persistently map preview bones");
            const VkDescriptorSetAllocateInfo setInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = previewDescriptorPool_,
                .descriptorSetCount = 1,
                .pSetLayouts = &previewSkinningDescriptorSetLayout_,
            };
            check(vkAllocateDescriptorSets(device_, &setInfo, &frame.previewBoneDescriptor),
                  "allocate preview bone descriptor");
            const VkDescriptorBufferInfo bufferInfo{frame.previewBoneBuffer, 0, previewBoneCapacity_};
            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = frame.previewBoneDescriptor,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &bufferInfo,
            };
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
            frame.previewBoneGeneration = previewBoneGeneration_;
        }
        return;
    }
    previewBoneSize_ = byteSize;
    auto& frame = frames_[frameIndex_];
    check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait for preview bone frame");
    std::memcpy(frame.mappedPreviewBones, bones.data(), bones.size_bytes());
    frame.previewBoneGeneration = ++previewBoneGeneration_;
}

void VulkanDevice::updatePreviewMaterials(std::span<const PreviewMaterial> materials) {
    const auto unchanged = std::numeric_limits<std::size_t>::max();
    std::size_t firstDirty = unchanged;
    std::size_t lastDirty = 0;
    if (previewGpuScene_.materials.size() != materials.size()) {
        firstDirty = 0;
        lastDirty = std::max(previewGpuScene_.materials.size(), materials.size()) - 1U;
    } else {
        for (std::size_t index = 0; index < materials.size(); ++index) {
            if (samePreviewMaterial(previewGpuScene_.materials[index], materials[index]))
                continue;
            firstDirty = std::min(firstDirty, index);
            lastDirty = index;
        }
    }
    previewGpuScene_.materials.assign(materials.begin(), materials.end());
    refreshPreviewMaterialDescriptors();

    previewGpuScene_.materialData.clear();
    previewGpuScene_.materialData.reserve(std::max<std::size_t>(previewGpuScene_.materials.size(), 1U));
    for (const auto& material : previewGpuScene_.materials) {
        PreviewMaterialGpu gpu;
        std::copy_n(material.diffuse, 4, gpu.diffuse);
        std::copy_n(material.ambient, 3, gpu.ambientShininess);
        gpu.ambientShininess[3] = material.shininess;
        std::copy_n(material.specular, 3, gpu.specular);
        std::copy_n(material.textureMultiply, 4, gpu.textureMultiply);
        std::copy_n(material.textureAdd, 4, gpu.textureAdd);
        std::copy_n(material.sphereMultiply, 4, gpu.sphereMultiply);
        std::copy_n(material.sphereAdd, 4, gpu.sphereAdd);
        std::copy_n(material.toonMultiply, 4, gpu.toonMultiply);
        std::copy_n(material.toonAdd, 4, gpu.toonAdd);
        std::copy_n(material.edgeColor, 4, gpu.edgeColor);
        gpu.edgeSize = material.edgeSize;
        gpu.flags = (material.doubleSided ? 0x01U : 0U) | ((material.toonMode & 0x03U) << 1U) |
                    ((material.sphereMode & 0x03U) << 3U) | (material.edgeEnabled ? 0x20U : 0U);
        gpu.textureSlots[0] = material.textureSlot;
        gpu.textureSlots[1] = material.toonTextureSlot;
        gpu.textureSlots[2] = material.sphereTextureSlot;
        previewGpuScene_.materialData.push_back(gpu);
    }
    if (previewGpuScene_.materialData.empty()) {
        PreviewMaterialGpu fallback;
        fallback.diffuse[0] = fallback.diffuse[1] = fallback.diffuse[2] = fallback.diffuse[3] = 1.0F;
        fallback.ambientShininess[0] = fallback.ambientShininess[1] = fallback.ambientShininess[2] = 1.0F;
        fallback.textureMultiply[0] = fallback.textureMultiply[1] = fallback.textureMultiply[2] =
            fallback.textureMultiply[3] = 1.0F;
        previewGpuScene_.materialData.push_back(fallback);
    }
    const auto byteSize = static_cast<VkDeviceSize>(previewGpuScene_.materialData.size() * sizeof(PreviewMaterialGpu));
    if (byteSize > previewMaterialCapacity_ || frames_.front().previewMaterialBuffer == VK_NULL_HANDLE) {
        waitIdle();
        destroyPreviewMaterialBuffers();
        previewMaterialSize_ = byteSize;
        previewMaterialCapacity_ = growPreviewCapacity(byteSize);
        ++previewMaterialGeneration_;
        for (auto& frame : frames_) {
            uploadPreviewBuffer(previewGpuScene_.materialData.data(), byteSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                frame.previewMaterialBuffer, frame.previewMaterialMemory, previewMaterialCapacity_);
            check(vkMapMemory(device_, frame.previewMaterialMemory, 0, byteSize, 0, &frame.mappedPreviewMaterials),
                  "persistently map preview materials");
            const VkDescriptorSetAllocateInfo setInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = previewDescriptorPool_,
                .descriptorSetCount = 1,
                .pSetLayouts = &previewMaterialDescriptorSetLayout_,
            };
            check(vkAllocateDescriptorSets(device_, &setInfo, &frame.previewMaterialDescriptor),
                  "allocate preview material buffer descriptor");
            const VkDescriptorBufferInfo bufferInfo{
                frame.previewMaterialBuffer,
                0,
                previewMaterialCapacity_,
            };
            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = frame.previewMaterialDescriptor,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &bufferInfo,
            };
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
            frame.previewMaterialGeneration = previewMaterialGeneration_;
        }
    } else {
        previewMaterialSize_ = byteSize;
        if (firstDirty == unchanged)
            return;
        const auto first = std::min(firstDirty, previewGpuScene_.materialData.size() - 1U);
        const auto last = std::min(lastDirty, previewGpuScene_.materialData.size() - 1U);
        const auto offset = first * sizeof(PreviewMaterialGpu);
        const auto size = (last - first + 1U) * sizeof(PreviewMaterialGpu);
        auto& frame = frames_[frameIndex_];
        check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait for preview material frame");
        std::memcpy(static_cast<std::byte*>(frame.mappedPreviewMaterials) + offset,
                    previewGpuScene_.materialData.data() + first, size);
        frame.previewMaterialGeneration = ++previewMaterialGeneration_;
    }
}

void VulkanDevice::updatePreviewDraws(std::span<const PreviewDraw> draws) {
    previewGpuScene_.draws.assign(draws.begin(), draws.end());
    previewIndirectCommands_.resize(draws.size());
    for (std::size_t index = 0; index < draws.size(); ++index) {
        const auto& draw = draws[index];
        previewIndirectCommands_[index] = {
            .indexCount = draw.firstIndex >= previewIndexCount_
                              ? 0U
                              : std::min(draw.indexCount, previewIndexCount_ - draw.firstIndex),
            .instanceCount = std::max(draw.instanceCount, 1U),
            .firstIndex = draw.firstIndex,
            .vertexOffset = 0,
            .firstInstance = draw.materialIndex,
        };
    }
    if (previewIndirectCommands_.empty()) {
        previewIndirectSize_ = 0;
        return;
    }
    const auto byteSize =
        static_cast<VkDeviceSize>(previewIndirectCommands_.size() * sizeof(VkDrawIndexedIndirectCommand));
    if (byteSize > previewIndirectCapacity_ || frames_.front().previewIndirectBuffer == VK_NULL_HANDLE) {
        waitIdle();
        destroyPreviewIndirectBuffers();
        previewIndirectSize_ = byteSize;
        previewIndirectCapacity_ = growPreviewCapacity(byteSize);
        ++previewIndirectGeneration_;
        for (auto& frame : frames_) {
            uploadPreviewBuffer(previewIndirectCommands_.data(), byteSize, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                frame.previewIndirectBuffer, frame.previewIndirectMemory, previewIndirectCapacity_);
            check(vkMapMemory(device_, frame.previewIndirectMemory, 0, previewIndirectCapacity_, 0,
                              &frame.mappedPreviewIndirect),
                  "persistently map preview indirect commands");
            frame.previewIndirectGeneration = previewIndirectGeneration_;
        }
        return;
    }
    previewIndirectSize_ = byteSize;
    auto& frame = frames_[frameIndex_];
    check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait for preview indirect frame");
    std::memcpy(frame.mappedPreviewIndirect, previewIndirectCommands_.data(),
                static_cast<std::size_t>(previewIndirectSize_));
    frame.previewIndirectGeneration = ++previewIndirectGeneration_;
}

void VulkanDevice::uploadPreviewTextures(std::span<const PreviewTexture> textures) {
    if (textures.size() >= static_cast<std::size_t>(previewBindlessTextureCapacity_))
        throw std::runtime_error("preview texture table exceeds Vulkan descriptor capacity");
    waitIdle();
    destroyPreviewTextures();
    const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    try {
        createPreviewTexture(1, 1, white);
        for (const auto& texture : textures) {
            if (texture.width == 0 || texture.height == 0 || texture.rgba.empty()) {
                // Preserve source texture numbering with a white placeholder.
                createPreviewTexture(1, 1, white);
            } else {
                createPreviewTexture(texture.width, texture.height, texture.rgba);
            }
        }
        refreshPreviewMaterialDescriptors(false);
        refreshPreviewBindlessDescriptor();
    } catch (...) {
        uploadContext_->abort();
        const auto uploadValue = uploadContext_->lastSubmittedValue();
        if (uploadValue != 0)
            uploadContext_->wait(uploadValue);
        destroyPreviewTextures();
        throw;
    }
    log::info("Uploaded ", textures.size(), " PMX texture(s) plus fallback");
}

void VulkanDevice::clearPreviewResources() {
    waitIdle();
    const std::array<PreviewVertex, 3> fallbackVertices{{
        {{0.0F, -0.65F, 0.0F}, {}, {}},
        {{0.65F, 0.55F, 0.0F}, {}, {}},
        {{-0.65F, 0.55F, 0.0F}, {}, {}},
    }};
    const std::array<std::uint32_t, 3> fallbackIndices{0, 1, 2};
    uploadPreviewMesh(fallbackVertices, fallbackIndices);
    const std::array<PreviewBoneTransform, 1> identityBones{};
    updatePreviewBones(identityBones);
    uploadPreviewMorphDeltas(std::span<const PreviewMorphDelta>{});
    updatePreviewMorphWeights(std::span<const float>{});
    destroyPreviewBackground();
    uploadPreviewTextures(std::span<const PreviewTexture>{});
    updatePreviewMaterials(std::span<const PreviewMaterial>{});
    previewGpuScene_.draws.clear();
    previewGpuScene_.view = {};
    previewGpuScene_.view.screenSource = PreviewScene::ScreenSource::white;
    std::fill(swapchainInitialized_.begin(), swapchainInitialized_.end(), false);
    log::info("Cleared preview GPU resources");
}

void VulkanDevice::createTypedDescriptorPool() {
    std::vector<VkDescriptorPoolSize> sizes{
        {VK_DESCRIPTOR_TYPE_SAMPLER, 4096},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
    };
    if (capabilities_.accelerationStructure)
        sizes.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1024});

    const VkDescriptorPoolCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 4096,
        .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
        .pPoolSizes = sizes.data(),
    };
    check(vkCreateDescriptorPool(device_, &createInfo, nullptr, &typedDescriptorPool_), "create typed descriptor pool");
}

handles::BufferHandle VulkanDevice::createBufferEx(const BufferResourceDesc& desc) {
    if (desc.size == 0)
        throw std::invalid_argument("typed buffer size must be non-zero");
    if (toBits(desc.usage) == 0)
        throw std::invalid_argument("typed buffer usage must be non-zero");
    constexpr auto kAccelerationUsage = toBits(ResourceUsage::asBuildRead) | toBits(ResourceUsage::asBuildWrite);
    if ((toBits(desc.usage) & kAccelerationUsage) != 0U && !capabilities_.accelerationStructure)
        throw std::runtime_error("typed buffer requests acceleration-structure usage on unsupported Vulkan device");

    TypedBuffer typed{};
    typed.desc = desc;
    typed.resource.size = static_cast<VkDeviceSize>(desc.size);
    const VkBufferUsageFlags usage = toVkUsage(desc.usage, capabilities_.bufferDeviceAddress);
    if (usage == 0)
        throw std::invalid_argument("typed buffer usage has no Vulkan mapping");
    const VkBufferCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = typed.resource.size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    check(vkCreateBuffer(device_, &createInfo, nullptr, &typed.resource.buffer), "create typed buffer");
    try {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, typed.resource.buffer, &requirements);
        const VkMemoryPropertyFlags memoryFlags =
            desc.cpuVisible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VkMemoryAllocateFlagsInfo allocationFlags{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = capabilities_.bufferDeviceAddress
                         ? static_cast<VkMemoryAllocateFlags>(VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT)
                         : static_cast<VkMemoryAllocateFlags>(0),
        };
        const VkMemoryAllocateInfo allocationInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = capabilities_.bufferDeviceAddress ? &allocationFlags : nullptr,
            .allocationSize = requirements.size,
            .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, memoryFlags),
        };
        check(vkAllocateMemory(device_, &allocationInfo, nullptr, &typed.resource.memory),
              "allocate typed buffer memory");
        check(vkBindBufferMemory(device_, typed.resource.buffer, typed.resource.memory, 0), "bind typed buffer memory");
        if (desc.cpuVisible)
            check(vkMapMemory(device_, typed.resource.memory, 0, typed.resource.size, 0, &typed.mapped),
                  "map typed buffer");
    } catch (...) {
        if (typed.mapped != nullptr)
            vkUnmapMemory(device_, typed.resource.memory);
        if (typed.resource.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, typed.resource.memory, nullptr);
        if (typed.resource.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, typed.resource.buffer, nullptr);
        throw;
    }

    const auto handle = typedBufferHandles_.create();
    typedBuffers_.emplace(handle, typed);
    return handle;
}

handles::TextureHandle VulkanDevice::createTextureEx(const TextureResourceDesc& desc) {
    if (!isValidTextureDesc(desc))
        throw std::invalid_argument("invalid typed texture description");
    const VkFormat format = toVkFormat(desc.format);
    if (format == VK_FORMAT_UNDEFINED)
        throw std::invalid_argument("typed texture format has no Vulkan mapping");
    const VkImageUsageFlags usage = toVkUsage(desc.usage);
    if (usage == 0)
        throw std::invalid_argument("typed texture usage has no Vulkan mapping");

    TypedTexture typed{};
    typed.desc = desc;
    const std::uint32_t layerMultiplier = desc.dimension == TextureDimension::cube ? 6U : 1U;
    const std::uint32_t imageLayers = desc.arrayLayers * layerMultiplier;
    const VkImageCreateFlags imageFlags = desc.dimension == TextureDimension::cube
                                              ? static_cast<VkImageCreateFlags>(VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
                                              : static_cast<VkImageCreateFlags>(0);
    const VkImageCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = imageFlags,
        .imageType = toVkImageType(desc.dimension),
        .format = format,
        .extent = {desc.extent.width, desc.extent.height, desc.extent.depth},
        .mipLevels = desc.mipLevels,
        .arrayLayers = imageLayers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    check(vkCreateImage(device_, &createInfo, nullptr, &typed.resource.image), "create typed texture");
    try {
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, typed.resource.image, &requirements);
        const VkMemoryAllocateInfo allocationInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };
        check(vkAllocateMemory(device_, &allocationInfo, nullptr, &typed.resource.memory),
              "allocate typed texture memory");
        check(vkBindImageMemory(device_, typed.resource.image, typed.resource.memory, 0), "bind typed texture memory");

        const VkImageAspectFlags aspect =
            desc.format == PixelFormat::depth32Float ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = typed.resource.image,
            .viewType = toVkImageViewType(desc.dimension, desc.arrayLayers),
            .format = format,
            .subresourceRange = {aspect, 0, desc.mipLevels, 0, imageLayers},
        };
        check(vkCreateImageView(device_, &viewInfo, nullptr, &typed.view), "create typed texture view");
    } catch (...) {
        if (typed.view != VK_NULL_HANDLE)
            vkDestroyImageView(device_, typed.view, nullptr);
        if (typed.resource.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, typed.resource.memory, nullptr);
        if (typed.resource.image != VK_NULL_HANDLE)
            vkDestroyImage(device_, typed.resource.image, nullptr);
        throw;
    }

    const auto handle = typedTextureHandles_.create();
    typedTextures_.emplace(handle, typed);
    return handle;
}

handles::SamplerHandle VulkanDevice::createSamplerEx() {
    return createSamplerEx(SamplerResourceDesc{});
}

handles::SamplerHandle VulkanDevice::createSamplerEx(const SamplerResourceDesc& desc) {
    const auto filter = desc.filter == SamplerFilter::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    const auto addressMode = [](SamplerAddressMode mode) {
        switch (mode) {
        case SamplerAddressMode::repeat:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case SamplerAddressMode::clampToEdge:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case SamplerAddressMode::mirroredRepeat:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case SamplerAddressMode::clampToBorder:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        }
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    };
    const VkSamplerCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = filter,
        .minFilter = filter,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = addressMode(desc.addressU),
        .addressModeV = addressMode(desc.addressV),
        .addressModeW = addressMode(desc.addressW),
        .mipLodBias = desc.mipLodBias,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0F,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = desc.minLod,
        .maxLod = desc.maxLod == std::numeric_limits<float>::max() ? VK_LOD_CLAMP_NONE : desc.maxLod,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    TypedSampler typed{};
    check(vkCreateSampler(device_, &createInfo, nullptr, &typed.sampler), "create typed sampler");
    const auto handle = typedSamplerHandles_.create();
    typedSamplers_.emplace(handle, typed);
    return handle;
}

void VulkanDevice::destroySamplerEx(handles::SamplerHandle handle) {
    const auto it = typedSamplers_.find(handle);
    if (it == typedSamplers_.end() || !typedSamplerHandles_.isAlive(handle))
        throw std::invalid_argument("stale typed sampler handle");
    if (it->second.sampler != VK_NULL_HANDLE)
        vkDestroySampler(device_, it->second.sampler, nullptr);
    typedSamplers_.erase(it);
    typedSamplerHandles_.destroy(handle);
}

handles::ShaderHandle VulkanDevice::createShaderEx(const ShaderDesc& desc) {
    if (desc.spirv.empty())
        throw std::invalid_argument("typed shader SPIR-V must be non-empty");
    if (desc.entryPoint.empty())
        throw std::invalid_argument("typed shader entry point must be non-empty");
    static_cast<void>(toVkShaderStage(desc.stage));

    const VkShaderModuleCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = desc.spirv.size_bytes(),
        .pCode = desc.spirv.data(),
    };
    TypedShader typed{.stage = desc.stage, .entryPoint = desc.entryPoint};
    check(vkCreateShaderModule(device_, &createInfo, nullptr, &typed.module), "create typed shader module");
    const auto handle = typedShaderHandles_.create();
    typedShaders_.emplace(handle, std::move(typed));
    return handle;
}

void VulkanDevice::destroyShaderEx(handles::ShaderHandle handle) {
    const auto it = typedShaders_.find(handle);
    if (it == typedShaders_.end() || !typedShaderHandles_.isAlive(handle))
        throw std::invalid_argument("stale typed shader handle");
    if (it->second.module != VK_NULL_HANDLE)
        vkDestroyShaderModule(device_, it->second.module, nullptr);
    typedShaders_.erase(it);
    typedShaderHandles_.destroy(handle);
}

handles::PipelineLayoutHandle VulkanDevice::createPipelineLayoutEx(const PipelineLayoutDesc& desc) {
    std::vector<VkDescriptorSetLayout> setLayouts;
    setLayouts.reserve(desc.setLayouts.size());
    for (const auto handle : desc.setLayouts) {
        const auto it = typedDescriptorSetLayouts_.find(handle);
        if (it == typedDescriptorSetLayouts_.end() || !typedDescriptorSetLayoutHandles_.isAlive(handle))
            throw std::invalid_argument("pipeline layout references a stale descriptor set layout");
        setLayouts.push_back(it->second.layout);
    }
    std::vector<VkPushConstantRange> pushConstants;
    pushConstants.reserve(desc.pushConstants.size());
    for (const auto& range : desc.pushConstants) {
        if (range.size == 0 || range.stages == ShaderStageMask::none ||
            range.offset > std::numeric_limits<std::uint32_t>::max() - range.size)
            throw std::invalid_argument("invalid typed push-constant range");
        pushConstants.push_back({toVkShaderStages(range.stages), range.offset, range.size});
    }
    const VkPipelineLayoutCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = static_cast<std::uint32_t>(setLayouts.size()),
        .pSetLayouts = setLayouts.data(),
        .pushConstantRangeCount = static_cast<std::uint32_t>(pushConstants.size()),
        .pPushConstantRanges = pushConstants.data(),
    };
    TypedPipelineLayout typed{.desc = desc};
    check(vkCreatePipelineLayout(device_, &createInfo, nullptr, &typed.layout), "create typed pipeline layout");
    const auto handle = typedPipelineLayoutHandles_.create();
    typedPipelineLayouts_.emplace(handle, std::move(typed));
    return handle;
}

void VulkanDevice::destroyPipelineLayoutEx(handles::PipelineLayoutHandle handle) {
    const auto it = typedPipelineLayouts_.find(handle);
    if (it == typedPipelineLayouts_.end() || !typedPipelineLayoutHandles_.isAlive(handle))
        throw std::invalid_argument("stale typed pipeline layout handle");
    if (it->second.layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, it->second.layout, nullptr);
    typedPipelineLayouts_.erase(it);
    typedPipelineLayoutHandles_.destroy(handle);
}

handles::PipelineHandle VulkanDevice::createComputePipelineEx(const ComputePipelineDescEx& desc) {
    const auto layoutIt = typedPipelineLayouts_.find(desc.layout);
    if (layoutIt == typedPipelineLayouts_.end() || !typedPipelineLayoutHandles_.isAlive(desc.layout))
        throw std::invalid_argument("compute pipeline references a stale pipeline layout");
    if (desc.shaders.size() != 1)
        throw std::invalid_argument("compute pipeline requires exactly one shader");
    const auto shaderIt = typedShaders_.find(desc.shaders.front());
    if (shaderIt == typedShaders_.end() || !typedShaderHandles_.isAlive(desc.shaders.front()) ||
        shaderIt->second.stage != ShaderStageMask::compute)
        throw std::invalid_argument("compute pipeline references a non-compute shader");
    const VkPipelineShaderStageCreateInfo stage{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = shaderIt->second.module,
        .pName = shaderIt->second.entryPoint.c_str(),
    };
    const VkComputePipelineCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = stage,
        .layout = layoutIt->second.layout,
    };
    TypedPipeline typed{.layout = desc.layout, .bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE};
    check(vkCreateComputePipelines(device_, pipelineCache_, 1, &createInfo, nullptr, &typed.pipeline),
          "create typed compute pipeline");
    const auto handle = typedPipelineHandles_.create();
    typedPipelines_.emplace(handle, typed);
    return handle;
}

handles::PipelineHandle VulkanDevice::createGraphicsPipelineEx(const GraphicsPipelineDescEx& desc) {
    const auto layoutIt = typedPipelineLayouts_.find(desc.layout);
    if (layoutIt == typedPipelineLayouts_.end() || !typedPipelineLayoutHandles_.isAlive(desc.layout))
        throw std::invalid_argument("graphics pipeline references a stale pipeline layout");
    if (desc.shaders.size() != 2)
        throw std::invalid_argument("native graphics pipeline requires vertex and fragment shaders");
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    stages.reserve(desc.shaders.size());
    bool hasVertex = false;
    bool hasFragment = false;
    for (const auto handle : desc.shaders) {
        const auto shaderIt = typedShaders_.find(handle);
        if (shaderIt == typedShaders_.end() || !typedShaderHandles_.isAlive(handle))
            throw std::invalid_argument("graphics pipeline references a stale shader");
        if (shaderIt->second.stage == ShaderStageMask::vertex)
            hasVertex = true;
        else if (shaderIt->second.stage == ShaderStageMask::fragment)
            hasFragment = true;
        else
            throw std::invalid_argument("graphics pipeline accepts only vertex and fragment shaders");
        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                          toVkShaderStage(shaderIt->second.stage), shaderIt->second.module,
                          shaderIt->second.entryPoint.c_str(), nullptr});
    }
    if (!hasVertex || !hasFragment)
        throw std::invalid_argument("graphics pipeline requires vertex and fragment shaders");
    std::vector<PixelFormat> colorFormats = desc.colorFormats;
    if (colorFormats.empty() && !desc.depthOnly)
        colorFormats.push_back(desc.colorFormat);
    if (colorFormats.empty() && !desc.depthOnly)
        throw std::invalid_argument("native graphics pipeline requires a color or depth attachment format");
    if (desc.depthOnly && !desc.depthFormat.has_value())
        throw std::invalid_argument("depth-only graphics pipeline requires a depth attachment format");
    std::vector<VkFormat> vkColorFormats;
    vkColorFormats.reserve(colorFormats.size());
    for (const auto format : colorFormats) {
        const auto vkFormat = toVkFormat(format);
        if (vkFormat == VK_FORMAT_UNDEFINED || format == PixelFormat::depth32Float)
            throw std::invalid_argument("native graphics pipeline color format is invalid");
        vkColorFormats.push_back(vkFormat);
    }
    VkFormat vkDepthFormat = VK_FORMAT_UNDEFINED;
    if (desc.depthFormat.has_value()) {
        vkDepthFormat = toVkFormat(*desc.depthFormat);
        if (vkDepthFormat == VK_FORMAT_UNDEFINED || *desc.depthFormat != PixelFormat::depth32Float)
            throw std::invalid_argument("native graphics pipeline depth format is invalid");
    }
    if (desc.depthStencil.depthTest && vkDepthFormat == VK_FORMAT_UNDEFINED)
        throw std::invalid_argument("depth testing requires a depth attachment format");
    std::vector<BlendAttachmentStateEx> blendStates(colorFormats.size());
    if (!desc.blendAttachments.empty()) {
        if (desc.blendAttachments.size() != colorFormats.size())
            throw std::invalid_argument("blend attachment count does not match color attachment count");
        blendStates = desc.blendAttachments;
    }
    std::vector<VkPipelineColorBlendAttachmentState> vkBlendAttachments;
    vkBlendAttachments.reserve(blendStates.size());
    for (const auto& state : blendStates) {
        vkBlendAttachments.push_back({
            .blendEnable = state.enabled ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = toVkBlendFactor(state.srcColor),
            .dstColorBlendFactor = toVkBlendFactor(state.dstColor),
            .colorBlendOp = toVkBlendOp(state.colorOp),
            .srcAlphaBlendFactor = toVkBlendFactor(state.srcAlpha),
            .dstAlphaBlendFactor = toVkBlendFactor(state.dstAlpha),
            .alphaBlendOp = toVkBlendOp(state.alphaOp),
            .colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        });
    }
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = toVkCullMode(desc.rasterizer.cullMode),
        .frontFace = toVkFrontFace(desc.rasterizer.frontFace),
        .lineWidth = 1.0F,
    };
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = static_cast<std::uint32_t>(vkBlendAttachments.size()),
        .pAttachments = vkBlendAttachments.data(),
    };
    const VkPipelineDepthStencilStateCreateInfo depthStencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = desc.depthStencil.depthTest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = desc.depthStencil.depthWrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = toVkCompareOp(desc.depthStencil.depthCompare),
    };
    const std::array dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data(),
    };
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = static_cast<std::uint32_t>(vkColorFormats.size()),
        .pColorAttachmentFormats = vkColorFormats.data(),
        .depthAttachmentFormat = vkDepthFormat,
    };
    const VkGraphicsPipelineCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = static_cast<std::uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = layoutIt->second.layout,
    };
    TypedPipeline typed{.layout = desc.layout, .bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS};
    check(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &createInfo, nullptr, &typed.pipeline),
          "create typed graphics pipeline");
    const auto handle = typedPipelineHandles_.create();
    typedPipelines_.emplace(handle, typed);
    return handle;
}

handles::PipelineHandle VulkanDevice::createRayTracingPipelineEx(const RayTracingPipelineDescEx& desc) {
    if (!capabilities_.rayTracingPipeline)
        throw std::runtime_error("ray-tracing pipeline is unsupported by this Vulkan device");
    const auto layoutIt = typedPipelineLayouts_.find(desc.layout);
    if (layoutIt == typedPipelineLayouts_.end() || !typedPipelineLayoutHandles_.isAlive(desc.layout))
        throw std::invalid_argument("ray-tracing pipeline references a stale pipeline layout");
    if (desc.rayGeneration.empty() || desc.maxRecursionDepth == 0 ||
        desc.maxRecursionDepth > rayTracingPipelineProperties_.maxRayRecursionDepth)
        throw std::invalid_argument("invalid ray-tracing pipeline shader or recursion depth");
    if (!desc.hitGroups.empty() && !desc.closestHit.empty())
        throw std::invalid_argument("ray-tracing pipeline cannot mix legacy closest-hit and hit-group lists");

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
    stages.reserve(desc.rayGeneration.size() + desc.miss.size() + desc.callable.size() + desc.hitGroups.size() * 3U);
    groups.reserve(desc.rayGeneration.size() + desc.miss.size() + desc.callable.size() + desc.hitGroups.size());
    const auto addStage = [&](handles::ShaderHandle handle, ShaderStageMask expected) -> std::uint32_t {
        const auto shaderIt = typedShaders_.find(handle);
        if (shaderIt == typedShaders_.end() || !typedShaderHandles_.isAlive(handle) ||
            shaderIt->second.stage != expected)
            throw std::invalid_argument("ray-tracing shader stage does not match its group");
        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, toVkShaderStage(expected),
                          shaderIt->second.module, shaderIt->second.entryPoint.c_str(), nullptr});
        return static_cast<std::uint32_t>(stages.size() - 1U);
    };
    const auto addGeneral = [&](handles::ShaderHandle handle, ShaderStageMask expected) {
        const auto shader = addStage(handle, expected);
        groups.push_back({VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
                          VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, shader, VK_SHADER_UNUSED_KHR,
                          VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR});
    };
    for (const auto handle : desc.rayGeneration)
        addGeneral(handle, ShaderStageMask::rayGeneration);
    for (const auto handle : desc.miss)
        addGeneral(handle, ShaderStageMask::miss);
    for (const auto handle : desc.callable)
        addGeneral(handle, ShaderStageMask::callable);
    std::vector<RayTracingHitGroupDesc> hitGroups = desc.hitGroups;
    if (hitGroups.empty()) {
        hitGroups.reserve(desc.closestHit.size());
        for (const auto handle : desc.closestHit)
            hitGroups.push_back({RayTracingHitGroupType::triangles, handle, {}, {}});
    }
    for (const auto& hit : hitGroups) {
        const auto type = hit.type == RayTracingHitGroupType::procedural
                              ? VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR
                              : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        const auto closest =
            hit.closestHit.valid() ? addStage(hit.closestHit, ShaderStageMask::closestHit) : VK_SHADER_UNUSED_KHR;
        const auto any = hit.anyHit.valid() ? addStage(hit.anyHit, ShaderStageMask::anyHit) : VK_SHADER_UNUSED_KHR;
        const auto intersection =
            hit.intersection.valid() ? addStage(hit.intersection, ShaderStageMask::intersection) : VK_SHADER_UNUSED_KHR;
        if (type == VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR && intersection != VK_SHADER_UNUSED_KHR)
            throw std::invalid_argument("triangle hit groups cannot contain an intersection shader");
        if (type == VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR && intersection == VK_SHADER_UNUSED_KHR)
            throw std::invalid_argument("procedural hit groups require an intersection shader");
        groups.push_back({VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr, type,
                          VK_SHADER_UNUSED_KHR, closest, any, intersection});
    }
    const auto create = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(device_, "vkCreateRayTracingPipelinesKHR"));
    if (create == nullptr)
        throw std::runtime_error("vkCreateRayTracingPipelinesKHR is unavailable");
    const VkRayTracingPipelineCreateInfoKHR createInfo{
        .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
        .stageCount = static_cast<std::uint32_t>(stages.size()),
        .pStages = stages.data(),
        .groupCount = static_cast<std::uint32_t>(groups.size()),
        .pGroups = groups.data(),
        .maxPipelineRayRecursionDepth = desc.maxRecursionDepth,
        .layout = layoutIt->second.layout,
    };
    TypedPipeline typed{.layout = desc.layout,
                        .groupCount = static_cast<std::uint32_t>(groups.size()),
                        .rayTracing = true,
                        .bindPoint = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR};
    check(create(device_, VK_NULL_HANDLE, pipelineCache_, 1, &createInfo, nullptr, &typed.pipeline),
          "create typed ray-tracing pipeline");
    const auto handle = typedPipelineHandles_.create();
    typedPipelines_.emplace(handle, typed);
    return handle;
}

void VulkanDevice::destroyPipelineEx(handles::PipelineHandle handle) {
    const auto it = typedPipelines_.find(handle);
    if (it == typedPipelines_.end() || !typedPipelineHandles_.isAlive(handle))
        throw std::invalid_argument("stale typed pipeline handle");
    if (it->second.pipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, it->second.pipeline, nullptr);
    typedPipelines_.erase(it);
    typedPipelineHandles_.destroy(handle);
}

handles::ShaderBindingTableHandle VulkanDevice::createShaderBindingTable(const ShaderBindingTableDesc& desc) {
    if (!capabilities_.rayTracingPipeline || !capabilities_.bufferDeviceAddress)
        throw std::runtime_error("shader binding tables require Vulkan ray-tracing pipeline and device address");
    const auto pipelineIt = typedPipelines_.find(desc.pipeline);
    if (pipelineIt == typedPipelines_.end() || !typedPipelineHandles_.isAlive(desc.pipeline) ||
        !pipelineIt->second.rayTracing)
        throw std::invalid_argument("SBT references a non-ray-tracing pipeline");
    if (desc.raygenCount == 0)
        throw std::invalid_argument("SBT requires at least one raygen group");
    const std::uint64_t totalGroups =
        static_cast<std::uint64_t>(desc.raygenCount) + desc.missCount + desc.hitCount + desc.callableCount;
    if (totalGroups != pipelineIt->second.groupCount)
        throw std::invalid_argument("SBT group counts do not match its ray-tracing pipeline");
    const VkDeviceSize handleSize = rayTracingPipelineProperties_.shaderGroupHandleSize;
    const VkDeviceSize handleAlignment = rayTracingPipelineProperties_.shaderGroupHandleAlignment;
    const VkDeviceSize baseAlignment = rayTracingPipelineProperties_.shaderGroupBaseAlignment;
    const VkDeviceSize stride = alignDeviceAddress(handleSize, handleAlignment);
    if (handleSize == 0 || stride == 0 || stride > rayTracingPipelineProperties_.maxShaderGroupStride)
        throw std::runtime_error("Vulkan ray-tracing properties cannot represent an SBT record");
    const auto regionSize = [stride](std::uint32_t count) {
        if (count != 0 && static_cast<VkDeviceSize>(count) > std::numeric_limits<VkDeviceSize>::max() / stride)
            throw std::overflow_error("SBT region size overflow");
        return stride * static_cast<VkDeviceSize>(count);
    };
    const VkDeviceSize raygenSize = regionSize(desc.raygenCount);
    const VkDeviceSize missSize = regionSize(desc.missCount);
    const VkDeviceSize hitSize = regionSize(desc.hitCount);
    const VkDeviceSize callableSize = regionSize(desc.callableCount);
    if (raygenSize > std::numeric_limits<VkDeviceSize>::max() - missSize ||
        raygenSize + missSize > std::numeric_limits<VkDeviceSize>::max() - hitSize ||
        raygenSize + missSize + hitSize > std::numeric_limits<VkDeviceSize>::max() - callableSize)
        throw std::overflow_error("SBT allocation size overflow");
    const VkDeviceSize reserveSize = raygenSize + missSize + hitSize + callableSize + baseAlignment * 4U;
    TypedShaderBindingTable typed{};
    const VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = reserveSize,
        .usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    check(vkCreateBuffer(device_, &bufferInfo, nullptr, &typed.buffer), "create shader binding table buffer");
    try {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, typed.buffer, &requirements);
        const VkMemoryAllocateFlagsInfo allocationFlags{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = static_cast<VkMemoryAllocateFlags>(VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT),
        };
        const VkMemoryAllocateInfo allocationInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &allocationFlags,
            .allocationSize = requirements.size,
            .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
        };
        check(vkAllocateMemory(device_, &allocationInfo, nullptr, &typed.memory), "allocate SBT memory");
        check(vkBindBufferMemory(device_, typed.buffer, typed.memory, 0), "bind SBT memory");
        const VkDeviceAddress baseAddress = bufferDeviceAddress(typed.buffer);
        const VkDeviceSize raygenOffset = alignDeviceAddress(baseAddress, baseAlignment) - baseAddress;
        const VkDeviceSize missOffset =
            alignDeviceAddress(baseAddress + raygenOffset + raygenSize, baseAlignment) - baseAddress;
        const VkDeviceSize hitOffset =
            alignDeviceAddress(baseAddress + missOffset + missSize, baseAlignment) - baseAddress;
        const VkDeviceSize callableOffset =
            alignDeviceAddress(baseAddress + hitOffset + hitSize, baseAlignment) - baseAddress;
        const VkDeviceSize endOffset = callableOffset + callableSize;
        if (endOffset < callableOffset || endOffset > reserveSize)
            throw std::overflow_error("SBT region offsets exceed allocation");
        typed.size = endOffset;
        typed.raygen = {baseAddress + raygenOffset, raygenSize, stride};
        if (desc.missCount != 0)
            typed.miss = {baseAddress + missOffset, missSize, stride};
        if (desc.hitCount != 0)
            typed.hit = {baseAddress + hitOffset, hitSize, stride};
        if (desc.callableCount != 0)
            typed.callable = {baseAddress + callableOffset, callableSize, stride};
        const auto getHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
            vkGetDeviceProcAddr(device_, "vkGetRayTracingShaderGroupHandlesKHR"));
        if (getHandles == nullptr)
            throw std::runtime_error("vkGetRayTracingShaderGroupHandlesKHR is unavailable");
        std::vector<std::uint8_t> groupHandles(static_cast<std::size_t>(totalGroups * handleSize));
        check(getHandles(device_, pipelineIt->second.pipeline, 0, static_cast<std::uint32_t>(totalGroups),
                         groupHandles.size(), groupHandles.data()),
              "get shader group handles");
        void* mapped = nullptr;
        check(vkMapMemory(device_, typed.memory, 0, typed.size, 0, &mapped), "map SBT memory");
        const auto copyRegion = [&](VkDeviceSize offset, std::uint32_t count, std::uint32_t firstGroup) {
            auto* destination = static_cast<std::uint8_t*>(mapped) + offset;
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto sourceOffset = static_cast<std::size_t>(firstGroup + index) * handleSize;
                std::memcpy(destination + static_cast<VkDeviceSize>(index) * stride, groupHandles.data() + sourceOffset,
                            static_cast<std::size_t>(handleSize));
            }
        };
        copyRegion(raygenOffset, desc.raygenCount, 0);
        copyRegion(missOffset, desc.missCount, desc.raygenCount);
        copyRegion(hitOffset, desc.hitCount, desc.raygenCount + desc.missCount);
        copyRegion(callableOffset, desc.callableCount, desc.raygenCount + desc.missCount + desc.hitCount);
        vkUnmapMemory(device_, typed.memory);
    } catch (...) {
        if (typed.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, typed.memory, nullptr);
        if (typed.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, typed.buffer, nullptr);
        throw;
    }
    const auto handle = typedShaderBindingTableHandles_.create();
    typedShaderBindingTables_.emplace(handle, typed);
    return handle;
}

void VulkanDevice::destroyShaderBindingTable(handles::ShaderBindingTableHandle handle) {
    const auto it = typedShaderBindingTables_.find(handle);
    if (it == typedShaderBindingTables_.end() || !typedShaderBindingTableHandles_.isAlive(handle))
        throw std::invalid_argument("stale shader binding table handle");
    if (it->second.buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device_, it->second.buffer, nullptr);
    if (it->second.memory != VK_NULL_HANDLE)
        vkFreeMemory(device_, it->second.memory, nullptr);
    typedShaderBindingTables_.erase(it);
    typedShaderBindingTableHandles_.destroy(handle);
}

void VulkanDevice::submitImmediate(const std::function<void(VkCommandBuffer)>& record) {
    if (uploadContext_ == nullptr)
        throw std::logic_error("Vulkan upload context is unavailable");
    try {
        uploadContext_->begin();
        record(uploadContext_->commandBuffer());
        const auto signal = uploadContext_->submit();
        uploadContext_->wait(signal);
    } catch (...) {
        uploadContext_->abort();
        throw;
    }
}

void VulkanDevice::copyBufferEx(handles::BufferHandle source, handles::BufferHandle destination) {
    const auto sourceIt = typedBuffers_.find(source);
    const auto destinationIt = typedBuffers_.find(destination);
    if (sourceIt == typedBuffers_.end() || !typedBufferHandles_.isAlive(source) ||
        destinationIt == typedBuffers_.end() || !typedBufferHandles_.isAlive(destination))
        throw std::invalid_argument("typed buffer copy references a stale buffer handle");
    if ((toBits(sourceIt->second.desc.usage) & toBits(ResourceUsage::transferSrc)) == 0U ||
        (toBits(destinationIt->second.desc.usage) & toBits(ResourceUsage::transferDst)) == 0U)
        throw std::invalid_argument("typed buffer copy requires transfer usage");
    if (sourceIt->second.desc.size > destinationIt->second.desc.size)
        throw std::out_of_range("typed buffer copy destination is too small");
    const VkBufferCopy region{0, 0, sourceIt->second.resource.size};
    submitImmediate([&](VkCommandBuffer commandBuffer) {
        vkCmdCopyBuffer(commandBuffer, sourceIt->second.resource.buffer, destinationIt->second.resource.buffer, 1,
                        &region);
    });
}

void VulkanDevice::recordCopyBuffer(VkCommandBuffer commandBuffer, handles::BufferHandle source,
                                    handles::BufferHandle destination) {
    const auto sourceIt = typedBuffers_.find(source);
    const auto destinationIt = typedBuffers_.find(destination);
    if (sourceIt == typedBuffers_.end() || !typedBufferHandles_.isAlive(source) ||
        destinationIt == typedBuffers_.end() || !typedBufferHandles_.isAlive(destination))
        throw std::invalid_argument("typed command-list buffer copy references a stale buffer handle");
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("typed command-list buffer copy requires a command buffer");
    if ((toBits(sourceIt->second.desc.usage) & toBits(ResourceUsage::transferSrc)) == 0U ||
        (toBits(destinationIt->second.desc.usage) & toBits(ResourceUsage::transferDst)) == 0U)
        throw std::invalid_argument("typed command-list buffer copy requires transfer usage");
    if (sourceIt->second.desc.size > destinationIt->second.desc.size)
        throw std::out_of_range("typed command-list buffer copy destination is too small");
    const VkBufferCopy region{0, 0, sourceIt->second.resource.size};
    vkCmdCopyBuffer(commandBuffer, sourceIt->second.resource.buffer, destinationIt->second.resource.buffer, 1, &region);
}

void VulkanDevice::recordUploadBuffer(VkCommandBuffer commandBuffer, handles::BufferHandle destination,
                                      std::span<const std::byte> bytes, std::size_t offset) {
    const auto destinationIt = typedBuffers_.find(destination);
    if (destinationIt == typedBuffers_.end() || !typedBufferHandles_.isAlive(destination))
        throw std::invalid_argument("typed command-list buffer upload references a stale buffer handle");
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("typed command-list buffer upload requires a command buffer");
    if (offset > destinationIt->second.desc.size || bytes.size() > destinationIt->second.desc.size - offset)
        throw std::out_of_range("typed command-list buffer upload exceeds allocation");
    if (bytes.empty())
        return;
    if ((toBits(destinationIt->second.desc.usage) & toBits(ResourceUsage::transferDst)) == 0U)
        throw std::logic_error("typed command-list buffer upload requires transfer-destination usage");
    auto* frame = frameForCommandBuffer(commandBuffer);
    if (frame == nullptr)
        throw std::invalid_argument("typed command-list buffer upload is not a frame command buffer");
    auto& staging = allocateNativeUploadBuffer(*frame, bytes.size(), 4);
    const auto stagingOffset = staging.offset - bytes.size();
    std::memcpy(static_cast<std::byte*>(staging.mapped) + stagingOffset, bytes.data(), bytes.size());
    const VkBufferCopy copy{.srcOffset = stagingOffset, .dstOffset = offset, .size = bytes.size()};
    vkCmdCopyBuffer(commandBuffer, staging.buffer, destinationIt->second.resource.buffer, 1, &copy);
    const VkBufferMemoryBarrier2 visible{
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = destinationIt->second.resource.buffer,
        .offset = offset,
        .size = bytes.size(),
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &visible,
    };
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void VulkanDevice::copyBufferToTextureEx(handles::BufferHandle source, handles::TextureHandle destination) {
    const auto sourceIt = typedBuffers_.find(source);
    const auto destinationIt = typedTextures_.find(destination);
    if (sourceIt == typedBuffers_.end() || !typedBufferHandles_.isAlive(source) ||
        destinationIt == typedTextures_.end() || !typedTextureHandles_.isAlive(destination))
        throw std::invalid_argument("typed buffer-to-texture copy references a stale handle");
    const auto& texture = destinationIt->second;
    if ((toBits(sourceIt->second.desc.usage) & toBits(ResourceUsage::transferSrc)) == 0U ||
        (toBits(texture.desc.usage) & toBits(ResourceUsage::transferDst)) == 0U)
        throw std::invalid_argument("typed buffer-to-texture copy requires transfer usage");
    const auto bytes = checkedResourceMul(mipBytes(texture.desc, 0), imageLayerCount(texture.desc));
    if (bytes > sourceIt->second.desc.size)
        throw std::out_of_range("typed buffer-to-texture source is too small");
    submitImmediate([&](VkCommandBuffer commandBuffer) {
        recordTextureTransition(commandBuffer, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkBufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {imageAspect(texture.desc.format), 0, 0, imageLayerCount(texture.desc)},
            .imageOffset = {0, 0, 0},
            .imageExtent = mipExtent(texture.desc, 0),
        };
        vkCmdCopyBufferToImage(commandBuffer, sourceIt->second.resource.buffer, texture.resource.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        recordTextureTransition(commandBuffer, destination, typedTextureFinalLayout(texture));
    });
}

void VulkanDevice::copyTextureToBufferEx(handles::TextureHandle source, handles::BufferHandle destination) {
    const auto sourceIt = typedTextures_.find(source);
    const auto destinationIt = typedBuffers_.find(destination);
    if (sourceIt == typedTextures_.end() || !typedTextureHandles_.isAlive(source) ||
        destinationIt == typedBuffers_.end() || !typedBufferHandles_.isAlive(destination))
        throw std::invalid_argument("typed texture-to-buffer copy references a stale handle");
    const auto& texture = sourceIt->second;
    if ((toBits(texture.desc.usage) & toBits(ResourceUsage::transferSrc)) == 0U ||
        (toBits(destinationIt->second.desc.usage) & toBits(ResourceUsage::transferDst)) == 0U)
        throw std::invalid_argument("typed texture-to-buffer copy requires transfer usage");
    const auto bytes = checkedResourceMul(mipBytes(texture.desc, 0), imageLayerCount(texture.desc));
    if (bytes > destinationIt->second.desc.size)
        throw std::out_of_range("typed texture-to-buffer destination is too small");
    submitImmediate([&](VkCommandBuffer commandBuffer) {
        recordTextureTransition(commandBuffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const VkBufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {imageAspect(texture.desc.format), 0, 0, imageLayerCount(texture.desc)},
            .imageOffset = {0, 0, 0},
            .imageExtent = mipExtent(texture.desc, 0),
        };
        vkCmdCopyImageToBuffer(commandBuffer, texture.resource.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               destinationIt->second.resource.buffer, 1, &region);
        recordTextureTransition(commandBuffer, source, typedTextureFinalLayout(texture));
    });
}

void VulkanDevice::copyTextureEx(handles::TextureHandle source, handles::TextureHandle destination) {
    submitImmediate([&](VkCommandBuffer commandBuffer) { recordCopyTexture(commandBuffer, source, destination); });
}

void VulkanDevice::clearTextureEx(handles::TextureHandle texture, const std::array<float, 4>& value) {
    submitImmediate([&](VkCommandBuffer commandBuffer) { recordClearTexture(commandBuffer, texture, value); });
}

void VulkanDevice::clearBufferEx(handles::BufferHandle buffer, std::uint32_t value) {
    const auto it = typedBuffers_.find(buffer);
    if (it == typedBuffers_.end() || !typedBufferHandles_.isAlive(buffer))
        throw std::invalid_argument("typed buffer clear references a stale buffer handle");
    if ((toBits(it->second.desc.usage) & toBits(ResourceUsage::transferDst)) == 0U)
        throw std::invalid_argument("typed buffer clear requires transfer-destination usage");
    if (it->second.resource.size % 4U != 0U)
        throw std::invalid_argument("typed buffer clear requires a four-byte-aligned buffer");
    submitImmediate([&](VkCommandBuffer commandBuffer) {
        vkCmdFillBuffer(commandBuffer, it->second.resource.buffer, 0, it->second.resource.size, value);
    });
}

void VulkanDevice::generateMipmapsEx(handles::TextureHandle texture) {
    submitImmediate([&](VkCommandBuffer commandBuffer) { recordGenerateMipmaps(commandBuffer, texture); });
}

void VulkanDevice::uploadTextureEx(handles::TextureHandle texture, std::span<const std::uint8_t> bytes,
                                   std::uint32_t mipLevel, std::uint32_t arrayLayer) {
    const auto it = typedTextures_.find(texture);
    if (it == typedTextures_.end() || !typedTextureHandles_.isAlive(texture))
        throw std::invalid_argument("typed texture upload references a stale texture handle");
    const auto& typed = it->second;
    if (mipLevel >= typed.desc.mipLevels || arrayLayer >= imageLayerCount(typed.desc))
        throw std::out_of_range("typed texture upload subresource is out of range");
    if ((toBits(typed.desc.usage) & toBits(ResourceUsage::transferDst)) == 0U)
        throw std::invalid_argument("typed texture upload requires transfer-destination usage");
    const auto expected = mipBytes(typed.desc, mipLevel);
    if (bytes.size() != expected)
        throw std::invalid_argument("typed texture upload byte count does not match its mip extent");
    if (uploadContext_ == nullptr)
        throw std::logic_error("Vulkan upload context is unavailable");
    try {
        uploadContext_->begin();
        const auto staging = uploadContext_->allocate(bytes.size(), 4);
        std::memcpy(staging.mapped, bytes.data(), bytes.size());
        const auto commandBuffer = uploadContext_->commandBuffer();
        recordTextureTransition(commandBuffer, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkBufferImageCopy region{
            .bufferOffset = staging.offset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {imageAspect(typed.desc.format), mipLevel, arrayLayer, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = mipExtent(typed.desc, mipLevel),
        };
        vkCmdCopyBufferToImage(commandBuffer, staging.buffer, typed.resource.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        recordTextureTransition(commandBuffer, texture, typedTextureFinalLayout(typed));
        const auto signal = uploadContext_->submit();
        uploadContext_->wait(signal);
    } catch (...) {
        uploadContext_->abort();
        throw;
    }
}

std::vector<std::uint8_t> VulkanDevice::readbackTextureEx(handles::TextureHandle texture, std::uint32_t mipLevel,
                                                          std::uint32_t arrayLayer) {
    const auto it = typedTextures_.find(texture);
    if (it == typedTextures_.end() || !typedTextureHandles_.isAlive(texture))
        throw std::invalid_argument("typed texture readback references a stale texture handle");
    const auto& typed = it->second;
    if (mipLevel >= typed.desc.mipLevels || arrayLayer >= imageLayerCount(typed.desc))
        throw std::out_of_range("typed texture readback subresource is out of range");
    if ((toBits(typed.desc.usage) & toBits(ResourceUsage::transferSrc)) == 0U)
        throw std::invalid_argument("typed texture readback requires transfer-source usage");
    const auto expected = mipBytes(typed.desc, mipLevel);
    if (uploadContext_ == nullptr)
        throw std::logic_error("Vulkan upload context is unavailable");
    try {
        uploadContext_->begin();
        const auto staging = uploadContext_->allocate(expected, 4);
        const auto commandBuffer = uploadContext_->commandBuffer();
        recordTextureTransition(commandBuffer, texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const VkBufferImageCopy region{
            .bufferOffset = staging.offset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {imageAspect(typed.desc.format), mipLevel, arrayLayer, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = mipExtent(typed.desc, mipLevel),
        };
        vkCmdCopyImageToBuffer(commandBuffer, typed.resource.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging.buffer, 1, &region);
        recordTextureTransition(commandBuffer, texture, typedTextureFinalLayout(typed));
        const auto signal = uploadContext_->submit();
        uploadContext_->wait(signal);
        std::vector<std::uint8_t> result(expected);
        std::memcpy(result.data(), staging.mapped, expected);
        return result;
    } catch (...) {
        uploadContext_->abort();
        throw;
    }
}

VkDeviceAddress VulkanDevice::bufferDeviceAddress(VkBuffer buffer) const {
    if (!capabilities_.bufferDeviceAddress)
        throw std::runtime_error("buffer device address is unsupported");
    const VkBufferDeviceAddressInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer,
    };
    const VkDeviceAddress address = vkGetBufferDeviceAddress(device_, &info);
    if (address == 0)
        throw std::runtime_error("Vulkan returned a null buffer device address");
    return address;
}

void VulkanDevice::recordTraceRays(VkCommandBuffer commandBuffer, handles::PipelineHandle pipeline,
                                   handles::ShaderBindingTableHandle sbt, std::uint32_t width, std::uint32_t height,
                                   std::uint32_t depth) {
    const auto pipelineIt = typedPipelines_.find(pipeline);
    const auto sbtIt = typedShaderBindingTables_.find(sbt);
    if (pipelineIt == typedPipelines_.end() || !typedPipelineHandles_.isAlive(pipeline) ||
        !pipelineIt->second.rayTracing)
        throw std::invalid_argument("traceRays references a non-ray-tracing pipeline");
    if (sbtIt == typedShaderBindingTables_.end() || !typedShaderBindingTableHandles_.isAlive(sbt))
        throw std::invalid_argument("traceRays references a stale shader binding table");
    if (width == 0 || height == 0 || depth == 0)
        throw std::invalid_argument("traceRays extent must be non-zero");
    const auto trace = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(vkGetDeviceProcAddr(device_, "vkCmdTraceRaysKHR"));
    if (trace == nullptr)
        throw std::runtime_error("vkCmdTraceRaysKHR is unavailable");
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("traceRays requires a command buffer");
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelineIt->second.pipeline);
    trace(commandBuffer, &sbtIt->second.raygen, &sbtIt->second.miss, &sbtIt->second.hit, &sbtIt->second.callable, width,
          height, depth);
}

void VulkanDevice::recordNativeOutputToImage(VkCommandBuffer commandBuffer, const NativeFrameOutput& output,
                                             VkImage target, VkImageView targetView, VkImageLayout previousLayout,
                                             VkPipelineStageFlags2 previousStage, VkAccessFlags2 previousAccess,
                                             VkImageLayout finalLayout, VkPipelineStageFlags2 finalStage,
                                             VkAccessFlags2 finalAccess, bool initialized, VkExtent2D extent) {
    if (!output.valid())
        throw std::invalid_argument("native frame output is invalid");
    if (target == VK_NULL_HANDLE)
        throw std::invalid_argument("native frame output target is unavailable");
    if (targetView == VK_NULL_HANDLE)
        throw std::invalid_argument("native frame output target view is unavailable");
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("native frame output requires a command buffer");
    if (nativeOutputPipeline_ == VK_NULL_HANDLE || nativeOutputPipelineLayout_ == VK_NULL_HANDLE)
        throw std::logic_error("native frame output pipeline is unavailable");

    const auto sourceIt = typedTextures_.find(output.texture);
    if (sourceIt == typedTextures_.end() || !typedTextureHandles_.isAlive(output.texture))
        throw std::invalid_argument("native frame output references a stale texture handle");
    const auto& source = sourceIt->second;
    if (source.desc.dimension != TextureDimension::d2 || source.desc.extent.width != extent.width ||
        source.desc.extent.height != extent.height || source.desc.extent.depth != 1 || source.desc.mipLevels != 1 ||
        source.desc.arrayLayers != 1 || source.desc.format != output.format ||
        (toBits(source.desc.usage) & toBits(ResourceUsage::sampledRead)) == 0U)
        throw std::invalid_argument("native frame output does not match the presentation target");

    const VkDescriptorSet descriptor = nativeOutputDescriptors_[frameIndex_];
    if (descriptor == VK_NULL_HANDLE || previewClampSampler_ == VK_NULL_HANDLE)
        throw std::logic_error("native frame output descriptors are unavailable");
    const VkDescriptorImageInfo imageInfo{
        .sampler = VK_NULL_HANDLE,
        .imageView = source.view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    const VkDescriptorImageInfo samplerInfo{
        .sampler = previewClampSampler_,
        .imageView = VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const std::array writes{
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor, 0, 0, 1,
                             VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &imageInfo, nullptr, nullptr},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor, 1, 0, 1,
                             VK_DESCRIPTOR_TYPE_SAMPLER, &samplerInfo, nullptr, nullptr},
    };
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    recordTextureTransition(commandBuffer, output.texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkImageMemoryBarrier2 toColor{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = initialized ? previousStage : VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = initialized ? previousAccess : VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = initialized ? previousLayout : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = target,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo toColorDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toColor,
    };
    vkCmdPipelineBarrier2(commandBuffer, &toColorDependency);
    const VkRenderingAttachmentInfo attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = targetView,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    const VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    };
    vkCmdBeginRendering(commandBuffer, &rendering);
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, nativeOutputPipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, nativeOutputPipelineLayout_, 0, 1,
                            &descriptor, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRendering(commandBuffer);
    const VkImageMemoryBarrier2 toFinal{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = finalStage,
        .dstAccessMask = finalAccess,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = finalLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = target,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo toFinalDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toFinal,
    };
    vkCmdPipelineBarrier2(commandBuffer, &toFinalDependency);
    recordTextureTransition(commandBuffer, output.texture, typedTextureFinalLayout(source));
}

void VulkanDevice::recordBindPipeline(VkCommandBuffer commandBuffer, handles::PipelineHandle pipeline) {
    const auto it = typedPipelines_.find(pipeline);
    if (it == typedPipelines_.end() || !typedPipelineHandles_.isAlive(pipeline))
        throw std::invalid_argument("typed pipeline bind references a stale pipeline handle");
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("typed pipeline bind requires a command buffer");
    vkCmdBindPipeline(commandBuffer, it->second.bindPoint, it->second.pipeline);
}

void VulkanDevice::recordDrawIndexed(VkCommandBuffer commandBuffer, const IndexedDrawEx& draw) {
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("typed indexed draw requires a command buffer");
    if (!draw.vertexBuffer.valid() || !draw.indexBuffer.valid() || draw.indexCount == 0 || draw.instanceCount == 0)
        throw std::invalid_argument("typed indexed draw has invalid buffers or counts");
    const auto vertexIt = typedBuffers_.find(draw.vertexBuffer);
    const auto indexIt = typedBuffers_.find(draw.indexBuffer);
    if (vertexIt == typedBuffers_.end() || !typedBufferHandles_.isAlive(draw.vertexBuffer) ||
        indexIt == typedBuffers_.end() || !typedBufferHandles_.isAlive(draw.indexBuffer))
        throw std::invalid_argument("typed indexed draw references a stale buffer handle");
    if ((toBits(vertexIt->second.desc.usage) & toBits(ResourceUsage::vertexRead)) == 0U ||
        (toBits(indexIt->second.desc.usage) & toBits(ResourceUsage::indexRead)) == 0U)
        throw std::invalid_argument("typed indexed draw buffers do not have vertex/index usage");
    const auto requiredIndexBytes = static_cast<std::uint64_t>(draw.firstIndex) * sizeof(std::uint32_t) +
                                    static_cast<std::uint64_t>(draw.indexCount) * sizeof(std::uint32_t);
    if (requiredIndexBytes > indexIt->second.desc.size)
        throw std::out_of_range("typed indexed draw exceeds its index buffer");
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexIt->second.resource.buffer, &vertexOffset);
    vkCmdBindIndexBuffer(commandBuffer, indexIt->second.resource.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(commandBuffer, draw.indexCount, draw.instanceCount, draw.firstIndex, draw.vertexOffset,
                     draw.firstInstance);
}

VkImageLayout VulkanDevice::typedTextureFinalLayout(const TypedTexture& texture) noexcept {
    return layoutForUsage(texture.desc.usage);
}

void VulkanDevice::recordTextureTransition(VkCommandBuffer commandBuffer, handles::TextureHandle texture,
                                           VkImageLayout nextLayout) {
    const auto it = typedTextures_.find(texture);
    if (it == typedTextures_.end() || !typedTextureHandles_.isAlive(texture))
        throw std::invalid_argument("typed transition references a stale texture handle");
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("typed transition requires a command buffer");

    if (it->second.layout == nextLayout)
        return;

    VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = it->second.layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE
                                                                       : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = it->second.layout == VK_IMAGE_LAYOUT_UNDEFINED
                             ? VK_ACCESS_2_NONE
                             : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = it->second.layout,
        .newLayout = nextLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = it->second.resource.image,
        .subresourceRange = {imageAspect(it->second.desc.format), 0, it->second.desc.mipLevels, 0,
                             imageLayerCount(it->second.desc)},
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    it->second.layout = nextLayout;
}

void VulkanDevice::recordTransitionTexture(VkCommandBuffer commandBuffer, handles::TextureHandle texture) {
    const auto it = typedTextures_.find(texture);
    if (it == typedTextures_.end() || !typedTextureHandles_.isAlive(texture))
        throw std::invalid_argument("typed transition references a stale texture handle");
    recordTextureTransition(commandBuffer, texture, typedTextureFinalLayout(it->second));
}

void VulkanDevice::recordCopyTexture(VkCommandBuffer commandBuffer, handles::TextureHandle source,
                                     handles::TextureHandle destination) {
    const auto sourceIt = typedTextures_.find(source);
    const auto destinationIt = typedTextures_.find(destination);
    if (sourceIt == typedTextures_.end() || !typedTextureHandles_.isAlive(source) ||
        destinationIt == typedTextures_.end() || !typedTextureHandles_.isAlive(destination))
        throw std::invalid_argument("typed texture copy references a stale texture handle");
    const auto& sourceDesc = sourceIt->second.desc;
    const auto& destinationDesc = destinationIt->second.desc;
    if (sourceDesc.dimension != destinationDesc.dimension || sourceDesc.extent.width != destinationDesc.extent.width ||
        sourceDesc.extent.height != destinationDesc.extent.height ||
        sourceDesc.extent.depth != destinationDesc.extent.depth || sourceDesc.mipLevels != destinationDesc.mipLevels ||
        sourceDesc.arrayLayers != destinationDesc.arrayLayers || sourceDesc.format != destinationDesc.format)
        throw std::invalid_argument("typed texture copy requires matching resources");
    if ((toBits(sourceDesc.usage) & toBits(ResourceUsage::transferSrc)) == 0U ||
        (toBits(destinationDesc.usage) & toBits(ResourceUsage::transferDst)) == 0U)
        throw std::invalid_argument("typed texture copy requires transfer usage");
    recordTextureTransition(commandBuffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    recordTextureTransition(commandBuffer, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    std::vector<VkImageCopy> regions;
    regions.reserve(sourceDesc.mipLevels);
    for (std::uint32_t mip = 0; mip < sourceDesc.mipLevels; ++mip) {
        regions.push_back({
            .srcSubresource = {imageAspect(sourceDesc.format), mip, 0, imageLayerCount(sourceDesc)},
            .srcOffset = {0, 0, 0},
            .dstSubresource = {imageAspect(destinationDesc.format), mip, 0, imageLayerCount(destinationDesc)},
            .dstOffset = {0, 0, 0},
            .extent = mipExtent(sourceDesc, mip),
        });
    }
    vkCmdCopyImage(commandBuffer, sourceIt->second.resource.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   destinationIt->second.resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   static_cast<std::uint32_t>(regions.size()), regions.data());
    recordTextureTransition(commandBuffer, source, typedTextureFinalLayout(sourceIt->second));
    recordTextureTransition(commandBuffer, destination, typedTextureFinalLayout(destinationIt->second));
}

void VulkanDevice::recordClearTexture(VkCommandBuffer commandBuffer, handles::TextureHandle texture,
                                      const std::array<float, 4>& value) {
    const auto it = typedTextures_.find(texture);
    if (it == typedTextures_.end() || !typedTextureHandles_.isAlive(texture))
        throw std::invalid_argument("typed texture clear references a stale texture handle");
    if ((toBits(it->second.desc.usage) & toBits(ResourceUsage::transferDst)) == 0U)
        throw std::invalid_argument("typed texture clear requires transfer-destination usage");
    recordTextureTransition(commandBuffer, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageSubresourceRange range{imageAspect(it->second.desc.format), 0, it->second.desc.mipLevels, 0,
                                        imageLayerCount(it->second.desc)};
    if (it->second.desc.format == PixelFormat::depth32Float) {
        const VkClearDepthStencilValue clear{value[0], 0};
        vkCmdClearDepthStencilImage(commandBuffer, it->second.resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &clear, 1, &range);
    } else {
        const VkClearColorValue clear{{value[0], value[1], value[2], value[3]}};
        vkCmdClearColorImage(commandBuffer, it->second.resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                             &range);
    }
    recordTextureTransition(commandBuffer, texture, typedTextureFinalLayout(it->second));
}

void VulkanDevice::recordGenerateMipmaps(VkCommandBuffer commandBuffer, handles::TextureHandle texture) {
    const auto it = typedTextures_.find(texture);
    if (it == typedTextures_.end() || !typedTextureHandles_.isAlive(texture))
        throw std::invalid_argument("typed mipmap generation references a stale texture handle");
    if (it->second.desc.mipLevels < 2 || it->second.desc.format == PixelFormat::depth32Float ||
        (toBits(it->second.desc.usage) & toBits(ResourceUsage::transferSrc)) == 0U ||
        (toBits(it->second.desc.usage) & toBits(ResourceUsage::transferDst)) == 0U)
        throw std::invalid_argument("typed mipmap generation requires color transfer source/destination usage");
    VkFormatProperties2 formatProperties{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    vkGetPhysicalDeviceFormatProperties2(physicalDevice_, toVkFormat(it->second.desc.format), &formatProperties);
    const auto features = formatProperties.formatProperties.optimalTilingFeatures;
    const bool blitSource = (features & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0U;
    const bool blitDestination = (features & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0U;
    if (!blitSource || !blitDestination)
        throw std::runtime_error("typed mipmap generation has no Vulkan blit support for the texture format");
    const auto filter =
        (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0U ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    recordTextureTransition(commandBuffer, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const auto aspect = imageAspect(it->second.desc.format);
    for (std::uint32_t mip = 1; mip < it->second.desc.mipLevels; ++mip) {
        const VkImageMemoryBarrier2 sourceBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = it->second.resource.image,
            .subresourceRange = {aspect, mip - 1U, 1, 0, imageLayerCount(it->second.desc)},
        };
        const VkDependencyInfo sourceDependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &sourceBarrier,
        };
        vkCmdPipelineBarrier2(commandBuffer, &sourceDependency);
        const auto sourceExtent = mipExtent(it->second.desc, mip - 1U);
        const auto destinationExtent = mipExtent(it->second.desc, mip);
        const VkImageBlit blit{
            .srcSubresource = {aspect, mip - 1U, 0, imageLayerCount(it->second.desc)},
            .srcOffsets = {{0, 0, 0},
                           {static_cast<std::int32_t>(sourceExtent.width),
                            static_cast<std::int32_t>(sourceExtent.height),
                            static_cast<std::int32_t>(sourceExtent.depth)}},
            .dstSubresource = {aspect, mip, 0, imageLayerCount(it->second.desc)},
            .dstOffsets = {{0, 0, 0},
                           {static_cast<std::int32_t>(destinationExtent.width),
                            static_cast<std::int32_t>(destinationExtent.height),
                            static_cast<std::int32_t>(destinationExtent.depth)}},
        };
        vkCmdBlitImage(commandBuffer, it->second.resource.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       it->second.resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
        const VkImageMemoryBarrier2 restoreBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = it->second.resource.image,
            .subresourceRange = {aspect, mip - 1U, 1, 0, imageLayerCount(it->second.desc)},
        };
        const VkDependencyInfo restoreDependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &restoreBarrier,
        };
        vkCmdPipelineBarrier2(commandBuffer, &restoreDependency);
    }
    recordTextureTransition(commandBuffer, texture, typedTextureFinalLayout(it->second));
}

void VulkanDevice::recordBindDescriptorSet(VkCommandBuffer commandBuffer, handles::PipelineHandle pipeline,
                                           handles::DescriptorSetHandle set, std::uint32_t setIndex) {
    const auto pipelineIt = typedPipelines_.find(pipeline);
    const auto setIt = typedDescriptorSets_.find(set);
    if (pipelineIt == typedPipelines_.end() || !typedPipelineHandles_.isAlive(pipeline))
        throw std::invalid_argument("typed descriptor bind references a stale pipeline handle");
    if (setIt == typedDescriptorSets_.end() || !typedDescriptorSetHandles_.isAlive(set))
        throw std::invalid_argument("typed descriptor bind references a stale descriptor set handle");
    const auto layoutIt = typedPipelineLayouts_.find(pipelineIt->second.layout);
    if (layoutIt == typedPipelineLayouts_.end() || setIndex >= layoutIt->second.desc.setLayouts.size() ||
        layoutIt->second.desc.setLayouts[setIndex] != setIt->second.layout)
        throw std::invalid_argument("typed descriptor set is incompatible with pipeline layout set index");
    vkCmdBindDescriptorSets(commandBuffer, pipelineIt->second.bindPoint, layoutIt->second.layout, setIndex, 1,
                            &setIt->second.set, 0, nullptr);
}

void VulkanDevice::recordPushConstants(VkCommandBuffer commandBuffer, handles::PipelineHandle pipeline,
                                       std::span<const std::byte> bytes) {
    const auto pipelineIt = typedPipelines_.find(pipeline);
    if (pipelineIt == typedPipelines_.end() || !typedPipelineHandles_.isAlive(pipeline))
        throw std::invalid_argument("typed push constants reference a stale pipeline handle");
    if (bytes.empty())
        return;
    const auto layoutIt = typedPipelineLayouts_.find(pipelineIt->second.layout);
    if (layoutIt == typedPipelineLayouts_.end())
        throw std::invalid_argument("typed push constants reference a stale pipeline layout");
    const auto range = std::find_if(
        layoutIt->second.desc.pushConstants.begin(), layoutIt->second.desc.pushConstants.end(),
        [&](const PushConstantRange& candidate) { return candidate.offset == 0 && candidate.size >= bytes.size(); });
    if (range == layoutIt->second.desc.pushConstants.end())
        throw std::out_of_range("typed push constants exceed pipeline layout range");
    vkCmdPushConstants(commandBuffer, layoutIt->second.layout, toVkShaderStages(range->stages), range->offset,
                       static_cast<std::uint32_t>(bytes.size()), bytes.data());
}

void VulkanDevice::recordBeginRendering(VkCommandBuffer commandBuffer, handles::TextureHandle target, bool clear) {
    RenderingInfoEx info;
    info.colors.push_back({.texture = target, .clear = clear});
    recordBeginRendering(commandBuffer, info);
}

void VulkanDevice::recordBeginRendering(VkCommandBuffer commandBuffer, const RenderingInfoEx& info) {
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("typed rendering requires a command buffer");
    if (info.colors.empty() && !info.depth.has_value())
        throw std::invalid_argument("typed rendering requires a color or depth attachment");

    const auto extentTexture = info.colors.empty() ? info.depth->texture : info.colors.front().texture;
    const auto firstIt = typedTextures_.find(extentTexture);
    if (firstIt == typedTextures_.end() || !typedTextureHandles_.isAlive(extentTexture))
        throw std::invalid_argument("typed rendering attachment references a stale texture handle");
    const auto& firstDescription = firstIt->second.desc;
    const Extent3D requestedExtent = info.extent.width == 0 || info.extent.height == 0
                                         ? firstDescription.extent
                                         : info.extent;
    if (requestedExtent.width == 0 || requestedExtent.height == 0 || requestedExtent.depth != 1)
        throw std::invalid_argument("typed rendering extent is invalid");

    std::vector<VkRenderingAttachmentInfo> colorAttachments;
    colorAttachments.reserve(info.colors.size());
    for (const auto& color : info.colors) {
        const auto it = typedTextures_.find(color.texture);
        if (it == typedTextures_.end() || !typedTextureHandles_.isAlive(color.texture))
            throw std::invalid_argument("typed rendering color attachment references a stale texture handle");
        const auto& description = it->second.desc;
        if (description.dimension != TextureDimension::d2 || description.extent.width != requestedExtent.width ||
            description.extent.height != requestedExtent.height || description.extent.depth != 1 ||
            description.mipLevels != 1 || description.arrayLayers != 1 ||
            description.format == PixelFormat::depth32Float ||
            (toBits(description.usage) & toBits(ResourceUsage::colorAttachment)) == 0U)
            throw std::invalid_argument("typed rendering color attachment is incompatible with the render area");
        const bool undefined = it->second.layout == VK_IMAGE_LAYOUT_UNDEFINED;
        recordTextureTransition(commandBuffer, color.texture, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        const VkClearValue clearValue{.color = {{color.clearColor[0], color.clearColor[1], color.clearColor[2],
                                                  color.clearColor[3]}}};
        colorAttachments.push_back({
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = it->second.view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = color.clear || undefined ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = clearValue,
        });
    }

    std::optional<VkRenderingAttachmentInfo> depthAttachment;
    if (info.depth.has_value()) {
        const auto it = typedTextures_.find(info.depth->texture);
        if (it == typedTextures_.end() || !typedTextureHandles_.isAlive(info.depth->texture))
            throw std::invalid_argument("typed rendering depth attachment references a stale texture handle");
        const auto& description = it->second.desc;
        const auto depthBits = toBits(description.usage);
        if (description.dimension != TextureDimension::d2 || description.extent.width != requestedExtent.width ||
            description.extent.height != requestedExtent.height || description.extent.depth != 1 ||
            description.mipLevels != 1 || description.arrayLayers != 1 || description.format != PixelFormat::depth32Float ||
            (depthBits & (toBits(ResourceUsage::depthRead) | toBits(ResourceUsage::depthWrite))) == 0U)
            throw std::invalid_argument("typed rendering depth attachment is incompatible with the render area");
        const bool undefined = it->second.layout == VK_IMAGE_LAYOUT_UNDEFINED;
        recordTextureTransition(commandBuffer, info.depth->texture, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        const VkClearValue clearValue{.depthStencil = {info.depth->clearDepth, 0U}};
        depthAttachment = VkRenderingAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = it->second.view,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .loadOp = info.depth->clear || undefined ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = clearValue,
        };
    }

    const VkExtent2D extent{requestedExtent.width, requestedExtent.height};
    const VkRenderingInfo rendering{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = static_cast<std::uint32_t>(colorAttachments.size()),
        .pColorAttachments = colorAttachments.data(),
        .pDepthAttachment = depthAttachment.has_value() ? &*depthAttachment : nullptr,
    };
    vkCmdBeginRendering(commandBuffer, &rendering);
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
}

void VulkanDevice::recordEndRendering(VkCommandBuffer commandBuffer, handles::TextureHandle target) {
    const std::array targets{target};
    recordEndRendering(commandBuffer, targets);
}

void VulkanDevice::recordEndRendering(VkCommandBuffer commandBuffer,
                                      std::span<const handles::TextureHandle> targets) {
    if (targets.empty())
        throw std::invalid_argument("typed rendering requires at least one attachment");
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("typed rendering requires a command buffer");
    for (const auto target : targets) {
        const auto it = typedTextures_.find(target);
        if (it == typedTextures_.end() || !typedTextureHandles_.isAlive(target))
            throw std::invalid_argument("typed rendering attachment references a stale texture handle");
        const auto expectedLayout = it->second.desc.format == PixelFormat::depth32Float
                                        ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                        : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (it->second.layout != expectedLayout)
            throw std::logic_error("typed rendering attachment is not in its rendering layout");
    }
    vkCmdEndRendering(commandBuffer);
    for (const auto target : targets) {
        const auto it = typedTextures_.find(target);
        recordTextureTransition(commandBuffer, target, typedTextureFinalLayout(it->second));
    }
}

void VulkanDevice::recordMemoryBarrier(VkCommandBuffer commandBuffer) {
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("typed memory barrier requires a command buffer");
    const VkMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        // Native resources are consumed by raster, compute, ray tracing, and
        // copy commands. Keep this generic barrier broad enough to cover all
        // producers and consumers, including writes from the preceding frame
        // submission that feed the current frame's history copy.
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void VulkanDevice::recordTransferBarrier(VkCommandBuffer commandBuffer) {
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("typed transfer barrier requires a command buffer");
    const VkMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void VulkanDevice::recordAccelerationStructureBarrier(VkCommandBuffer commandBuffer) {
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("acceleration barrier requires a command buffer");
    const VkMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask =
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                         VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    };
    const VkDependencyInfo dependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void VulkanDevice::recordBlasUpdate(VkCommandBuffer commandBuffer, handles::AccelerationStructureHandle blas,
                                    const BlasGeometryDesc& geometry) {
    const auto it = typedAccelerationStructures_.find(blas);
    if (it == typedAccelerationStructures_.end() || !typedAccelerationStructureHandles_.isAlive(blas) ||
        it->second.topLevel)
        throw std::invalid_argument("record BLAS update references a stale handle");
    recordAccelerationBuildOnCommand(commandBuffer, it->second, geometry, true);
}

void VulkanDevice::recordTlasUpdate(VkCommandBuffer commandBuffer, handles::AccelerationStructureHandle tlas,
                                    std::span<const AccelerationInstanceDesc> instances) {
    const auto it = typedAccelerationStructures_.find(tlas);
    if (it == typedAccelerationStructures_.end() || !typedAccelerationStructureHandles_.isAlive(tlas) ||
        !it->second.topLevel)
        throw std::invalid_argument("record TLAS update references a stale handle");
    recordTopLevelBuildOnCommand(commandBuffer, it->second, instances, true);
}

handles::DescriptorSetLayoutHandle VulkanDevice::createDescriptorSetLayoutEx(const DescriptorSetLayoutDesc& desc) {
    if (desc.bindings.empty())
        throw std::invalid_argument("typed descriptor set layout must contain a binding");

    const auto descriptorType = [](DescriptorKind kind) {
        switch (kind) {
        case DescriptorKind::sampler:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case DescriptorKind::sampledImage:
            return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case DescriptorKind::combinedImageSampler:
            return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case DescriptorKind::storageImage:
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case DescriptorKind::uniformBuffer:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case DescriptorKind::storageBuffer:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case DescriptorKind::accelerationStructure:
            return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        }
        return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    };

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(desc.bindings.size());
    std::unordered_set<std::uint32_t> seen;
    for (const auto& binding : desc.bindings) {
        if (binding.count == 0 || binding.stages == ShaderStageMask::none || !seen.insert(binding.binding).second)
            throw std::invalid_argument("invalid or duplicate typed descriptor binding");
        if (binding.kind == DescriptorKind::accelerationStructure && !capabilities_.accelerationStructure)
            throw std::runtime_error("acceleration-structure descriptors are unsupported by this Vulkan device");
        bindings.push_back(
            {binding.binding, descriptorType(binding.kind), binding.count, toVkShaderStages(binding.stages), nullptr});
    }

    const VkDescriptorSetLayoutCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<std::uint32_t>(bindings.size()),
        .pBindings = bindings.data(),
    };
    TypedDescriptorSetLayout typed{.desc = desc};
    check(vkCreateDescriptorSetLayout(device_, &createInfo, nullptr, &typed.layout),
          "create typed descriptor set layout");
    const auto handle = typedDescriptorSetLayoutHandles_.create();
    typedDescriptorSetLayouts_.emplace(handle, std::move(typed));
    return handle;
}

handles::DescriptorSetHandle VulkanDevice::allocateDescriptorSetEx(handles::DescriptorSetLayoutHandle layout,
                                                                   std::span<const DescriptorBindingEx> bindings) {
    const auto layoutIt = typedDescriptorSetLayouts_.find(layout);
    if (layoutIt == typedDescriptorSetLayouts_.end() || !typedDescriptorSetLayoutHandles_.isAlive(layout))
        throw std::invalid_argument("stale typed descriptor set layout handle");
    if (typedDescriptorPool_ == VK_NULL_HANDLE)
        throw std::runtime_error("typed descriptor pool is unavailable");

    const VkDescriptorSetAllocateInfo allocateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = typedDescriptorPool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &layoutIt->second.layout,
    };
    TypedDescriptorSet typed{.layout = layout};
    check(vkAllocateDescriptorSets(device_, &allocateInfo, &typed.set), "allocate typed descriptor set");
    const auto handle = typedDescriptorSetHandles_.create();
    try {
        typedDescriptorSets_.emplace(handle, typed);
        updateDescriptorSetEx(handle, bindings);
    } catch (...) {
        typedDescriptorSets_.erase(handle);
        typedDescriptorSetHandles_.destroy(handle);
        vkFreeDescriptorSets(device_, typedDescriptorPool_, 1, &typed.set);
        throw;
    }
    return handle;
}

void VulkanDevice::updateDescriptorSetEx(handles::DescriptorSetHandle set,
                                         std::span<const DescriptorBindingEx> bindings) {
    const auto setIt = typedDescriptorSets_.find(set);
    if (setIt == typedDescriptorSets_.end() || !typedDescriptorSetHandles_.isAlive(set))
        throw std::invalid_argument("stale typed descriptor set handle");
    const auto layoutIt = typedDescriptorSetLayouts_.find(setIt->second.layout);
    if (layoutIt == typedDescriptorSetLayouts_.end())
        throw std::logic_error("typed descriptor set refers to a missing layout");

    const auto findLayoutBinding = [&layoutIt](std::uint32_t slot) -> const DescriptorSetLayoutBinding* {
        for (const auto& binding : layoutIt->second.desc.bindings)
            if (binding.binding == slot)
                return &binding;
        return nullptr;
    };
    const auto descriptorType = [](DescriptorKind kind) {
        switch (kind) {
        case DescriptorKind::sampler:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case DescriptorKind::sampledImage:
            return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case DescriptorKind::combinedImageSampler:
            return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case DescriptorKind::storageImage:
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case DescriptorKind::uniformBuffer:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case DescriptorKind::storageBuffer:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case DescriptorKind::accelerationStructure:
            return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        }
        return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    };

    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufferInfos;
    std::vector<VkDescriptorImageInfo> imageInfos;
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> accelerationInfos;
    writes.reserve(bindings.size());
    bufferInfos.reserve(bindings.size());
    imageInfos.reserve(bindings.size());
    accelerationInfos.reserve(bindings.size());
    std::unordered_set<std::uint64_t> seen;
    for (const auto& binding : bindings) {
        const auto* layoutBinding = findLayoutBinding(binding.slot);
        if (layoutBinding == nullptr || binding.arrayElement >= layoutBinding->count)
            throw std::invalid_argument("typed descriptor binding is not present in its layout");
        const std::uint64_t key = (static_cast<std::uint64_t>(binding.slot) << 32U) | binding.arrayElement;
        if (!seen.insert(key).second)
            throw std::invalid_argument("duplicate typed descriptor update");

        const VkDescriptorType type = descriptorType(layoutBinding->kind);
        VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = setIt->second.set,
            .dstBinding = binding.slot,
            .dstArrayElement = binding.arrayElement,
            .descriptorCount = 1,
            .descriptorType = type,
        };
        switch (layoutBinding->kind) {
        case DescriptorKind::sampler: {
            const auto samplerIt = typedSamplers_.find(binding.sampler);
            if (samplerIt == typedSamplers_.end() || !typedSamplerHandles_.isAlive(binding.sampler))
                throw std::invalid_argument("stale typed sampler handle");
            imageInfos.push_back({samplerIt->second.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
            write.pImageInfo = &imageInfos.back();
            break;
        }
        case DescriptorKind::sampledImage:
        case DescriptorKind::storageImage:
        case DescriptorKind::combinedImageSampler: {
            const auto textureIt = typedTextures_.find(binding.texture);
            if (textureIt == typedTextures_.end() || !typedTextureHandles_.isAlive(binding.texture))
                throw std::invalid_argument("stale typed texture handle");
            VkDescriptorImageInfo info{VK_NULL_HANDLE, textureIt->second.view,
                                       layoutBinding->kind == DescriptorKind::storageImage
                                           ? VK_IMAGE_LAYOUT_GENERAL
                                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            if (layoutBinding->kind == DescriptorKind::combinedImageSampler) {
                const auto samplerIt = typedSamplers_.find(binding.sampler);
                if (samplerIt == typedSamplers_.end() || !typedSamplerHandles_.isAlive(binding.sampler))
                    throw std::invalid_argument("stale typed sampler handle");
                info.sampler = samplerIt->second.sampler;
            }
            imageInfos.push_back(info);
            write.pImageInfo = &imageInfos.back();
            break;
        }
        case DescriptorKind::uniformBuffer:
        case DescriptorKind::storageBuffer: {
            const auto bufferIt = typedBuffers_.find(binding.buffer);
            if (bufferIt == typedBuffers_.end() || !typedBufferHandles_.isAlive(binding.buffer))
                throw std::invalid_argument("stale typed buffer handle");
            bufferInfos.push_back({bufferIt->second.resource.buffer, 0, bufferIt->second.resource.size});
            write.pBufferInfo = &bufferInfos.back();
            break;
        }
        case DescriptorKind::accelerationStructure: {
            const auto accelerationIt = typedAccelerationStructures_.find(binding.accelerationStructure);
            if (accelerationIt == typedAccelerationStructures_.end() ||
                !typedAccelerationStructureHandles_.isAlive(binding.accelerationStructure) ||
                accelerationIt->second.structure == VK_NULL_HANDLE)
                throw std::invalid_argument("stale typed acceleration-structure handle");
            accelerationInfos.push_back({
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                .accelerationStructureCount = 1,
                .pAccelerationStructures = &accelerationIt->second.structure,
            });
            write.pNext = &accelerationInfos.back();
            break;
        }
        }
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void VulkanDevice::destroyDescriptorSetEx(handles::DescriptorSetHandle handle) {
    const auto it = typedDescriptorSets_.find(handle);
    if (it == typedDescriptorSets_.end() || !typedDescriptorSetHandles_.isAlive(handle))
        throw std::invalid_argument("stale typed descriptor set handle");
    check(vkFreeDescriptorSets(device_, typedDescriptorPool_, 1, &it->second.set), "free typed descriptor set");
    typedDescriptorSets_.erase(it);
    typedDescriptorSetHandles_.destroy(handle);
}

void VulkanDevice::destroyDescriptorSetLayoutEx(handles::DescriptorSetLayoutHandle handle) {
    const auto it = typedDescriptorSetLayouts_.find(handle);
    if (it == typedDescriptorSetLayouts_.end() || !typedDescriptorSetLayoutHandles_.isAlive(handle))
        throw std::invalid_argument("stale typed descriptor set layout handle");
    vkDestroyDescriptorSetLayout(device_, it->second.layout, nullptr);
    typedDescriptorSetLayouts_.erase(it);
    typedDescriptorSetLayoutHandles_.destroy(handle);
}

void VulkanDevice::destroyBufferEx(handles::BufferHandle handle) {
    const auto it = typedBuffers_.find(handle);
    if (it == typedBuffers_.end() || !typedBufferHandles_.isAlive(handle))
        throw std::invalid_argument("stale typed buffer handle");
    if (it->second.mapped != nullptr)
        vkUnmapMemory(device_, it->second.resource.memory);
    if (it->second.resource.buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device_, it->second.resource.buffer, nullptr);
    if (it->second.resource.memory != VK_NULL_HANDLE)
        vkFreeMemory(device_, it->second.resource.memory, nullptr);
    typedBuffers_.erase(it);
    typedBufferHandles_.destroy(handle);
}

void VulkanDevice::uploadBufferEx(handles::BufferHandle handle, std::span<const std::byte> bytes, std::size_t offset) {
    const auto it = typedBuffers_.find(handle);
    if (it == typedBuffers_.end() || !typedBufferHandles_.isAlive(handle))
        throw std::invalid_argument("stale typed buffer handle");
    if (offset > it->second.desc.size || bytes.size() > it->second.desc.size - offset)
        throw std::out_of_range("typed buffer upload exceeds allocation");
    if (bytes.empty())
        return;
    if (it->second.mapped != nullptr) {
        std::memcpy(static_cast<std::byte*>(it->second.mapped) + offset, bytes.data(), bytes.size());
        return;
    }
    if ((toBits(it->second.desc.usage) & toBits(ResourceUsage::transferDst)) == 0U)
        throw std::logic_error("device-local typed buffer upload requires transfer-destination usage");
    if (uploadContext_ == nullptr)
        throw std::logic_error("Vulkan upload context is unavailable");
    try {
        uploadContext_->begin();
        const auto staging = uploadContext_->allocate(bytes.size(), 4);
        std::memcpy(staging.mapped, bytes.data(), bytes.size());
        const VkBufferCopy copy{.srcOffset = staging.offset, .dstOffset = offset, .size = bytes.size()};
        vkCmdCopyBuffer(uploadContext_->commandBuffer(), staging.buffer, it->second.resource.buffer, 1, &copy);
        const VkBufferMemoryBarrier2 visible{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = it->second.resource.buffer,
            .offset = offset,
            .size = bytes.size(),
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &visible,
        };
        vkCmdPipelineBarrier2(uploadContext_->commandBuffer(), &dependency);
        const auto signal = uploadContext_->submit();
        uploadContext_->wait(signal);
    } catch (...) {
        uploadContext_->abort();
        throw;
    }
}

std::vector<std::byte> VulkanDevice::readbackBufferEx(handles::BufferHandle handle, std::size_t offset,
                                                      std::size_t size) {
    const auto it = typedBuffers_.find(handle);
    if (it == typedBuffers_.end() || !typedBufferHandles_.isAlive(handle))
        throw std::invalid_argument("stale typed buffer handle");
    if (offset > it->second.desc.size || size > it->second.desc.size - offset)
        throw std::out_of_range("typed buffer readback exceeds allocation");
    if (it->second.mapped == nullptr)
        throw std::logic_error("typed buffer readback requires a CPU-visible buffer");
    std::vector<std::byte> bytes(size);
    if (!bytes.empty())
        std::memcpy(bytes.data(), static_cast<const std::byte*>(it->second.mapped) + offset, size);
    return bytes;
}

void VulkanDevice::destroyTextureEx(handles::TextureHandle handle) {
    const auto it = typedTextures_.find(handle);
    if (it == typedTextures_.end() || !typedTextureHandles_.isAlive(handle))
        throw std::invalid_argument("stale typed texture handle");
    if (it->second.view != VK_NULL_HANDLE)
        vkDestroyImageView(device_, it->second.view, nullptr);
    if (it->second.resource.image != VK_NULL_HANDLE)
        vkDestroyImage(device_, it->second.resource.image, nullptr);
    if (it->second.resource.memory != VK_NULL_HANDLE)
        vkFreeMemory(device_, it->second.resource.memory, nullptr);
    typedTextures_.erase(it);
    typedTextureHandles_.destroy(handle);
}

void VulkanDevice::retireBufferEx(handles::BufferHandle handle, std::uint64_t) {
    waitIdle();
    destroyBufferEx(handle);
}

void VulkanDevice::retireTextureEx(handles::TextureHandle handle, std::uint64_t) {
    waitIdle();
    destroyTextureEx(handle);
}

void VulkanDevice::destroyTypedResources() noexcept {
    const auto destroyAccelerationStructure = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device_, "vkDestroyAccelerationStructureKHR"));
    for (const auto& [handle, resource] : typedAccelerationStructures_) {
        static_cast<void>(handle);
        if (destroyAccelerationStructure != nullptr && resource.structure != VK_NULL_HANDLE)
            destroyAccelerationStructure(device_, resource.structure, nullptr);
        if (resource.mappedInstances != nullptr)
            vkUnmapMemory(device_, resource.instanceMemory);
        if (resource.instanceMemory != VK_NULL_HANDLE)
            vkFreeMemory(device_, resource.instanceMemory, nullptr);
        if (resource.instanceBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, resource.instanceBuffer, nullptr);
        if (resource.storageMemory != VK_NULL_HANDLE)
            vkFreeMemory(device_, resource.storageMemory, nullptr);
        if (resource.storageBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, resource.storageBuffer, nullptr);
    }
    typedAccelerationStructures_.clear();
    typedAccelerationStructureHandles_.clear();
    for (const auto& [handle, resource] : typedShaderBindingTables_) {
        static_cast<void>(handle);
        if (resource.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, resource.buffer, nullptr);
        if (resource.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, resource.memory, nullptr);
    }
    typedShaderBindingTables_.clear();
    typedShaderBindingTableHandles_.clear();
    for (const auto& [handle, resource] : typedPipelines_) {
        static_cast<void>(handle);
        if (resource.pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(device_, resource.pipeline, nullptr);
    }
    typedPipelines_.clear();
    typedPipelineHandles_.clear();
    for (const auto& [handle, resource] : typedPipelineLayouts_) {
        static_cast<void>(handle);
        if (resource.layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(device_, resource.layout, nullptr);
    }
    typedPipelineLayouts_.clear();
    typedPipelineLayoutHandles_.clear();
    for (const auto& [handle, resource] : typedShaders_) {
        static_cast<void>(handle);
        if (resource.module != VK_NULL_HANDLE)
            vkDestroyShaderModule(device_, resource.module, nullptr);
    }
    typedShaders_.clear();
    typedShaderHandles_.clear();
    nativeFullscreenVertexShader_ = {};
    typedDescriptorSets_.clear();
    typedDescriptorSetHandles_.clear();
    if (typedDescriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, typedDescriptorPool_, nullptr);
        typedDescriptorPool_ = VK_NULL_HANDLE;
    }
    for (const auto& [handle, resource] : typedDescriptorSetLayouts_) {
        static_cast<void>(handle);
        if (resource.layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device_, resource.layout, nullptr);
    }
    typedDescriptorSetLayouts_.clear();
    typedDescriptorSetLayoutHandles_.clear();
    for (const auto& [handle, resource] : typedSamplers_) {
        static_cast<void>(handle);
        if (resource.sampler != VK_NULL_HANDLE)
            vkDestroySampler(device_, resource.sampler, nullptr);
    }
    typedSamplers_.clear();
    typedSamplerHandles_.clear();
    for (const auto& [handle, resource] : typedTextures_) {
        static_cast<void>(handle);
        if (resource.view != VK_NULL_HANDLE)
            vkDestroyImageView(device_, resource.view, nullptr);
        if (resource.resource.image != VK_NULL_HANDLE)
            vkDestroyImage(device_, resource.resource.image, nullptr);
        if (resource.resource.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, resource.resource.memory, nullptr);
    }
    typedTextures_.clear();
    typedTextureHandles_.clear();
    for (const auto& [handle, resource] : typedBuffers_) {
        static_cast<void>(handle);
        if (resource.mapped != nullptr)
            vkUnmapMemory(device_, resource.resource.memory);
        if (resource.resource.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, resource.resource.buffer, nullptr);
        if (resource.resource.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, resource.resource.memory, nullptr);
    }
    typedBuffers_.clear();
    typedBufferHandles_.clear();
}

std::uint32_t VulkanDevice::findMemoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &properties);
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1U << i)) != 0 && (properties.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    throw std::runtime_error("no compatible Vulkan memory type");
}

BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc) {
    if (desc.size == 0)
        throw std::invalid_argument("buffer size must be non-zero");
    VulkanBuffer resource;
    resource.size = desc.size;
    const VkBufferCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = desc.size,
        .usage = toVkUsage(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
#if DAYO_ENABLE_VMA
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.requiredFlags = desc.cpuVisible
                                       ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                       : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (desc.cpuVisible)
        allocationInfo.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    check(vmaCreateBuffer(allocator_, &createInfo, &allocationInfo, &resource.buffer, &resource.allocation, nullptr),
          "create VMA buffer");
#else
    check(vkCreateBuffer(device_, &createInfo, nullptr, &resource.buffer), "create buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, resource.buffer, &requirements);
    const VkMemoryPropertyFlags flags = desc.cpuVisible
                                            ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryAllocateFlagsInfo allocationFlags{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = capabilities_.bufferDeviceAddress
                     ? static_cast<VkMemoryAllocateFlags>(VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT)
                     : VkMemoryAllocateFlags{},
    };
    const VkMemoryAllocateInfo allocationInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = capabilities_.bufferDeviceAddress ? &allocationFlags : nullptr,
        .allocationSize = requirements.size,
        .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, flags),
    };
    try {
        check(vkAllocateMemory(device_, &allocationInfo, nullptr, &resource.memory), "allocate buffer memory");
        check(vkBindBufferMemory(device_, resource.buffer, resource.memory, 0), "bind buffer memory");
    } catch (...) {
        if (resource.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, resource.memory, nullptr);
        vkDestroyBuffer(device_, resource.buffer, nullptr);
        throw;
    }
#endif
    const auto handle = nextResourceHandle_++;
    buffers_.emplace(handle, resource);
    return handle;
}

TextureHandle VulkanDevice::createTexture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0)
        throw std::invalid_argument("texture size must be non-zero");
    VulkanImage resource;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc.storage)
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (desc.renderTarget)
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc.format == TextureDesc::Format::depth32Float)
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    const VkImageCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = toVkFormat(desc.format),
        .extent = {desc.width, desc.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
#if DAYO_ENABLE_VMA
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    check(vmaCreateImage(allocator_, &createInfo, &allocationInfo, &resource.image, &resource.allocation, nullptr),
          "create VMA texture");
#else
    check(vkCreateImage(device_, &createInfo, nullptr, &resource.image), "create texture");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, resource.image, &requirements);
    const VkMemoryAllocateInfo allocationInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    try {
        check(vkAllocateMemory(device_, &allocationInfo, nullptr, &resource.memory), "allocate texture memory");
        check(vkBindImageMemory(device_, resource.image, resource.memory, 0), "bind texture memory");
    } catch (...) {
        if (resource.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, resource.memory, nullptr);
        vkDestroyImage(device_, resource.image, nullptr);
        throw;
    }
#endif
    const auto handle = nextResourceHandle_++;
    textures_.emplace(handle, resource);
    return handle;
}

std::unique_ptr<Device> createVulkanDevice(platform::Window& window, bool validation) {
    return std::make_unique<VulkanDevice>(window, validation);
}

} // namespace dayo::graphics
