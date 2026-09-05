#pragma once

#include <vulkan/vulkan.h>

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
