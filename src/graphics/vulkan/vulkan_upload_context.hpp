#pragma once

#include "core/upload_ring.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace dayo::graphics {

class VulkanUploadContext final {
  public:
    struct Slice {
        VkBuffer buffer{};
        VkDeviceSize offset{};
        void* mapped{};
        std::uint64_t retireValue{};
    };

    VulkanUploadContext(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue, std::uint32_t queueFamily,
                        VkSemaphore timeline, std::uint64_t& nextTimelineValue,
                        VkDeviceSize capacity = 64ULL * 1024ULL * 1024ULL);
    ~VulkanUploadContext();

    VulkanUploadContext(const VulkanUploadContext&) = delete;
    VulkanUploadContext& operator=(const VulkanUploadContext&) = delete;

    void begin();
    [[nodiscard]] Slice allocate(VkDeviceSize size, VkDeviceSize alignment = 16);
    [[nodiscard]] VkCommandBuffer commandBuffer() const noexcept {
        return commandBuffers_[batchIndex_];
    }
    [[nodiscard]] std::uint64_t lastSubmittedValue() const noexcept {
        return lastSubmittedValue_;
    }
    [[nodiscard]] std::uint64_t submit();
    void wait(std::uint64_t value);
    void reclaim();

  private:
    VkDevice device_{};
    VkQueue queue_{};
    VkSemaphore timeline_{};
    std::uint64_t* nextTimelineValue_{};
    VkBuffer stagingBuffer_{};
    VkDeviceMemory stagingMemory_{};
    void* mapped_{};
    VkCommandPool commandPool_{};
    static constexpr std::size_t batchCount = 2;
    std::array<VkCommandBuffer, batchCount> commandBuffers_{};
    std::array<std::uint64_t, batchCount> submittedValues_{};
    std::size_t batchIndex_{batchCount - 1};
    std::uint64_t lastSubmittedValue_{};
    core::UploadRing ring_;
    std::uint64_t pendingSignalValue_{};
};

} // namespace dayo::graphics
