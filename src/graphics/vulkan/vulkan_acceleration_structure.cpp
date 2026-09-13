#include "graphics/vulkan/vulkan_acceleration_structure.hpp"

#include "graphics/vulkan/vulkan_command_list.hpp"
#include "graphics/vulkan/vulkan_device.hpp"

#include <stdexcept>
#include <vector>

namespace dayo::graphics {
namespace {

AccelerationInstanceDesc convert(const TlasInstanceDesc& instance) {
    return {.blas = instance.blas,
            .transform = instance.transform.values,
            .instanceId = instance.instanceId,
            .mask = instance.mask,
            .sbtRecordOffset = instance.sbtRecordOffset,
            .flags = instance.flags};
}

std::vector<AccelerationInstanceDesc> convert(std::span<const TlasInstanceDesc> instances) {
    std::vector<AccelerationInstanceDesc> result;
    result.reserve(instances.size());
    for (const auto& instance : instances)
        result.push_back(convert(instance));
    return result;
}

} // namespace

handles::AccelerationStructureHandle VulkanAccelerationBackend::createBlas(const BlasGeometryDesc& geometry) {
    return device_->createBlasEx(geometry);
}

handles::AccelerationStructureHandle VulkanAccelerationBackend::rebuildBlas(handles::AccelerationStructureHandle blas,
                                                                            const BlasGeometryDesc& geometry) {
    return device_->rebuildBlasEx(blas, geometry);
}

void VulkanAccelerationBackend::refitBlas(handles::AccelerationStructureHandle blas, const BlasGeometryDesc& geometry) {
    device_->refitBlasEx(blas, geometry);
}

handles::AccelerationStructureHandle
VulkanAccelerationBackend::createTlas(std::span<const TlasInstanceDesc> instances) {
    const auto converted = convert(instances);
    return device_->createTlasEx(converted);
}

handles::AccelerationStructureHandle
VulkanAccelerationBackend::rebuildTlas(handles::AccelerationStructureHandle tlas,
                                       std::span<const TlasInstanceDesc> instances) {
    const auto converted = convert(instances);
    return device_->rebuildTlasEx(tlas, converted);
}

void VulkanAccelerationBackend::updateTlas(handles::AccelerationStructureHandle tlas,
                                           std::span<const TlasInstanceDesc> instances) {
    const auto converted = convert(instances);
    device_->updateTlasEx(tlas, converted);
}

void VulkanAccelerationBackend::destroyBlas(handles::AccelerationStructureHandle blas) {
    device_->destroyAccelerationStructureEx(blas);
}

void VulkanAccelerationBackend::destroyTlas(handles::AccelerationStructureHandle tlas) {
    device_->destroyAccelerationStructureEx(tlas);
}

void VulkanAccelerationBackend::recordBlasUpdate(CommandList& commands,
                                                 handles::AccelerationStructureHandle blas,
                                                 const BlasGeometryDesc& geometry) {
    auto* vulkanCommands = dynamic_cast<VulkanCommandList*>(&commands);
    if (vulkanCommands == nullptr)
        throw std::invalid_argument("Vulkan acceleration updates require a Vulkan command list");
    device_->recordBlasUpdate(vulkanCommands->commandBuffer(), blas, geometry);
}

void VulkanAccelerationBackend::recordTlasUpdate(CommandList& commands,
                                                 handles::AccelerationStructureHandle tlas,
                                                 std::span<const TlasInstanceDesc> instances) {
    auto* vulkanCommands = dynamic_cast<VulkanCommandList*>(&commands);
    if (vulkanCommands == nullptr)
        throw std::invalid_argument("Vulkan acceleration updates require a Vulkan command list");
    device_->recordTlasUpdate(vulkanCommands->commandBuffer(), tlas, convert(instances));
}

} // namespace dayo::graphics
