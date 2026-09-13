#pragma once

#include "graphics/subayai_acceleration_structure.hpp"
#include "graphics/subayai_deform.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>

namespace dayo::graphics {

// One animated mesh input to the native geometry bridge. The deform runtime
// owns the uploaded buffers; the generations are supplied by the scene so the
// acceleration service can distinguish topology, deformation, and world-only
// changes.
struct NativeGeometryMeshUpload {
    std::uint32_t meshId{};
    NativeDeformUpload deform;
    handles::PipelineHandle deformPipeline{};
    handles::DescriptorSetLayoutHandle deformDescriptorLayout{};
    std::uint64_t topologyGeneration{};
    std::uint64_t deformVersion{};
};

// Connects the native deform pass to BLAS/TLAS policy. Deform commands must be
// recorded and submitted before synchronizeAcceleration() is called: Vulkan
// acceleration builds consume the persistent deformed vertex buffer produced
// by that dispatch.
class NativeGeometryRuntime {
  public:
    explicit NativeGeometryRuntime(IAccelerationBackend* backend = nullptr) : backend_(backend), acceleration_(backend) {}
    ~NativeGeometryRuntime();

    NativeGeometryRuntime(const NativeGeometryRuntime&) = delete;
    NativeGeometryRuntime& operator=(const NativeGeometryRuntime&) = delete;

    void setBackend(IAccelerationBackend* backend) noexcept;

    [[nodiscard]] bool initialize(Device& device, std::span<const NativeGeometryMeshUpload> meshes,
                                  std::string* error = nullptr);
    [[nodiscard]] bool updateMesh(const NativeGeometryMeshUpload& mesh, std::string* error = nullptr);
    void recordDeform(CommandList& commands) const;
    [[nodiscard]] bool synchronizeAcceleration(std::string* error = nullptr);
    [[nodiscard]] TlasAction synchronizeWorld(std::uint64_t worldGeneration,
                                               std::span<const WorldInstance> instances);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && !meshes_.empty();
    }
    [[nodiscard]] std::size_t meshCount() const noexcept {
        return meshes_.size();
    }
    [[nodiscard]] const NativeDeformRuntime* deform(std::uint32_t meshId) const noexcept;
    [[nodiscard]] handles::AccelerationStructureHandle blas(std::uint32_t meshId) const noexcept {
        return acceleration_.blas(meshId);
    }
    [[nodiscard]] handles::AccelerationStructureHandle tlas() const noexcept {
        return acceleration_.tlas();
    }
    [[nodiscard]] handles::DescriptorSetLayoutHandle descriptorLayout() const noexcept {
        return descriptorLayout_;
    }
    [[nodiscard]] handles::DescriptorSetHandle descriptorSet() const noexcept {
        return descriptorSet_;
    }
    [[nodiscard]] const AccelerationStructureService& acceleration() const noexcept {
        return acceleration_;
    }

  private:
    struct MeshState {
        NativeDeformRuntime deform;
        handles::DescriptorSetLayoutHandle deformDescriptorLayout{};
        std::uint64_t topologyGeneration{};
        std::uint64_t deformVersion{};
    };

    [[nodiscard]] bool initializeMesh(const NativeGeometryMeshUpload& mesh, std::string* error);

    Device* device_{};
    IAccelerationBackend* backend_{};
    AccelerationStructureService acceleration_;
    std::map<std::uint32_t, MeshState> meshes_;
    handles::DescriptorSetLayoutHandle descriptorLayout_{};
    handles::DescriptorSetHandle descriptorSet_{};
};

[[nodiscard]] DescriptorSetLayoutDesc nativeGeometryDescriptorLayout() noexcept;

} // namespace dayo::graphics
