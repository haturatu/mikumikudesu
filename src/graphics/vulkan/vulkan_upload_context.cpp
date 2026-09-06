#include "graphics/vulkan/vulkan_upload_context.hpp"

#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace dayo::graphics {
namespace {

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
}

std::uint32_t findMemoryType(VkPhysicalDevice physicalDevice, std::uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((bits & (1U << index)) != 0 && (properties.memoryTypes[index].propertyFlags & flags) == flags)
            return index;
    }
    throw std::runtime_error("no compatible Vulkan upload memory type");
}

} // namespace

VulkanUploadContext::VulkanUploadContext(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue,
                                         std::uint32_t queueFamily, VkSemaphore timeline,
                                         std::uint64_t& nextTimelineValue, VkDeviceSize capacity)
    : device_(device), physicalDevice_(physicalDevice), queue_(queue), timeline_(timeline),
      nextTimelineValue_(&nextTimelineValue), ring_(capacity) {
    if (capacity == 0 || capacity > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("Vulkan upload capacity is invalid");

    const VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = capacity,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    try {
        check(vkCreateBuffer(device_, &bufferInfo, nullptr, &stagingBuffer_), "create persistent upload buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, stagingBuffer_, &requirements);
        const VkMemoryAllocateInfo allocationInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex =
                findMemoryType(physicalDevice, requirements.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
        };
        check(vkAllocateMemory(device_, &allocationInfo, nullptr, &stagingMemory_),
              "allocate persistent upload memory");
        check(vkBindBufferMemory(device_, stagingBuffer_, stagingMemory_, 0), "bind persistent upload memory");
        check(vkMapMemory(device_, stagingMemory_, 0, capacity, 0, &mapped_), "map persistent upload memory");

        const VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queueFamily,
        };
        check(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "create persistent upload pool");
        const VkCommandBufferAllocateInfo commandInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = commandPool_,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = static_cast<std::uint32_t>(commandBuffers_.size()),
        };
        check(vkAllocateCommandBuffers(device_, &commandInfo, commandBuffers_.data()),
              "allocate persistent upload commands");
    } catch (...) {
        if (commandPool_ != VK_NULL_HANDLE)
            vkDestroyCommandPool(device_, commandPool_, nullptr);
        if (mapped_ != nullptr)
            vkUnmapMemory(device_, stagingMemory_);
        if (stagingMemory_ != VK_NULL_HANDLE)
            vkFreeMemory(device_, stagingMemory_, nullptr);
        if (stagingBuffer_ != VK_NULL_HANDLE)
            vkDestroyBuffer(device_, stagingBuffer_, nullptr);
        throw;
    }
}

VulkanUploadContext::~VulkanUploadContext() {
    abort();
    for (auto& upload : pendingDedicated_)
        destroyDedicated(upload);
    for (auto& upload : submittedDedicated_)
        destroyDedicated(upload);
    if (mapped_ != nullptr)
        vkUnmapMemory(device_, stagingMemory_);
    if (commandPool_ != VK_NULL_HANDLE)
        vkDestroyCommandPool(device_, commandPool_, nullptr);
    if (stagingMemory_ != VK_NULL_HANDLE)
        vkFreeMemory(device_, stagingMemory_, nullptr);
    if (stagingBuffer_ != VK_NULL_HANDLE)
        vkDestroyBuffer(device_, stagingBuffer_, nullptr);
}

void VulkanUploadContext::begin() {
    if (pendingSignalValue_ != 0)
        throw std::logic_error("Vulkan upload command is already recording");
    reclaim();
    batchIndex_ = (batchIndex_ + 1U) % commandBuffers_.size();
    if (submittedValues_[batchIndex_] != 0) {
        std::uint64_t completedValue = 0;
        check(vkGetSemaphoreCounterValue(device_, timeline_, &completedValue), "query upload batch timeline");
        if (completedValue < submittedValues_[batchIndex_])
            wait(submittedValues_[batchIndex_]);
    }
    check(vkResetCommandBuffer(commandBuffers_[batchIndex_], 0), "reset persistent upload command");
    const VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    check(vkBeginCommandBuffer(commandBuffers_[batchIndex_], &beginInfo), "begin persistent upload command");
    pendingSignalValue_ = ++*nextTimelineValue_;
}

VulkanUploadContext::Slice VulkanUploadContext::allocate(VkDeviceSize size, VkDeviceSize alignment) {
    if (pendingSignalValue_ == 0)
        throw std::logic_error("Vulkan upload allocation requires begin");
    if (size == 0 || size > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("Vulkan upload allocation is invalid");
    if (size > ring_.capacity())
        return allocateDedicated(size);

    auto allocation =
        ring_.tryAllocate(static_cast<std::size_t>(size), pendingSignalValue_, static_cast<std::size_t>(alignment));
    while (!allocation) {
        const auto oldest = ring_.oldestRetireValue();
        // Allocations from this recording batch have no timeline signal yet;
        // waiting on them would deadlock. Use dedicated staging in that case.
        if (!oldest || *oldest >= pendingSignalValue_)
            break;
        wait(*oldest);
        allocation =
            ring_.tryAllocate(static_cast<std::size_t>(size), pendingSignalValue_, static_cast<std::size_t>(alignment));
    }
    if (!allocation)
        return allocateDedicated(size);
    return {stagingBuffer_, allocation->offset, static_cast<std::byte*>(mapped_) + allocation->offset,
            pendingSignalValue_};
}

VulkanUploadContext::Slice VulkanUploadContext::allocateDedicated(VkDeviceSize size) {
    DedicatedUpload upload;
    try {
        const VkBufferCreateInfo bufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        check(vkCreateBuffer(device_, &bufferInfo, nullptr, &upload.buffer), "create dedicated upload buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, upload.buffer, &requirements);
        const VkMemoryAllocateInfo allocationInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex =
                findMemoryType(physicalDevice_, requirements.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
        };
        check(vkAllocateMemory(device_, &allocationInfo, nullptr, &upload.memory), "allocate dedicated upload memory");
        check(vkBindBufferMemory(device_, upload.buffer, upload.memory, 0), "bind dedicated upload memory");
        check(vkMapMemory(device_, upload.memory, 0, size, 0, &upload.mapped), "map dedicated upload memory");
        upload.retireValue = pendingSignalValue_;
        pendingDedicated_.push_back(upload);
        return {upload.buffer, 0, upload.mapped, pendingSignalValue_};
    } catch (...) {
        destroyDedicated(upload);
        throw;
    }
}

std::uint64_t VulkanUploadContext::submit() {
    if (pendingSignalValue_ == 0)
        throw std::logic_error("Vulkan upload command is not recording");
    // Reserve before submitting. Allocation failure must leave the batch
    // abortable; once vkQueueSubmit succeeds its dedicated buffers belong to
    // the GPU until their timeline value retires.
    submittedDedicated_.reserve(submittedDedicated_.size() + pendingDedicated_.size());
    check(vkEndCommandBuffer(commandBuffers_[batchIndex_]), "end persistent upload command");
    const VkTimelineSemaphoreSubmitInfo timelineSubmit{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &pendingSignalValue_,
    };
    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timelineSubmit,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffers_[batchIndex_],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &timeline_,
    };
    const auto signalValue = pendingSignalValue_;
    check(vkQueueSubmit(queue_, 1, &submitInfo, VK_NULL_HANDLE), "submit persistent upload command");
    submittedValues_[batchIndex_] = signalValue;
    lastSubmittedValue_ = signalValue;
    submittedDedicated_.insert(submittedDedicated_.end(), std::make_move_iterator(pendingDedicated_.begin()),
                               std::make_move_iterator(pendingDedicated_.end()));
    pendingDedicated_.clear();
    pendingSignalValue_ = 0;
    return signalValue;
}

void VulkanUploadContext::abort() noexcept {
    if (pendingSignalValue_ == 0)
        return;
    ring_.rollback(pendingSignalValue_);
    for (auto& upload : pendingDedicated_)
        destroyDedicated(upload);
    pendingDedicated_.clear();
    static_cast<void>(vkResetCommandBuffer(commandBuffers_[batchIndex_], 0));
    pendingSignalValue_ = 0;
}

void VulkanUploadContext::destroyDedicated(DedicatedUpload& upload) noexcept {
    if (upload.mapped != nullptr)
        vkUnmapMemory(device_, upload.memory);
    if (upload.memory != VK_NULL_HANDLE)
        vkFreeMemory(device_, upload.memory, nullptr);
    if (upload.buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device_, upload.buffer, nullptr);
    upload = {};
}

void VulkanUploadContext::reclaimDedicated(std::uint64_t completedValue) noexcept {
    auto current = submittedDedicated_.begin();
    while (current != submittedDedicated_.end()) {
        if (current->retireValue <= completedValue) {
            destroyDedicated(*current);
            current = submittedDedicated_.erase(current);
        } else {
            ++current;
        }
    }
}

void VulkanUploadContext::wait(std::uint64_t value) {
    const VkSemaphoreWaitInfo waitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &timeline_,
        .pValues = &value,
    };
    check(vkWaitSemaphores(device_, &waitInfo, UINT64_MAX), "wait for persistent upload command");
    ring_.reclaim(value);
    reclaimDedicated(value);
}

void VulkanUploadContext::reclaim() {
    std::uint64_t completedValue = 0;
    check(vkGetSemaphoreCounterValue(device_, timeline_, &completedValue), "query persistent upload timeline");
    ring_.reclaim(completedValue);
    reclaimDedicated(completedValue);
}

} // namespace dayo::graphics
