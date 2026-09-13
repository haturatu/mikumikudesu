#pragma once

#include "graphics/subayai_acceleration_structure.hpp"

namespace dayo::graphics {

class VulkanDevice;

// Adapter from the renderer-neutral rebuild/refit policy to Vulkan's typed
// acceleration-structure resources. The policy service remains usable with a
// mock backend, while the application can install this adapter on Vulkan.
class VulkanAccelerationBackend final : public IAccelerationBackend {
  public:
    explicit VulkanAccelerationBackend(VulkanDevice& device) noexcept : device_(&device) {}

    [[nodiscard]] handles::AccelerationStructureHandle createBlas(const BlasGeometryDesc& geometry) override;
    [[nodiscard]] handles::AccelerationStructureHandle rebuildBlas(handles::AccelerationStructureHandle blas,
                                                                   const BlasGeometryDesc& geometry) override;
    void refitBlas(handles::AccelerationStructureHandle blas, const BlasGeometryDesc& geometry) override;
    [[nodiscard]] handles::AccelerationStructureHandle createTlas(std::span<const TlasInstanceDesc> instances) override;
    [[nodiscard]] handles::AccelerationStructureHandle
    rebuildTlas(handles::AccelerationStructureHandle tlas, std::span<const TlasInstanceDesc> instances) override;
    void updateTlas(handles::AccelerationStructureHandle tlas, std::span<const TlasInstanceDesc> instances) override;
    void destroyBlas(handles::AccelerationStructureHandle blas) override;
    void destroyTlas(handles::AccelerationStructureHandle tlas) override;

  private:
    VulkanDevice* device_{};
};

} // namespace dayo::graphics
