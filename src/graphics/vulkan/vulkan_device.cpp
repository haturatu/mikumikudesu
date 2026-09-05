#include "graphics/vulkan/vulkan_device.hpp"

#include "core/log.hpp"
#include "graphics/timestamp.hpp"
#include "platform/window.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#if DAYO_HAS_IMGUI
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace dayo::graphics {
namespace {

void check(VkResult result, std::string_view operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
    }
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

} // namespace

VulkanDevice::VulkanDevice(platform::Window& window, bool validation) : window_(window), validation_(validation) {
    createInstance(validation);
    createSurface();
    selectPhysicalDevice();
    queryCapabilities();
    createLogicalDevice();
    createSwapchain();
    createPreviewDescriptors();
    createPipeline();
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
    uploadPreviewTextures(std::span<const PreviewTexture>{});
    updatePreviewMaterials(std::span<const PreviewMaterial>{});
    log::info("Vulkan device ready: ", capabilities_.gpuName, " (", capabilities_.driverName, ")");
}

VulkanDevice::~VulkanDevice() {
    if (device_ != VK_NULL_HANDLE)
        vkDeviceWaitIdle(device_);
    destroyUi();
    destroyOffscreenResource();
    destroyPreviewMesh();
    destroyPreviewBones();
    destroyPreviewBackground();
    destroyPreviewTextures();
    for (const auto& [handle, resource] : textures_) {
        static_cast<void>(handle);
        if (resource.image != VK_NULL_HANDLE)
            vkDestroyImage(device_, resource.image, nullptr);
        if (resource.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, resource.memory, nullptr);
    }
    for (const auto& [handle, resource] : buffers_) {
        static_cast<void>(handle);
        if (resource.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, resource.buffer, nullptr);
        if (resource.memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, resource.memory, nullptr);
    }
    destroyFrames();
    destroyPipeline();
    destroyPreviewDescriptors();
    destroySwapchain();
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

    VkPhysicalDeviceDriverProperties driver{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
    };
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
    capabilities_.bufferDeviceAddress = vulkan12.bufferDeviceAddress == VK_TRUE;
    capabilities_.descriptorIndexing =
        vulkan12.runtimeDescriptorArray == VK_TRUE && vulkan12.descriptorBindingPartiallyBound == VK_TRUE;
    capabilities_.accelerationStructure = acceleration.accelerationStructure == VK_TRUE &&
                                          hasName(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    capabilities_.rayTracingPipeline =
        rayPipeline.rayTracingPipeline == VK_TRUE && hasName(extensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    capabilities_.rayQuery = rayQuery.rayQuery == VK_TRUE && hasName(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    capabilities_.fragmentShaderBarycentric = barycentric.fragmentShaderBarycentric == VK_TRUE &&
                                              hasName(extensions, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);

    if (!capabilities_.swapchain)
        throw std::runtime_error("selected GPU lacks VK_KHR_swapchain");
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
        .descriptorIndexing = capabilities_.descriptorIndexing,
        .descriptorBindingPartiallyBound = capabilities_.descriptorIndexing,
        .runtimeDescriptorArray = capabilities_.descriptorIndexing,
        .bufferDeviceAddress = capabilities_.bufferDeviceAddress,
    };
    const VkPhysicalDeviceFeatures2 features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vulkan12,
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
        VkVertexInputAttributeDescription{9, 0, VK_FORMAT_R32_SFLOAT, offsetof(PreviewVertex, cloneOffset)},
        VkVertexInputAttributeDescription{10, 0, VK_FORMAT_R32_SFLOAT, offsetof(PreviewVertex, edgeScale)},
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
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
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
        previewDescriptorSetLayout_,
        previewSkinningDescriptorSetLayout_,
        previewMaterialDescriptorSetLayout_,
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
    const auto result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
    auto transparentPipelineInfo = pipelineInfo;
    transparentPipelineInfo.pDepthStencilState = &transparentDepthStencil;
    transparentPipelineInfo.pColorBlendState = &transparentBlend;
    const auto transparentResult =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &transparentPipelineInfo, nullptr, &transparentPipeline_);
    auto backgroundPipelineInfo = pipelineInfo;
    backgroundPipelineInfo.pDepthStencilState = &backgroundDepthStencil;
    const auto backgroundResult =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &backgroundPipelineInfo, nullptr, &backgroundPipeline_);
    auto edgePipelineInfo = pipelineInfo;
    edgePipelineInfo.pStages = edgeStages.data();
    edgePipelineInfo.pDepthStencilState = &edgeDepthStencil;
    edgePipelineInfo.pRasterizationState = &edgeRasterizer;
    edgePipelineInfo.pColorBlendState = &transparentBlend;
    const auto edgeResult =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &edgePipelineInfo, nullptr, &edgePipeline_);
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

void VulkanDevice::createPreviewDescriptors() {
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
    const std::array poolSizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 32768},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 16384},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64},
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
    previewClampSampler_ = VK_NULL_HANDLE;
    previewSampler_ = VK_NULL_HANDLE;
    previewDescriptorPool_ = VK_NULL_HANDLE;
    previewDescriptorSetLayout_ = VK_NULL_HANDLE;
    previewSkinningDescriptorSetLayout_ = VK_NULL_HANDLE;
    previewMaterialDescriptorSetLayout_ = VK_NULL_HANDLE;
}

void VulkanDevice::destroyPreviewTextures() {
    destroyPreviewMaterialDescriptors();
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
    VkBuffer staging{};
    VkDeviceMemory stagingMemory{};
    const VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = byteSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    check(vkCreateBuffer(device_, &bufferInfo, nullptr, &staging), "create texture staging buffer");
    VkMemoryRequirements stagingRequirements{};
    vkGetBufferMemoryRequirements(device_, staging, &stagingRequirements);
    const VkMemoryAllocateInfo stagingAllocation{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = stagingRequirements.size,
        .memoryTypeIndex = findMemoryType(stagingRequirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    check(vkAllocateMemory(device_, &stagingAllocation, nullptr, &stagingMemory), "allocate texture staging memory");
    check(vkBindBufferMemory(device_, staging, stagingMemory, 0), "bind texture staging memory");
    void* mapped = nullptr;
    check(vkMapMemory(device_, stagingMemory, 0, byteSize, 0, &mapped), "map texture staging memory");
    std::memcpy(mapped, rgba.data(), rgba.size_bytes());
    vkUnmapMemory(device_, stagingMemory);

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

    VkCommandPool uploadPool{};
    const VkCommandPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = queueFamily_,
    };
    check(vkCreateCommandPool(device_, &poolInfo, nullptr, &uploadPool), "create texture upload pool");
    VkCommandBuffer command{};
    const VkCommandBufferAllocateInfo commandInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = uploadPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    check(vkAllocateCommandBuffers(device_, &commandInfo, &command), "allocate texture upload command");
    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    check(vkBeginCommandBuffer(command, &beginInfo), "begin texture upload");
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
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {width, height, 1},
    };
    vkCmdCopyBufferToImage(command, staging, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
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
    check(vkEndCommandBuffer(command), "end texture upload");
    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command,
    };
    check(vkQueueSubmit(queue_, 1, &submitInfo, VK_NULL_HANDLE), "submit texture upload");
    check(vkQueueWaitIdle(queue_), "wait for texture upload");
    vkDestroyCommandPool(device_, uploadPool, nullptr);
    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);

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
    for (auto& frame : frames_) {
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
    ImGui::StyleColorsDark();
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

void VulkanDevice::resize() {
    swapchainDirty_ = true;
}

void VulkanDevice::recreateSwapchain() {
    if (window_.pixelWidth() == 0 || window_.pixelHeight() == 0)
        return;
    check(vkDeviceWaitIdle(device_), "wait before swapchain recreation");
    destroyUi();
    destroyOffscreenResource();
    destroyPipeline();
    destroySwapchain();
    createSwapchain();
    createPipeline();
    createUi();
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

void VulkanDevice::recordPreviewModel(VkCommandBuffer command, const PreviewPushConstants& constants) {
    auto& frame = frames_[frameIndex_];
    if (frame.previewMaterialDescriptor == VK_NULL_HANDLE)
        return;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1,
                            &frame.previewMaterialDescriptor, 0, nullptr);

    const auto textureDescriptor = [&](std::uint32_t materialIndex) {
        if (materialIndex < previewMaterialDescriptors_.size()) {
            return previewMaterialDescriptors_[materialIndex];
        }
        return previewTextures_.empty() ? VK_NULL_HANDLE : previewTextures_.front().descriptor;
    };
    const auto draw = [&](const PreviewDraw& item, VkPipeline pipeline) {
        if (item.materialIndex >= previewMaterials_.size() || item.indexCount == 0 ||
            item.firstIndex >= previewIndexCount_)
            return;
        const auto descriptor = textureDescriptor(item.materialIndex);
        if (descriptor == VK_NULL_HANDLE)
            return;
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptor, 0,
                                nullptr);
        vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(constants), &constants);
        const auto count = std::min(item.indexCount, previewIndexCount_ - item.firstIndex);
        vkCmdDrawIndexed(command, count, 1, item.firstIndex, 0, item.materialIndex);
    };

    if (previewMaterials_.empty() || previewDraws_.empty()) {
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

    // Original Preview.fxdayo draws all PMX materials in file order with the MMD blend/depth state.
    for (const auto& item : previewDraws_)
        draw(item, transparentPipeline_);

    // Preview.fxdayo has no outline pass. Keep PMX edge data available for a future renderer pass,
    // but do not draw it in the original Preview-compatible path.
}

void VulkanDevice::renderFrame() {
    if (window_.pixelWidth() == 0 || window_.pixelHeight() == 0)
        return;
#if DAYO_HAS_IMGUI
    ImGui::Render();
#endif
    auto& frame = frames_[frameIndex_];
    check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait for frame");
    resolveTimestampQuery(frame);
    synchronizePreviewVertices(frame);
    synchronizePreviewBones(frame);
    synchronizePreviewMaterials(frame);

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
    auto& depth = swapchainDepth_[imageIndex];
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
    const VkDependencyInfo toColorDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<std::uint32_t>(renderBarriers.size()),
        .pImageMemoryBarriers = renderBarriers.data(),
    };
    vkCmdPipelineBarrier2(frame.commandBuffer, &toColorDependency);

    VkClearValue clear{};
    clear.color = !previewScene_.backgroundEnabled || previewScene_.screenSource == PreviewScene::ScreenSource::white
                      ? VkClearColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}
                  : activeRenderer_ == RendererKind::preview ? VkClearColorValue{{0.025F, 0.035F, 0.055F, 1.0F}}
                                                             : VkClearColorValue{{0.055F, 0.025F, 0.045F, 1.0F}};
    const VkRenderingAttachmentInfo attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = swapchainViews_[imageIndex],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = previewScene_.backgroundEnabled &&
                          previewScene_.screenSource == PreviewScene::ScreenSource::previousFrame &&
                          swapchainInitialized_[imageIndex]
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
        .renderArea = {{0, 0}, swapchainExtent_},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
        .pDepthAttachment = &depthAttachment,
    };
    vkCmdBeginRendering(frame.commandBuffer, &renderingInfo);
    depth.initialized = true;
    const VkViewport viewport{
        0.0F, 0.0F, static_cast<float>(swapchainExtent_.width), static_cast<float>(swapchainExtent_.height),
        0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, swapchainExtent_};
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                            &frame.previewBoneDescriptor, 0, nullptr);
    if (frame.previewMaterialDescriptor != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1,
                                &frame.previewMaterialDescriptor, 0, nullptr);
    }
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &frame.previewVertexBuffer, &vertexOffset);
    vkCmdBindIndexBuffer(frame.commandBuffer, previewIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    PreviewPushConstants constants;
    std::copy_n(previewScene_.cameraRotation, 3, constants.camera.begin());
    constants.camera[3] = previewScene_.cameraDistance;
    std::copy_n(previewScene_.target, 3, constants.target.begin());
    constants.target[3] =
        previewScene_.perspective ? previewScene_.verticalFovRadians : -previewScene_.verticalFovRadians;
    std::copy_n(previewScene_.lightDirection, 3, constants.light.begin());
    constants.light[3] = swapchainExtent_.height == 0
                             ? 1.0F
                             : static_cast<float>(swapchainExtent_.width) / static_cast<float>(swapchainExtent_.height);
    const bool hasBackground = previewScene_.backgroundEnabled &&
                               (previewScene_.screenSource == PreviewScene::ScreenSource::backgroundImage ||
                                previewScene_.screenSource == PreviewScene::ScreenSource::backgroundVideo) &&
                               previewBackgroundTexture_.descriptor != VK_NULL_HANDLE &&
                               previewBackgroundIndexCount_ != 0;
    if (hasBackground) {
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, backgroundPipeline_);
        const VkDeviceSize backgroundOffset = 0;
        vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &previewBackgroundVertexBuffer_, &backgroundOffset);
        vkCmdBindIndexBuffer(frame.commandBuffer, previewBackgroundIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &previewBackgroundTexture_.descriptor, 0, nullptr);
        vkCmdPushConstants(frame.commandBuffer, pipelineLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
        vkCmdDrawIndexed(frame.commandBuffer, previewBackgroundIndexCount_, 1, 0, 0, 0);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &frame.previewVertexBuffer, &vertexOffset);
        vkCmdBindIndexBuffer(frame.commandBuffer, previewIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    }
    if (!hasBackground) {
        vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &frame.previewVertexBuffer, &vertexOffset);
        vkCmdBindIndexBuffer(frame.commandBuffer, previewIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    }
    recordPreviewModel(frame.commandBuffer, constants);
#if DAYO_HAS_IMGUI
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commandBuffer);
#endif
    vkCmdEndRendering(frame.commandBuffer);
    if (frame.timestampQueryPool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampQueryPool, 1);

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

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &frame.imageAvailable,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = &frame.commandBuffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &frame.renderFinished,
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
    if (activeRenderer_ != RendererKind::preview) {
        throw std::runtime_error("video export currently supports the Preview renderer only");
    }
    if (target.width == 0 || target.height == 0)
        throw std::invalid_argument("video dimensions must be non-zero");
    const VkExtent2D extent{target.width, target.height};
    auto& frame = frames_[frameIndex_];
    check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait for offscreen frame slot");
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
    synchronizePreviewMaterials(frame);
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

    const VkImageMemoryBarrier2 toColor{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = offscreen_.colorInitialized ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = offscreen_.colorInitialized ? VK_ACCESS_2_TRANSFER_READ_BIT : 0U,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = offscreen_.colorInitialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = offscreen_.colorImage,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkImageMemoryBarrier2 toDepth{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask =
            offscreen_.depth.initialized ? VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT : VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = offscreen_.depth.initialized ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0U,
        .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout =
            offscreen_.depth.initialized ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = offscreen_.depth.image,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    const std::array renderBarriers{toColor, toDepth};
    const VkDependencyInfo renderDependency{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<std::uint32_t>(renderBarriers.size()),
        .pImageMemoryBarriers = renderBarriers.data(),
    };
    vkCmdPipelineBarrier2(frame.commandBuffer, &renderDependency);

    VkClearValue clear{};
    clear.color = !previewScene_.backgroundEnabled || previewScene_.screenSource == PreviewScene::ScreenSource::white
                      ? VkClearColorValue{{1.0F, 1.0F, 1.0F, 1.0F}}
                      : VkClearColorValue{{0.025F, 0.035F, 0.055F, 1.0F}};
    const VkRenderingAttachmentInfo colorAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = offscreen_.colorView,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = clear,
    };
    VkClearValue depthClear{};
    depthClear.depthStencil = {1.0F, 0};
    const VkRenderingAttachmentInfo depthAttachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = offscreen_.depth.view,
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
    vkCmdBeginRendering(frame.commandBuffer, &renderingInfo);
    offscreen_.depth.initialized = true;
    const VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height),
                              0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                            &frame.previewBoneDescriptor, 0, nullptr);
    if (frame.previewMaterialDescriptor != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1,
                                &frame.previewMaterialDescriptor, 0, nullptr);
    }
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &frame.previewVertexBuffer, &vertexOffset);
    vkCmdBindIndexBuffer(frame.commandBuffer, previewIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    PreviewPushConstants constants;
    std::copy_n(previewScene_.cameraRotation, 3, constants.camera.begin());
    constants.camera[3] = previewScene_.cameraDistance;
    std::copy_n(previewScene_.target, 3, constants.target.begin());
    constants.target[3] =
        previewScene_.perspective ? previewScene_.verticalFovRadians : -previewScene_.verticalFovRadians;
    std::copy_n(previewScene_.lightDirection, 3, constants.light.begin());
    constants.light[3] = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    const bool hasBackground = previewScene_.backgroundEnabled &&
                               (previewScene_.screenSource == PreviewScene::ScreenSource::backgroundImage ||
                                previewScene_.screenSource == PreviewScene::ScreenSource::backgroundVideo) &&
                               previewBackgroundTexture_.descriptor != VK_NULL_HANDLE &&
                               previewBackgroundIndexCount_ != 0;
    if (hasBackground) {
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, backgroundPipeline_);
        const VkDeviceSize backgroundOffset = 0;
        vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &previewBackgroundVertexBuffer_, &backgroundOffset);
        vkCmdBindIndexBuffer(frame.commandBuffer, previewBackgroundIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &previewBackgroundTexture_.descriptor, 0, nullptr);
        vkCmdPushConstants(frame.commandBuffer, pipelineLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
        vkCmdDrawIndexed(frame.commandBuffer, previewBackgroundIndexCount_, 1, 0, 0, 0);
        vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &frame.previewVertexBuffer, &vertexOffset);
        vkCmdBindIndexBuffer(frame.commandBuffer, previewIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    }
    if (!hasBackground) {
        vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &frame.previewVertexBuffer, &vertexOffset);
        vkCmdBindIndexBuffer(frame.commandBuffer, previewIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    }
    recordPreviewModel(frame.commandBuffer, constants);
    vkCmdEndRendering(frame.commandBuffer);
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
    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &frame.commandBuffer,
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
        if (!frames_.empty()) {
            const auto previousFrame = (frameIndex_ + frames_.size() - 1U) % frames_.size();
            resolveTimestampQuery(frames_[previousFrame]);
        }
    }
}

void VulkanDevice::destroyPreviewMesh() {
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
    previewVertexGeneration_ = 0;
    previewIndexBuffer_ = VK_NULL_HANDLE;
    previewIndexMemory_ = VK_NULL_HANDLE;
    previewIndexCount_ = 0;
    previewVertexUpdateCount_ = 0;
}

void VulkanDevice::synchronizePreviewVertices(Frame& frame) {
    if (frame.previewVertexGeneration == previewVertexGeneration_ || previewVertexSize_ == 0)
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

void VulkanDevice::refreshPreviewMaterialDescriptors() {
    std::vector<std::array<std::uint32_t, 3>> keys;
    keys.reserve(previewMaterials_.size());
    for (const auto& material : previewMaterials_) {
        keys.push_back({material.textureSlot, material.toonTextureSlot, material.sphereTextureSlot});
    }
    if (keys == previewMaterialDescriptorKeys_ && previewMaterialDescriptors_.size() == previewMaterials_.size())
        return;
    waitIdle();
    destroyPreviewMaterialDescriptors();
    if (previewMaterials_.empty() || previewTextures_.empty())
        return;

    previewMaterialDescriptors_.resize(previewMaterials_.size());
    const auto layout = previewDescriptorSetLayout_;
    const VkDescriptorSetAllocateInfo setInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = previewDescriptorPool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    };
    for (std::size_t index = 0; index < previewMaterials_.size(); ++index) {
        auto& material = previewMaterials_[index];
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
                                       VkDeviceMemory& memory) {
    const VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
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
    check(vkMapMemory(device_, memory, 0, size, 0, &mapped), "map preview mesh memory");
    std::memcpy(mapped, data, static_cast<std::size_t>(size));
    vkUnmapMemory(device_, memory);
}

void VulkanDevice::uploadPreviewMesh(std::span<const PreviewVertex> vertices, std::span<const std::uint32_t> indices) {
    if (vertices.empty() || indices.empty())
        throw std::invalid_argument("preview mesh is empty");
    waitIdle();
    destroyPreviewMesh();

    try {
        for (auto& frame : frames_) {
            uploadPreviewBuffer(vertices.data(), vertices.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                frame.previewVertexBuffer, frame.previewVertexMemory);
            check(vkMapMemory(device_, frame.previewVertexMemory, 0, vertices.size_bytes(), 0,
                              &frame.mappedPreviewVertices),
                  "persistently map preview vertices");
        }
        ++previewVertexGeneration_;
        for (auto& frame : frames_)
            frame.previewVertexGeneration = previewVertexGeneration_;
        previewVertexSize_ = vertices.size_bytes();
        uploadPreviewBuffer(indices.data(), indices.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, previewIndexBuffer_,
                            previewIndexMemory_);
        previewIndexCount_ = static_cast<std::uint32_t>(indices.size());
    } catch (...) {
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
    auto& frame = frames_[frameIndex_];
    if (vertices.size_bytes() != previewVertexSize_ || frame.mappedPreviewVertices == nullptr) {
        throw std::invalid_argument("preview vertex update has a different size");
    }
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
    if (byteSize != previewBoneSize_) {
        waitIdle();
        destroyPreviewBones();
        previewBoneSize_ = byteSize;
        ++previewBoneGeneration_;
        for (auto& frame : frames_) {
            uploadPreviewBuffer(bones.data(), byteSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, frame.previewBoneBuffer,
                                frame.previewBoneMemory);
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
            const VkDescriptorBufferInfo bufferInfo{frame.previewBoneBuffer, 0, previewBoneSize_};
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
    auto& frame = frames_[frameIndex_];
    check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait for preview bone frame");
    std::memcpy(frame.mappedPreviewBones, bones.data(), bones.size_bytes());
    frame.previewBoneGeneration = ++previewBoneGeneration_;
}

void VulkanDevice::updatePreviewMaterials(std::span<const PreviewMaterial> materials) {
    previewMaterials_.assign(materials.begin(), materials.end());
    refreshPreviewMaterialDescriptors();

    previewMaterialData_.clear();
    previewMaterialData_.reserve(std::max<std::size_t>(previewMaterials_.size(), 1U));
    for (const auto& material : previewMaterials_) {
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
        previewMaterialData_.push_back(gpu);
    }
    if (previewMaterialData_.empty()) {
        PreviewMaterialGpu fallback;
        fallback.diffuse[0] = fallback.diffuse[1] = fallback.diffuse[2] = fallback.diffuse[3] = 1.0F;
        fallback.ambientShininess[0] = fallback.ambientShininess[1] = fallback.ambientShininess[2] = 1.0F;
        fallback.textureMultiply[0] = fallback.textureMultiply[1] = fallback.textureMultiply[2] =
            fallback.textureMultiply[3] = 1.0F;
        previewMaterialData_.push_back(fallback);
    }
    const auto byteSize = static_cast<VkDeviceSize>(previewMaterialData_.size() * sizeof(PreviewMaterialGpu));
    if (byteSize != previewMaterialSize_) {
        waitIdle();
        destroyPreviewMaterialBuffers();
        previewMaterialSize_ = byteSize;
        ++previewMaterialGeneration_;
        for (auto& frame : frames_) {
            uploadPreviewBuffer(previewMaterialData_.data(), byteSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                frame.previewMaterialBuffer, frame.previewMaterialMemory);
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
                previewMaterialSize_,
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
        auto& frame = frames_[frameIndex_];
        check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait for preview material frame");
        std::memcpy(frame.mappedPreviewMaterials, previewMaterialData_.data(), static_cast<std::size_t>(byteSize));
        frame.previewMaterialGeneration = ++previewMaterialGeneration_;
    }
}

void VulkanDevice::updatePreviewDraws(std::span<const PreviewDraw> draws) {
    previewDraws_.assign(draws.begin(), draws.end());
}

void VulkanDevice::uploadPreviewTextures(std::span<const PreviewTexture> textures) {
    waitIdle();
    destroyPreviewTextures();
    const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    createPreviewTexture(1, 1, white);
    for (const auto& texture : textures) {
        if (texture.width == 0 || texture.height == 0 || texture.rgba.empty()) {
            // Preserve source texture numbering with a white placeholder.
            createPreviewTexture(1, 1, white);
        } else {
            createPreviewTexture(texture.width, texture.height, texture.rgba);
        }
    }
    refreshPreviewMaterialDescriptors();
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
    destroyPreviewBackground();
    uploadPreviewTextures(std::span<const PreviewTexture>{});
    updatePreviewMaterials(std::span<const PreviewMaterial>{});
    previewDraws_.clear();
    previewScene_ = {};
    previewScene_.screenSource = PreviewScene::ScreenSource::white;
    std::fill(swapchainInitialized_.begin(), swapchainInitialized_.end(), false);
    log::info("Cleared preview GPU resources");
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
    BufferResource resource;
    const VkBufferCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = desc.size,
        .usage = toVkUsage(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
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
    const auto handle = nextResourceHandle_++;
    buffers_.emplace(handle, resource);
    return handle;
}

TextureHandle VulkanDevice::createTexture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0)
        throw std::invalid_argument("texture size must be non-zero");
    TextureResource resource;
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
    const auto handle = nextResourceHandle_++;
    textures_.emplace(handle, resource);
    return handle;
}

std::unique_ptr<Device> createVulkanDevice(platform::Window& window, bool validation) {
    return std::make_unique<VulkanDevice>(window, validation);
}

} // namespace dayo::graphics
