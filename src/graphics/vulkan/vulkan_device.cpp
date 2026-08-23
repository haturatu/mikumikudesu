#include "graphics/vulkan/vulkan_device.hpp"

#include "core/log.hpp"
#include "platform/window.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#if DAYO_HAS_IMGUI
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
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
        throw std::runtime_error(std::string(operation) + " failed with VkResult "
                                 + std::to_string(result));
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void*) {
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
    if (!input) throw std::runtime_error(std::string("cannot open shader: ") + filename);
    const auto end = input.tellg();
    if (end <= 0) throw std::runtime_error(std::string("empty shader: ") + filename);
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) throw std::runtime_error(std::string("cannot read shader: ") + filename);
    return bytes;
}

bool hasName(std::span<const VkExtensionProperties> values, const char* name) {
    return std::ranges::any_of(values, [name](const VkExtensionProperties& value) {
        return std::strcmp(value.extensionName, name) == 0;
    });
}

bool hasLayer(std::span<const VkLayerProperties> values, const char* name) {
    return std::ranges::any_of(values, [name](const VkLayerProperties& value) {
        return std::strcmp(value.layerName, name) == 0;
    });
}

VkFormat toVkFormat(TextureDesc::Format format) {
    switch (format) {
    case TextureDesc::Format::rgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureDesc::Format::rgba8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case TextureDesc::Format::rgba16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureDesc::Format::rgba32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TextureDesc::Format::depth32Float: return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkBufferUsageFlags toVkUsage(BufferDesc::Usage usage) {
    switch (usage) {
    case BufferDesc::Usage::vertex: return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    case BufferDesc::Usage::index: return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    case BufferDesc::Usage::uniform: return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    case BufferDesc::Usage::storage:
        return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    case BufferDesc::Usage::accelerationStructure:
        return VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
             | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    return 0;
}

} // namespace

VulkanDevice::VulkanDevice(platform::Window& window, bool validation)
    : window_(window), validation_(validation) {
    createInstance(validation);
    createSurface();
    selectPhysicalDevice();
    queryCapabilities();
    createLogicalDevice();
    createSwapchain();
    createPipeline();
    createFrames();
    createUi();
    const std::array<PreviewVertex, 3> fallbackVertices {{ {}, {}, {} }};
    const std::array<std::uint32_t, 3> fallbackIndices { 0, 1, 2 };
    uploadPreviewMesh(fallbackVertices, fallbackIndices);
    log::info("Vulkan device ready: ", capabilities_.gpuName, " (", capabilities_.driverName, ")");
}

VulkanDevice::~VulkanDevice() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
    destroyUi();
    destroyPreviewMesh();
    for (const auto& [handle, resource] : textures_) {
        static_cast<void>(handle);
        if (resource.image != VK_NULL_HANDLE) vkDestroyImage(device_, resource.image, nullptr);
        if (resource.memory != VK_NULL_HANDLE) vkFreeMemory(device_, resource.memory, nullptr);
    }
    for (const auto& [handle, resource] : buffers_) {
        static_cast<void>(handle);
        if (resource.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, resource.buffer, nullptr);
        if (resource.memory != VK_NULL_HANDLE) vkFreeMemory(device_, resource.memory, nullptr);
    }
    destroyFrames();
    destroyPipeline();
    destroySwapchain();
    if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
    if (surface_ != VK_NULL_HANDLE) SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
    if (debugMessenger_ != VK_NULL_HANDLE) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy != nullptr) destroy(instance_, debugMessenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
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
    if (validation_) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    const VkApplicationInfo appInfo {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "mikumikudesu",
        .applicationVersion = VK_MAKE_API_VERSION(0, 1, 20, 0),
        .pEngineName = "dayo-native",
        .engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo createInfo {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = static_cast<std::uint32_t>(enabledLayers.size()),
        .ppEnabledLayerNames = enabledLayers.data(),
        .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    check(vkCreateInstance(&createInfo, nullptr, &instance_), "create Vulkan instance");

    if (validation_) {
        const VkDebugUtilsMessengerCreateInfoEXT debugInfo {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                             | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debugCallback,
        };
        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create != nullptr) check(create(instance_, &debugInfo, nullptr, &debugMessenger_),
                                     "create Vulkan debug messenger");
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
    if (deviceCount == 0) throw std::runtime_error("no Vulkan physical device found");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    check(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()), "enumerate physical devices");

    int bestScore = -1;
    for (const auto candidate : devices) {
        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3) continue;

        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        for (std::uint32_t i = 0; i < familyCount; ++i) {
            VkBool32 present = VK_FALSE;
            check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &present),
                  "query surface support");
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 || present == VK_FALSE) continue;
            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000
                            : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 500 : 0;
            if (score > bestScore) {
                bestScore = score;
                physicalDevice_ = candidate;
                physicalProperties_ = properties;
                queueFamily_ = i;
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

    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR,
    };
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
        .pNext = &barycentric,
    };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayPipeline {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .pNext = &rayQuery,
    };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &rayPipeline,
    };
    VkPhysicalDeviceVulkan12Features vulkan12 {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &acceleration,
    };
    VkPhysicalDeviceFeatures2 features {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vulkan12,
    };
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &features);

    VkPhysicalDeviceDriverProperties driver {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 properties {
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
    capabilities_.descriptorIndexing = vulkan12.runtimeDescriptorArray == VK_TRUE
                                    && vulkan12.descriptorBindingPartiallyBound == VK_TRUE;
    capabilities_.accelerationStructure = acceleration.accelerationStructure == VK_TRUE
        && hasName(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    capabilities_.rayTracingPipeline = rayPipeline.rayTracingPipeline == VK_TRUE
        && hasName(extensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    capabilities_.rayQuery = rayQuery.rayQuery == VK_TRUE
        && hasName(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    capabilities_.fragmentShaderBarycentric = barycentric.fragmentShaderBarycentric == VK_TRUE
        && hasName(extensions, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);

    if (!capabilities_.swapchain) throw std::runtime_error("selected GPU lacks VK_KHR_swapchain");
}

void VulkanDevice::createLogicalDevice() {
    const float priority = 1.0F;
    const VkDeviceQueueCreateInfo queueInfo {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queueFamily_,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };

    std::vector<const char*> extensions {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    };
    const bool rtBase = capabilities_.accelerationStructure && capabilities_.bufferDeviceAddress;
    if (rtBase) {
        extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    }
    if (capabilities_.rayTracingPipeline) extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    if (capabilities_.rayQuery) extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    if (capabilities_.fragmentShaderBarycentric) {
        extensions.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
    }

    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR,
        .fragmentShaderBarycentric = capabilities_.fragmentShaderBarycentric,
    };
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
        .pNext = &barycentric,
        .rayQuery = capabilities_.rayQuery,
    };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayPipeline {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .pNext = &rayQuery,
        .rayTracingPipeline = capabilities_.rayTracingPipeline,
    };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &rayPipeline,
        .accelerationStructure = capabilities_.accelerationStructure,
    };
    VkPhysicalDeviceVulkan13Features vulkan13 {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &acceleration,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features vulkan12 {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &vulkan13,
        .descriptorIndexing = capabilities_.descriptorIndexing,
        .descriptorBindingPartiallyBound = capabilities_.descriptorIndexing,
        .runtimeDescriptorArray = capabilities_.descriptorIndexing,
        .bufferDeviceAddress = capabilities_.bufferDeviceAddress,
    };
    const VkPhysicalDeviceFeatures2 features {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vulkan12,
    };
    const VkDeviceCreateInfo createInfo {
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
    VkSurfaceCapabilitiesKHR surfaceCapabilities {};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &surfaceCapabilities),
          "query surface capabilities");
    std::uint32_t formatCount = 0;
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr),
          "query surface formats");
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data()),
          "query surface formats");
    if (formats.empty()) throw std::runtime_error("surface exposes no Vulkan formats");

    VkSurfaceFormatKHR selected = formats.front();
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB
            && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            selected = format;
            break;
        }
    }
    swapchainFormat_ = selected.format;
    if (surfaceCapabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        swapchainExtent_ = surfaceCapabilities.currentExtent;
    } else {
        swapchainExtent_.width = std::clamp(window_.pixelWidth(),
            surfaceCapabilities.minImageExtent.width, surfaceCapabilities.maxImageExtent.width);
        swapchainExtent_.height = std::clamp(window_.pixelHeight(),
            surfaceCapabilities.minImageExtent.height, surfaceCapabilities.maxImageExtent.height);
    }

    std::uint32_t imageCount = std::max(surfaceCapabilities.minImageCount, 2U);
    if (surfaceCapabilities.maxImageCount != 0) {
        imageCount = std::min(imageCount, surfaceCapabilities.maxImageCount);
    }
    const VkSwapchainCreateInfoKHR createInfo {
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
    check(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data()),
          "get swapchain images");
    swapchainViews_.resize(imageCount);
    swapchainInitialized_.assign(imageCount, false);
    for (std::size_t i = 0; i < swapchainImages_.size(); ++i) {
        const VkImageViewCreateInfo viewInfo {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = swapchainImages_[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = swapchainFormat_,
            .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        check(vkCreateImageView(device_, &viewInfo, nullptr, &swapchainViews_[i]),
              "create swapchain image view");
    }
}

void VulkanDevice::destroySwapchain() {
    for (const auto view : swapchainViews_) vkDestroyImageView(device_, view, nullptr);
    swapchainViews_.clear();
    swapchainImages_.clear();
    swapchainInitialized_.clear();
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void VulkanDevice::createPipeline() {
    const auto vertexCode = readBinary(DAYO_PREVIEW_VERTEX_SPV);
    const auto fragmentCode = readBinary(DAYO_PREVIEW_FRAGMENT_SPV);
    const VkShaderModuleCreateInfo vertexInfo {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vertexCode.size(),
        .pCode = reinterpret_cast<const std::uint32_t*>(vertexCode.data()),
    };
    const VkShaderModuleCreateInfo fragmentInfo {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fragmentCode.size(),
        .pCode = reinterpret_cast<const std::uint32_t*>(fragmentCode.data()),
    };
    VkShaderModule vertex {};
    VkShaderModule fragment {};
    check(vkCreateShaderModule(device_, &vertexInfo, nullptr, &vertex), "create vertex shader");
    try {
        check(vkCreateShaderModule(device_, &fragmentInfo, nullptr, &fragment), "create fragment shader");
    } catch (...) {
        vkDestroyShaderModule(device_, vertex, nullptr);
        throw;
    }

    const std::array stages {
        VkPipelineShaderStageCreateInfo {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertex,
            .pName = "VS",
        },
        VkPipelineShaderStageCreateInfo {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment,
            .pName = "PS",
        },
    };
    const VkVertexInputBindingDescription vertexBinding {
        .binding = 0,
        .stride = sizeof(PreviewVertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    const std::array vertexAttributes {
        VkVertexInputAttributeDescription { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                            offsetof(PreviewVertex, position) },
        VkVertexInputAttributeDescription { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                            offsetof(PreviewVertex, normal) },
        VkVertexInputAttributeDescription { 2, 0, VK_FORMAT_R32G32_SFLOAT,
                                            offsetof(PreviewVertex, uv) },
    };
    const VkPipelineVertexInputStateCreateInfo vertexInput {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vertexBinding,
        .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(vertexAttributes.size()),
        .pVertexAttributeDescriptions = vertexAttributes.data(),
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkPipelineViewportStateCreateInfo viewport {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const VkPipelineRasterizationStateCreateInfo rasterizer {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0F,
    };
    const VkPipelineMultisampleStateCreateInfo multisample {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState blendAttachment {
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blendAttachment,
    };
    const std::array dynamicStates { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    const VkPipelineDynamicStateCreateInfo dynamic {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data(),
    };
    const VkPipelineLayoutCreateInfo layoutInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };
    check(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_),
          "create pipeline layout");
    const VkPipelineRenderingCreateInfo renderingInfo {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &swapchainFormat_,
    };
    const VkGraphicsPipelineCreateInfo pipelineInfo {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &renderingInfo,
        .stageCount = static_cast<std::uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertexInput,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = pipelineLayout_,
    };
    const auto result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                   nullptr, &pipeline_);
    vkDestroyShaderModule(device_, fragment, nullptr);
    vkDestroyShaderModule(device_, vertex, nullptr);
    check(result, "create graphics pipeline");
}

void VulkanDevice::destroyPipeline() {
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
}

void VulkanDevice::createFrames() {
    for (auto& frame : frames_) {
        const VkCommandPoolCreateInfo poolInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queueFamily_,
        };
        check(vkCreateCommandPool(device_, &poolInfo, nullptr, &frame.commandPool), "create command pool");
        const VkCommandBufferAllocateInfo commandInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = frame.commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        check(vkAllocateCommandBuffers(device_, &commandInfo, &frame.commandBuffer),
              "allocate command buffer");
        const VkSemaphoreCreateInfo semaphoreInfo { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.imageAvailable),
              "create image semaphore");
        check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.renderFinished),
              "create render semaphore");
        const VkFenceCreateInfo fenceInfo {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        check(vkCreateFence(device_, &fenceInfo, nullptr, &frame.inFlight), "create frame fence");
    }
}

void VulkanDevice::destroyFrames() {
    if (device_ == VK_NULL_HANDLE) return;
    for (auto& frame : frames_) {
        if (frame.inFlight != VK_NULL_HANDLE) vkDestroyFence(device_, frame.inFlight, nullptr);
        if (frame.renderFinished != VK_NULL_HANDLE) vkDestroySemaphore(device_, frame.renderFinished, nullptr);
        if (frame.imageAvailable != VK_NULL_HANDLE) vkDestroySemaphore(device_, frame.imageAvailable, nullptr);
        if (frame.commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device_, frame.commandPool, nullptr);
        frame = {};
    }
}

void VulkanDevice::createUi() {
#if DAYO_HAS_IMGUI
    const std::array<VkDescriptorPoolSize, 1> poolSizes {{
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 },
    }};
    const VkDescriptorPoolCreateInfo poolInfo {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1024,
        .poolSizeCount = static_cast<std::uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data(),
    };
    check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &imguiDescriptorPool_),
          "create ImGui descriptor pool");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL3_InitForVulkan(window_.sdlHandle())) {
        throw std::runtime_error("ImGui SDL3 initialization failed");
    }
    ImGui_ImplVulkan_InitInfo info {};
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
    info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapchainFormat_;
    if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("ImGui Vulkan initialization failed");
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

void VulkanDevice::resize() { swapchainDirty_ = true; }

void VulkanDevice::recreateSwapchain() {
    if (window_.pixelWidth() == 0 || window_.pixelHeight() == 0) return;
    check(vkDeviceWaitIdle(device_), "wait before swapchain recreation");
    destroyUi();
    destroyPipeline();
    destroySwapchain();
    createSwapchain();
    createPipeline();
    createUi();
    swapchainDirty_ = false;
}

void VulkanDevice::beginUiFrame() {
    if (swapchainDirty_) recreateSwapchain();
#if DAYO_HAS_IMGUI
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
#endif
}

void VulkanDevice::renderFrame() {
    if (window_.pixelWidth() == 0 || window_.pixelHeight() == 0) return;
#if DAYO_HAS_IMGUI
    ImGui::Render();
#endif
    auto& frame = frames_[frameIndex_];
    check(vkWaitForFences(device_, 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "wait for frame");

    std::uint32_t imageIndex = 0;
    const auto acquire = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) check(acquire, "acquire swapchain image");
    check(vkResetFences(device_, 1, &frame.inFlight), "reset frame fence");
    check(vkResetCommandPool(device_, frame.commandPool, 0), "reset command pool");
    const VkCommandBufferBeginInfo beginInfo { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    check(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "begin command buffer");

    const VkImageMemoryBarrier2 toColor {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = swapchainInitialized_[imageIndex]
            ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = swapchainInitialized_[imageIndex]
            ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = swapchainImages_[imageIndex],
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    const VkDependencyInfo toColorDependency {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toColor,
    };
    vkCmdPipelineBarrier2(frame.commandBuffer, &toColorDependency);

    VkClearValue clear {};
    clear.color = activeRenderer_ == RendererKind::preview
        ? VkClearColorValue {{ 0.025F, 0.035F, 0.055F, 1.0F }}
        : VkClearColorValue {{ 0.055F, 0.025F, 0.045F, 1.0F }};
    const VkRenderingAttachmentInfo attachment {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = swapchainViews_[imageIndex],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = clear,
    };
    const VkRenderingInfo renderingInfo {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, swapchainExtent_ },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    };
    vkCmdBeginRendering(frame.commandBuffer, &renderingInfo);
    const VkViewport viewport { 0.0F, 0.0F, static_cast<float>(swapchainExtent_.width),
                                static_cast<float>(swapchainExtent_.height), 0.0F, 1.0F };
    const VkRect2D scissor { { 0, 0 }, swapchainExtent_ };
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(frame.commandBuffer, 0, 1, &previewVertexBuffer_, &vertexOffset);
    vkCmdBindIndexBuffer(frame.commandBuffer, previewIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(frame.commandBuffer, previewIndexCount_, 1, 0, 0, 0);
#if DAYO_HAS_IMGUI
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.commandBuffer);
#endif
    vkCmdEndRendering(frame.commandBuffer);

    const VkImageMemoryBarrier2 toPresent {
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
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    const VkDependencyInfo toPresentDependency {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &toPresent,
    };
    vkCmdPipelineBarrier2(frame.commandBuffer, &toPresentDependency);
    check(vkEndCommandBuffer(frame.commandBuffer), "end command buffer");

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSubmitInfo submitInfo {
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
    const VkPresentInfoKHR presentInfo {
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

void VulkanDevice::waitIdle() {
    if (device_ != VK_NULL_HANDLE) check(vkDeviceWaitIdle(device_), "wait for Vulkan device");
}

void VulkanDevice::destroyPreviewMesh() {
    if (previewIndexBuffer_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, previewIndexBuffer_, nullptr);
    if (previewIndexMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_, previewIndexMemory_, nullptr);
    if (previewVertexBuffer_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, previewVertexBuffer_, nullptr);
    if (previewVertexMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_, previewVertexMemory_, nullptr);
    previewVertexBuffer_ = VK_NULL_HANDLE;
    previewVertexMemory_ = VK_NULL_HANDLE;
    previewVertexSize_ = 0;
    previewIndexBuffer_ = VK_NULL_HANDLE;
    previewIndexMemory_ = VK_NULL_HANDLE;
    previewIndexCount_ = 0;
}

void VulkanDevice::uploadPreviewMesh(std::span<const PreviewVertex> vertices,
                                     std::span<const std::uint32_t> indices) {
    if (vertices.empty() || indices.empty()) throw std::invalid_argument("preview mesh is empty");
    waitIdle();
    destroyPreviewMesh();

    auto upload = [this](const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
                         VkBuffer& buffer, VkDeviceMemory& memory) {
        const VkBufferCreateInfo bufferInfo {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        check(vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer), "create preview mesh buffer");
        VkMemoryRequirements requirements {};
        vkGetBufferMemoryRequirements(device_, buffer, &requirements);
        const VkMemoryAllocateInfo allocationInfo {
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
    };

    try {
        upload(vertices.data(), vertices.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
               previewVertexBuffer_, previewVertexMemory_);
        previewVertexSize_ = vertices.size_bytes();
        upload(indices.data(), indices.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
               previewIndexBuffer_, previewIndexMemory_);
        previewIndexCount_ = static_cast<std::uint32_t>(indices.size());
    } catch (...) {
        destroyPreviewMesh();
        throw;
    }
    log::info("Uploaded PMX preview mesh: ", vertices.size(), " vertices, ",
              indices.size() / 3, " triangles");
}

void VulkanDevice::updatePreviewVertices(std::span<const PreviewVertex> vertices) {
    if (vertices.size_bytes() != previewVertexSize_ || previewVertexMemory_ == VK_NULL_HANDLE) {
        throw std::invalid_argument("preview vertex update has a different size");
    }
    // The preview path uses coherent host memory. Waiting here is conservative and
    // will be replaced by per-frame staging when the GPU skinning path is selected.
    waitIdle();
    void* mapped = nullptr;
    check(vkMapMemory(device_, previewVertexMemory_, 0, previewVertexSize_, 0, &mapped),
          "map animated preview vertices");
    std::memcpy(mapped, vertices.data(), vertices.size_bytes());
    vkUnmapMemory(device_, previewVertexMemory_);
}

std::uint32_t VulkanDevice::findMemoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties properties {};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &properties);
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1U << i)) != 0 && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    throw std::runtime_error("no compatible Vulkan memory type");
}

BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc) {
    if (desc.size == 0) throw std::invalid_argument("buffer size must be non-zero");
    BufferResource resource;
    const VkBufferCreateInfo createInfo {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = desc.size,
        .usage = toVkUsage(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    check(vkCreateBuffer(device_, &createInfo, nullptr, &resource.buffer), "create buffer");
    VkMemoryRequirements requirements {};
    vkGetBufferMemoryRequirements(device_, resource.buffer, &requirements);
    const VkMemoryPropertyFlags flags = desc.cpuVisible
        ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryAllocateFlagsInfo allocationFlags {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = capabilities_.bufferDeviceAddress
            ? static_cast<VkMemoryAllocateFlags>(VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT)
            : VkMemoryAllocateFlags {},
    };
    const VkMemoryAllocateInfo allocationInfo {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = capabilities_.bufferDeviceAddress ? &allocationFlags : nullptr,
        .allocationSize = requirements.size,
        .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, flags),
    };
    try {
        check(vkAllocateMemory(device_, &allocationInfo, nullptr, &resource.memory), "allocate buffer memory");
        check(vkBindBufferMemory(device_, resource.buffer, resource.memory, 0), "bind buffer memory");
    } catch (...) {
        if (resource.memory != VK_NULL_HANDLE) vkFreeMemory(device_, resource.memory, nullptr);
        vkDestroyBuffer(device_, resource.buffer, nullptr);
        throw;
    }
    const auto handle = nextResourceHandle_++;
    buffers_.emplace(handle, resource);
    return handle;
}

TextureHandle VulkanDevice::createTexture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0) throw std::invalid_argument("texture size must be non-zero");
    TextureResource resource;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (desc.storage) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (desc.renderTarget) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc.format == TextureDesc::Format::depth32Float) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    const VkImageCreateInfo createInfo {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = toVkFormat(desc.format),
        .extent = { desc.width, desc.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    check(vkCreateImage(device_, &createInfo, nullptr, &resource.image), "create texture");
    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(device_, resource.image, &requirements);
    const VkMemoryAllocateInfo allocationInfo {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    try {
        check(vkAllocateMemory(device_, &allocationInfo, nullptr, &resource.memory), "allocate texture memory");
        check(vkBindImageMemory(device_, resource.image, resource.memory, 0), "bind texture memory");
    } catch (...) {
        if (resource.memory != VK_NULL_HANDLE) vkFreeMemory(device_, resource.memory, nullptr);
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
