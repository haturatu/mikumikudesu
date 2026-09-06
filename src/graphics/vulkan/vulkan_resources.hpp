#pragma once

#include <vulkan/vulkan.h>

#if DAYO_ENABLE_VMA
VK_DEFINE_HANDLE(VmaAllocation)
#endif

namespace dayo::graphics {

struct VulkanBuffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
#if DAYO_ENABLE_VMA
    VmaAllocation allocation{};
#endif
    VkDeviceSize size{};
};

struct VulkanImage {
    VkImage image{};
    VkDeviceMemory memory{};
#if DAYO_ENABLE_VMA
    VmaAllocation allocation{};
#endif
};

} // namespace dayo::graphics
