#include "graphics/vulkan/vulkan_device.hpp"

#include "graphics/vulkan/vulkan_upload_context.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace dayo::graphics {
namespace {

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
}

VkDeviceAddress accelerationAddress(VkDevice device, VkAccelerationStructureKHR structure) {
    const auto getAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR"));
    if (getAddress == nullptr)
        throw std::runtime_error("vkGetAccelerationStructureDeviceAddressKHR is unavailable");
    const VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure = structure,
    };
    const auto address = getAddress(device, &addressInfo);
    if (address == 0)
        throw std::runtime_error("Vulkan returned a null acceleration-structure device address");
    return address;
}

VkDeviceSize checkedMultiply(VkDeviceSize left, VkDeviceSize right, const char* message) {
    if (right != 0 && left > std::numeric_limits<VkDeviceSize>::max() / right)
        throw std::overflow_error(message);
    return left * right;
}

VkDeviceSize checkedAdd(VkDeviceSize left, VkDeviceSize right, const char* message) {
    if (left > std::numeric_limits<VkDeviceSize>::max() - right)
        throw std::overflow_error(message);
    return left + right;
}

VkGeometryInstanceFlagsKHR instanceFlags(std::uint32_t flags) noexcept {
    VkGeometryInstanceFlagsKHR result = 0;
    if ((flags & 1U) != 0U)
        result |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    if ((flags & 2U) != 0U)
        result |= VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
    if ((flags & 4U) != 0U)
        result |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
    return result;
}

} // namespace

VkBuffer VulkanDevice::createAccelerationBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory& memory,
                                                bool hostVisible, void** mapped) {
    if (size == 0)
        throw std::invalid_argument("acceleration buffer size must be non-zero");
    VkBuffer buffer = VK_NULL_HANDLE;
    const VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    check(vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer), "create acceleration buffer");
    try {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, buffer, &requirements);
        VkMemoryAllocateFlagsInfo flags{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
        };
        const VkMemoryPropertyFlags properties =
            hostVisible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const VkMemoryAllocateInfo allocationInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &flags,
            .allocationSize = requirements.size,
            .memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, properties),
        };
        check(vkAllocateMemory(device_, &allocationInfo, nullptr, &memory), "allocate acceleration buffer memory");
        check(vkBindBufferMemory(device_, buffer, memory, 0), "bind acceleration buffer memory");
        if (hostVisible)
            check(vkMapMemory(device_, memory, 0, size, 0, mapped), "map acceleration instance buffer");
    } catch (...) {
        if (mapped != nullptr && *mapped != nullptr)
            vkUnmapMemory(device_, memory);
        if (memory != VK_NULL_HANDLE)
            vkFreeMemory(device_, memory, nullptr);
        vkDestroyBuffer(device_, buffer, nullptr);
        memory = VK_NULL_HANDLE;
        throw;
    }
    return buffer;
}

void VulkanDevice::destroyAccelerationBuffer(VkBuffer buffer, VkDeviceMemory memory, void* mapped) noexcept {
    if (mapped != nullptr)
        vkUnmapMemory(device_, memory);
    if (memory != VK_NULL_HANDLE)
        vkFreeMemory(device_, memory, nullptr);
    if (buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device_, buffer, nullptr);
}

VkBuffer VulkanDevice::allocateRecordedAccelerationScratch(VkDeviceSize size) {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* unusedMapped = nullptr;
    const auto buffer =
        createAccelerationBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory, false, &unusedMapped);
    try {
        pendingAccelerationScratch_[frameIndex_].push_back({.buffer = buffer, .memory = memory});
    } catch (...) {
        destroyAccelerationBuffer(buffer, memory, unusedMapped);
        throw;
    }
    return buffer;
}

void VulkanDevice::reclaimAccelerationScratch(std::size_t frameIndex) noexcept {
    if (frameIndex >= pendingAccelerationScratch_.size())
        return;
    for (const auto scratch : pendingAccelerationScratch_[frameIndex])
        destroyAccelerationBuffer(scratch.buffer, scratch.memory, nullptr);
    pendingAccelerationScratch_[frameIndex].clear();
}

void VulkanDevice::reclaimAllAccelerationScratch() noexcept {
    for (std::size_t index = 0; index < pendingAccelerationScratch_.size(); ++index)
        reclaimAccelerationScratch(index);
}

VulkanDevice::BlasBuildInput VulkanDevice::makeBlasBuildInput(const BlasGeometryDesc& geometry) const {
    if (geometry.triangles.empty())
        throw std::invalid_argument("BLAS geometry must contain at least one triangle description");
    BlasBuildInput input;
    input.geometries.reserve(geometry.triangles.size());
    input.ranges.reserve(geometry.triangles.size());
    input.primitiveCounts.reserve(geometry.triangles.size());
    input.allowUpdate = true;
    for (const auto& triangle : geometry.triangles) {
        const auto vertexIt = typedBuffers_.find(triangle.vertexBuffer);
        if (vertexIt == typedBuffers_.end() || !typedBufferHandles_.isAlive(triangle.vertexBuffer))
            throw std::invalid_argument("BLAS references a stale vertex buffer");
        if (triangle.vertexFormat != VertexFormat::r32g32b32Sfloat || triangle.vertexCount < 3)
            throw std::invalid_argument("BLAS vertex geometry is invalid");
        const auto stride =
            triangle.vertexStride == 0 ? static_cast<std::uint32_t>(sizeof(float) * 3U) : triangle.vertexStride;
        if (stride < sizeof(float) * 3U)
            throw std::invalid_argument("BLAS vertex stride is too small");
        const auto vertexBytes = checkedAdd(
            checkedMultiply(static_cast<VkDeviceSize>(triangle.vertexCount - 1U), stride, "BLAS vertex range overflow"),
            sizeof(float) * 3U, "BLAS vertex range overflow");
        if (triangle.vertexOffset > vertexIt->second.resource.size ||
            vertexBytes > vertexIt->second.resource.size - triangle.vertexOffset)
            throw std::out_of_range("BLAS vertex range exceeds its buffer");

        VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress =
            bufferDeviceAddress(vertexIt->second.resource.buffer) + triangle.vertexOffset;
        triangles.vertexStride = stride;
        triangles.maxVertex = triangle.vertexCount - 1U;
        std::uint32_t primitiveCount = triangle.vertexCount / 3U;
        if (triangle.indexType != IndexType::none) {
            const auto indexIt = typedBuffers_.find(triangle.indexBuffer);
            if (indexIt == typedBuffers_.end() || !typedBufferHandles_.isAlive(triangle.indexBuffer))
                throw std::invalid_argument("BLAS references a stale index buffer");
            const auto indexSize =
                triangle.indexType == IndexType::uint16 ? sizeof(std::uint16_t) : sizeof(std::uint32_t);
            if (triangle.indexCount < 3 || triangle.indexCount % 3U != 0)
                throw std::invalid_argument("BLAS index count must contain complete triangles");
            const auto indexBytes = checkedMultiply(triangle.indexCount, indexSize, "BLAS index range overflow");
            if (triangle.indexOffset > indexIt->second.resource.size ||
                indexBytes > indexIt->second.resource.size - triangle.indexOffset)
                throw std::out_of_range("BLAS index range exceeds its buffer");
            triangles.indexType = triangle.indexType == IndexType::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            triangles.indexData.deviceAddress =
                bufferDeviceAddress(indexIt->second.resource.buffer) + triangle.indexOffset;
            primitiveCount = triangle.indexCount / 3U;
        } else if (triangle.indexCount != 0 || triangle.indexBuffer.valid()) {
            throw std::invalid_argument("non-indexed BLAS geometry cannot specify an index buffer");
        }
        VkAccelerationStructureGeometryKHR value{};
        value.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        value.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        value.flags = triangle.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
        value.geometry.triangles = triangles;
        input.geometries.push_back(value);
        input.primitiveCounts.push_back(primitiveCount);
        input.ranges.push_back({primitiveCount, 0, 0, 0});
        input.allowUpdate = input.allowUpdate && triangle.allowUpdate;
    }
    return input;
}

void VulkanDevice::recordAccelerationBuild(const TypedAccelerationStructure& destination,
                                           const BlasGeometryDesc& geometry, bool update) {
    const auto input = makeBlasBuildInput(geometry);
    if (update && !destination.allowUpdate)
        throw std::logic_error("BLAS was not created with update support");
    const auto getSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureBuildSizesKHR"));
    const auto build = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(device_, "vkCmdBuildAccelerationStructuresKHR"));
    if (getSizes == nullptr || build == nullptr)
        throw std::runtime_error("Vulkan acceleration-structure build functions are unavailable");
    VkAccelerationStructureBuildGeometryInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    sizeInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    sizeInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                     (input.allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
    sizeInfo.geometryCount = static_cast<std::uint32_t>(input.geometries.size());
    sizeInfo.pGeometries = input.geometries.data();
    VkAccelerationStructureBuildSizesInfoKHR sizes{.sType =
                                                       VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getSizes(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &sizeInfo, input.primitiveCounts.data(), &sizes);
    const auto scratchSize = update ? sizes.updateScratchSize : sizes.buildScratchSize;
    VkDeviceMemory scratchMemory = VK_NULL_HANDLE;
    void* unusedMapped = nullptr;
    const auto scratch =
        createAccelerationBuffer(scratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, scratchMemory, false, &unusedMapped);
    try {
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags = sizeInfo.flags;
        buildInfo.mode =
            update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.srcAccelerationStructure = update ? destination.structure : VK_NULL_HANDLE;
        buildInfo.dstAccelerationStructure = destination.structure;
        buildInfo.scratchData.deviceAddress = bufferDeviceAddress(scratch);
        buildInfo.geometryCount = static_cast<std::uint32_t>(input.geometries.size());
        buildInfo.pGeometries = input.geometries.data();
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = input.ranges.data();
        uploadContext_->begin();
        build(uploadContext_->commandBuffer(), 1, &buildInfo, &ranges);
        const auto signal = uploadContext_->submit();
        uploadContext_->wait(signal);
    } catch (...) {
        uploadContext_->abort();
        destroyAccelerationBuffer(scratch, scratchMemory, unusedMapped);
        throw;
    }
    destroyAccelerationBuffer(scratch, scratchMemory, unusedMapped);
}

void VulkanDevice::recordAccelerationBuildOnCommand(VkCommandBuffer commandBuffer,
                                                    const TypedAccelerationStructure& destination,
                                                    const BlasGeometryDesc& geometry, bool update) {
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("BLAS command recording requires a command buffer");
    const auto input = makeBlasBuildInput(geometry);
    if (update && !destination.allowUpdate)
        throw std::logic_error("BLAS was not created with update support");
    const auto getSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureBuildSizesKHR"));
    const auto build = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(device_, "vkCmdBuildAccelerationStructuresKHR"));
    if (getSizes == nullptr || build == nullptr)
        throw std::runtime_error("Vulkan acceleration-structure build functions are unavailable");
    VkAccelerationStructureBuildGeometryInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    sizeInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    sizeInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                     (input.allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
    sizeInfo.geometryCount = static_cast<std::uint32_t>(input.geometries.size());
    sizeInfo.pGeometries = input.geometries.data();
    VkAccelerationStructureBuildSizesInfoKHR sizes{.sType =
                                                       VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getSizes(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &sizeInfo, input.primitiveCounts.data(), &sizes);
    const auto scratchSize = update ? sizes.updateScratchSize : sizes.buildScratchSize;
    const auto scratch = allocateRecordedAccelerationScratch(scratchSize);
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = sizeInfo.flags;
    buildInfo.mode =
        update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.srcAccelerationStructure = update ? destination.structure : VK_NULL_HANDLE;
    buildInfo.dstAccelerationStructure = destination.structure;
    buildInfo.scratchData.deviceAddress = bufferDeviceAddress(scratch);
    buildInfo.geometryCount = static_cast<std::uint32_t>(input.geometries.size());
    buildInfo.pGeometries = input.geometries.data();
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = input.ranges.data();
    build(commandBuffer, 1, &buildInfo, &ranges);
}

handles::AccelerationStructureHandle VulkanDevice::createBlasEx(const BlasGeometryDesc& geometry) {
    if (!capabilities_.accelerationStructure || !capabilities_.bufferDeviceAddress)
        throw std::runtime_error("BLAS requires Vulkan acceleration structures and buffer device address");
    const auto input = makeBlasBuildInput(geometry);
    const auto getSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureBuildSizesKHR"));
    const auto create = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device_, "vkCreateAccelerationStructureKHR"));
    const auto destroy = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device_, "vkDestroyAccelerationStructureKHR"));
    if (getSizes == nullptr || create == nullptr || destroy == nullptr)
        throw std::runtime_error("Vulkan acceleration-structure functions are unavailable");
    VkAccelerationStructureBuildGeometryInfoKHR sizeInfo{};
    sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    sizeInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    sizeInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                     (input.allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0);
    sizeInfo.geometryCount = static_cast<std::uint32_t>(input.geometries.size());
    sizeInfo.pGeometries = input.geometries.data();
    VkAccelerationStructureBuildSizesInfoKHR sizes{.sType =
                                                       VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getSizes(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &sizeInfo, input.primitiveCounts.data(), &sizes);

    TypedAccelerationStructure destination{
        .storageSize = sizes.accelerationStructureSize, .topLevel = false, .allowUpdate = input.allowUpdate};
    destination.storageBuffer =
        createAccelerationBuffer(destination.storageSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                                 destination.storageMemory, false, nullptr);
    try {
        const VkAccelerationStructureCreateInfoKHR createInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = destination.storageBuffer,
            .size = destination.storageSize,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        };
        check(create(device_, &createInfo, nullptr, &destination.structure), "create BLAS");
        recordAccelerationBuild(destination, geometry, false);
    } catch (...) {
        if (destination.structure != VK_NULL_HANDLE)
            destroy(device_, destination.structure, nullptr);
        destroyAccelerationBuffer(destination.storageBuffer, destination.storageMemory, nullptr);
        throw;
    }
    const auto handle = typedAccelerationStructureHandles_.create();
    typedAccelerationStructures_.emplace(handle, destination);
    return handle;
}

handles::AccelerationStructureHandle VulkanDevice::rebuildBlasEx(handles::AccelerationStructureHandle blas,
                                                                 const BlasGeometryDesc& geometry) {
    const auto it = typedAccelerationStructures_.find(blas);
    if (it == typedAccelerationStructures_.end() || !typedAccelerationStructureHandles_.isAlive(blas) ||
        it->second.topLevel)
        throw std::invalid_argument("rebuild BLAS references a stale handle");
    // Return a replacement so callers can retire the old AS only after the
    // new build has completed and no command buffer refers to it.
    return createBlasEx(geometry);
}

void VulkanDevice::refitBlasEx(handles::AccelerationStructureHandle blas, const BlasGeometryDesc& geometry) {
    const auto it = typedAccelerationStructures_.find(blas);
    if (it == typedAccelerationStructures_.end() || !typedAccelerationStructureHandles_.isAlive(blas) ||
        it->second.topLevel)
        throw std::invalid_argument("refit BLAS references a stale handle");
    recordAccelerationBuild(it->second, geometry, true);
}

std::vector<VkAccelerationStructureInstanceKHR>
VulkanDevice::makeTlasInstances(std::span<const AccelerationInstanceDesc> instances) const {
    if (instances.empty())
        throw std::invalid_argument("TLAS requires at least one instance");
    std::vector<VkAccelerationStructureInstanceKHR> result;
    result.reserve(instances.size());
    for (const auto& instance : instances) {
        if (instance.instanceId > 0x00FFFFFFU || instance.sbtRecordOffset > 0x00FFFFFFU)
            throw std::out_of_range("TLAS instance metadata exceeds Vulkan's 24-bit field");
        const auto blasIt = typedAccelerationStructures_.find(instance.blas);
        if (blasIt == typedAccelerationStructures_.end() ||
            !typedAccelerationStructureHandles_.isAlive(instance.blas) || blasIt->second.topLevel)
            throw std::invalid_argument("TLAS references a stale or top-level acceleration structure");
        VkTransformMatrixKHR transform{};
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 4; ++column)
                transform.matrix[row][column] = instance.transform[row * 4U + column];
        }
        const auto customIndex = instance.instanceId & 0x00FFFFFFU;
        const auto mask = instance.mask & 0x000000FFU;
        const auto sbtRecordOffset = instance.sbtRecordOffset & 0x00FFFFFFU;
        const auto flags = static_cast<std::uint32_t>(instanceFlags(instance.flags));
        VkAccelerationStructureInstanceKHR value{};
        value.transform = transform;
        const std::uint32_t customIndexAndMask = customIndex | (mask << 24U);
        const std::uint32_t sbtRecordOffsetAndFlags = sbtRecordOffset | ((flags & 0xFFU) << 24U);
        static_assert(offsetof(VkAccelerationStructureInstanceKHR, accelerationStructureReference) ==
                      sizeof(VkTransformMatrixKHR) + sizeof(std::uint32_t) * 2U);
        auto* encoded = reinterpret_cast<std::uint8_t*>(&value) + sizeof(VkTransformMatrixKHR);
        std::memcpy(encoded, &customIndexAndMask, sizeof(customIndexAndMask));
        std::memcpy(encoded + sizeof(customIndexAndMask), &sbtRecordOffsetAndFlags, sizeof(sbtRecordOffsetAndFlags));
        value.accelerationStructureReference = accelerationAddress(device_, blasIt->second.structure);
        result.push_back(value);
    }
    return result;
}

void VulkanDevice::recordTopLevelBuild(const TypedAccelerationStructure& destination,
                                       std::span<const AccelerationInstanceDesc> instances, bool update) {
    if (!destination.topLevel || destination.instanceBuffer == VK_NULL_HANDLE || destination.mappedInstances == nullptr)
        throw std::logic_error("TLAS has no instance buffer");
    const auto encoded = makeTlasInstances(instances);
    const auto bytes = checkedMultiply(encoded.size(), sizeof(VkAccelerationStructureInstanceKHR),
                                       "TLAS instance buffer size overflow");
    if (bytes > destination.instanceSize)
        throw std::out_of_range("TLAS instance buffer is too small for the update");
    std::memcpy(destination.mappedInstances, encoded.data(), static_cast<std::size_t>(bytes));

    const VkAccelerationStructureGeometryInstancesDataKHR instanceData{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
        .arrayOfPointers = VK_FALSE,
        .data = {.deviceAddress = bufferDeviceAddress(destination.instanceBuffer)},
    };
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = instanceData;
    const std::uint32_t primitiveCount = static_cast<std::uint32_t>(encoded.size());
    const VkAccelerationStructureBuildGeometryInfoKHR sizeInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                 VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };
    const auto getSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureBuildSizesKHR"));
    const auto build = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(device_, "vkCmdBuildAccelerationStructuresKHR"));
    if (getSizes == nullptr || build == nullptr)
        throw std::runtime_error("Vulkan TLAS build functions are unavailable");
    VkAccelerationStructureBuildSizesInfoKHR sizes{.sType =
                                                       VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getSizes(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &sizeInfo, &primitiveCount, &sizes);
    const auto scratchSize = update ? sizes.updateScratchSize : sizes.buildScratchSize;
    VkDeviceMemory scratchMemory = VK_NULL_HANDLE;
    void* unusedMapped = nullptr;
    const auto scratch =
        createAccelerationBuffer(scratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, scratchMemory, false, &unusedMapped);
    try {
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        buildInfo.flags = sizeInfo.flags;
        buildInfo.mode =
            update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.srcAccelerationStructure = update ? destination.structure : VK_NULL_HANDLE;
        buildInfo.dstAccelerationStructure = destination.structure;
        buildInfo.scratchData.deviceAddress = bufferDeviceAddress(scratch);
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geometry;
        const VkAccelerationStructureBuildRangeInfoKHR range{primitiveCount, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        uploadContext_->begin();
        build(uploadContext_->commandBuffer(), 1, &buildInfo, &ranges);
        const auto signal = uploadContext_->submit();
        uploadContext_->wait(signal);
    } catch (...) {
        uploadContext_->abort();
        destroyAccelerationBuffer(scratch, scratchMemory, unusedMapped);
        throw;
    }
    destroyAccelerationBuffer(scratch, scratchMemory, unusedMapped);
}

void VulkanDevice::recordTopLevelBuildOnCommand(VkCommandBuffer commandBuffer,
                                                const TypedAccelerationStructure& destination,
                                                std::span<const AccelerationInstanceDesc> instances, bool update) {
    if (commandBuffer == VK_NULL_HANDLE)
        throw std::invalid_argument("TLAS command recording requires a command buffer");
    if (!destination.topLevel || destination.instanceBuffer == VK_NULL_HANDLE || destination.mappedInstances == nullptr)
        throw std::logic_error("TLAS has no instance buffer");
    const auto encoded = makeTlasInstances(instances);
    const auto bytes = checkedMultiply(encoded.size(), sizeof(VkAccelerationStructureInstanceKHR),
                                       "TLAS instance buffer size overflow");
    if (bytes > destination.instanceSize)
        throw std::out_of_range("TLAS instance buffer is too small for the update");
    std::memcpy(destination.mappedInstances, encoded.data(), static_cast<std::size_t>(bytes));

    const VkAccelerationStructureGeometryInstancesDataKHR instanceData{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
        .arrayOfPointers = VK_FALSE,
        .data = {.deviceAddress = bufferDeviceAddress(destination.instanceBuffer)},
    };
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = instanceData;
    const std::uint32_t primitiveCount = static_cast<std::uint32_t>(encoded.size());
    const VkAccelerationStructureBuildGeometryInfoKHR sizeInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                 VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };
    const auto getSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureBuildSizesKHR"));
    const auto build = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(device_, "vkCmdBuildAccelerationStructuresKHR"));
    if (getSizes == nullptr || build == nullptr)
        throw std::runtime_error("Vulkan TLAS build functions are unavailable");
    VkAccelerationStructureBuildSizesInfoKHR sizes{.sType =
                                                       VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getSizes(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &sizeInfo, &primitiveCount, &sizes);
    const auto scratchSize = update ? sizes.updateScratchSize : sizes.buildScratchSize;
    const auto scratch = allocateRecordedAccelerationScratch(scratchSize);
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = sizeInfo.flags;
    buildInfo.mode =
        update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.srcAccelerationStructure = update ? destination.structure : VK_NULL_HANDLE;
    buildInfo.dstAccelerationStructure = destination.structure;
    buildInfo.scratchData.deviceAddress = bufferDeviceAddress(scratch);
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
    const VkAccelerationStructureBuildRangeInfoKHR range{primitiveCount, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    build(commandBuffer, 1, &buildInfo, &ranges);
}

handles::AccelerationStructureHandle VulkanDevice::createTlasEx(std::span<const AccelerationInstanceDesc> instances) {
    if (!capabilities_.accelerationStructure || !capabilities_.bufferDeviceAddress)
        throw std::runtime_error("TLAS requires Vulkan acceleration structures and buffer device address");
    const auto encoded = makeTlasInstances(instances);
    TypedAccelerationStructure destination{.instanceSize = encoded.size() * sizeof(VkAccelerationStructureInstanceKHR),
                                           .topLevel = true,
                                           .allowUpdate = true};
    destination.instanceBuffer = createAccelerationBuffer(
        destination.instanceSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        destination.instanceMemory, true, &destination.mappedInstances);
    std::memcpy(destination.mappedInstances, encoded.data(), static_cast<std::size_t>(destination.instanceSize));
    const VkAccelerationStructureGeometryInstancesDataKHR instanceData{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
        .arrayOfPointers = VK_FALSE,
        .data = {.deviceAddress = bufferDeviceAddress(destination.instanceBuffer)},
    };
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = instanceData;
    const std::uint32_t primitiveCount = static_cast<std::uint32_t>(encoded.size());
    const VkAccelerationStructureBuildGeometryInfoKHR sizeInfo{
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                 VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry,
    };
    const auto getSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureBuildSizesKHR"));
    const auto create = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device_, "vkCreateAccelerationStructureKHR"));
    const auto destroy = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device_, "vkDestroyAccelerationStructureKHR"));
    if (getSizes == nullptr || create == nullptr || destroy == nullptr)
        throw std::runtime_error("Vulkan TLAS functions are unavailable");
    VkAccelerationStructureBuildSizesInfoKHR sizes{.sType =
                                                       VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    getSizes(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &sizeInfo, &primitiveCount, &sizes);
    destination.storageSize = sizes.accelerationStructureSize;
    destination.storageBuffer =
        createAccelerationBuffer(destination.storageSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                                 destination.storageMemory, false, nullptr);
    try {
        const VkAccelerationStructureCreateInfoKHR createInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = destination.storageBuffer,
            .size = destination.storageSize,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        };
        check(create(device_, &createInfo, nullptr, &destination.structure), "create TLAS");
        recordTopLevelBuild(destination, instances, false);
    } catch (...) {
        if (destination.structure != VK_NULL_HANDLE)
            destroy(device_, destination.structure, nullptr);
        destroyAccelerationBuffer(destination.storageBuffer, destination.storageMemory, nullptr);
        destroyAccelerationBuffer(destination.instanceBuffer, destination.instanceMemory, destination.mappedInstances);
        throw;
    }
    const auto handle = typedAccelerationStructureHandles_.create();
    typedAccelerationStructures_.emplace(handle, destination);
    return handle;
}

handles::AccelerationStructureHandle VulkanDevice::rebuildTlasEx(handles::AccelerationStructureHandle tlas,
                                                                 std::span<const AccelerationInstanceDesc> instances) {
    const auto it = typedAccelerationStructures_.find(tlas);
    if (it == typedAccelerationStructures_.end() || !typedAccelerationStructureHandles_.isAlive(tlas) ||
        !it->second.topLevel)
        throw std::invalid_argument("rebuild TLAS references a stale handle");
    return createTlasEx(instances);
}

void VulkanDevice::updateTlasEx(handles::AccelerationStructureHandle tlas,
                                std::span<const AccelerationInstanceDesc> instances) {
    const auto it = typedAccelerationStructures_.find(tlas);
    if (it == typedAccelerationStructures_.end() || !typedAccelerationStructureHandles_.isAlive(tlas) ||
        !it->second.topLevel)
        throw std::invalid_argument("update TLAS references a stale handle");
    if (instances.size() * sizeof(VkAccelerationStructureInstanceKHR) > it->second.instanceSize)
        throw std::out_of_range("TLAS update exceeds its instance buffer");
    recordTopLevelBuild(it->second, instances, true);
}

void VulkanDevice::destroyAccelerationStructureEx(handles::AccelerationStructureHandle handle) {
    const auto it = typedAccelerationStructures_.find(handle);
    if (it == typedAccelerationStructures_.end() || !typedAccelerationStructureHandles_.isAlive(handle))
        throw std::invalid_argument("stale acceleration-structure handle");
    waitIdle();
    const auto destroy = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device_, "vkDestroyAccelerationStructureKHR"));
    if (destroy == nullptr)
        throw std::runtime_error("vkDestroyAccelerationStructureKHR is unavailable");
    if (it->second.structure != VK_NULL_HANDLE)
        destroy(device_, it->second.structure, nullptr);
    destroyAccelerationBuffer(it->second.storageBuffer, it->second.storageMemory, nullptr);
    destroyAccelerationBuffer(it->second.instanceBuffer, it->second.instanceMemory, it->second.mappedInstances);
    typedAccelerationStructures_.erase(it);
    typedAccelerationStructureHandles_.destroy(handle);
}

} // namespace dayo::graphics
