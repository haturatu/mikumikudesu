#include "graphics/subayai_geometry.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {

namespace {

[[nodiscard]] ShaderStageMask nativeGeometryStages() noexcept {
    return ShaderStageMask::compute | ShaderStageMask::fragment | ShaderStageMask::rayGeneration |
           ShaderStageMask::miss | ShaderStageMask::closestHit | ShaderStageMask::anyHit |
           ShaderStageMask::intersection | ShaderStageMask::callable;
}

} // namespace

DescriptorSetLayoutDesc nativeGeometryDescriptorLayout() noexcept {
    return {.bindings = {{0, DescriptorKind::accelerationStructure, 1, nativeGeometryStages()}}};
}

NativeGeometryRuntime::~NativeGeometryRuntime() {
    reset();
}

void NativeGeometryRuntime::setBackend(IAccelerationBackend* backend) noexcept {
    if (backend_ == backend)
        return;
    reset();
    backend_ = backend;
    acceleration_.setBackend(backend);
}

bool NativeGeometryRuntime::initialize(Device& device, std::span<const NativeGeometryMeshUpload> meshes,
                                       std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    try {
        if (meshes.empty())
            throw std::invalid_argument("native geometry requires at least one mesh");
        if (backend_ == nullptr || !device.capabilities().accelerationStructure ||
            !device.capabilities().bufferDeviceAddress)
            throw std::runtime_error("native geometry requires acceleration-structure device support");
        device_ = &device;
        descriptorLayout_ = device.createDescriptorSetLayoutEx(nativeGeometryDescriptorLayout());
        if (!descriptorLayout_.valid())
            throw std::runtime_error("native geometry descriptor layout is invalid");
        for (const auto& mesh : meshes) {
            if (!meshes_.try_emplace(mesh.meshId).second)
                throw std::invalid_argument("native geometry contains a duplicate mesh id");
            if (!initializeMesh(mesh, error)) {
                reset();
                return false;
            }
        }
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = exception.what();
        reset();
        return false;
    } catch (...) {
        if (error != nullptr)
            *error = "native geometry initialization failed";
        reset();
        return false;
    }
    return true;
}

bool NativeGeometryRuntime::initializeMesh(const NativeGeometryMeshUpload& mesh, std::string* error) {
    auto found = meshes_.find(mesh.meshId);
    if (found == meshes_.end() || device_ == nullptr)
        return false;
    if (!found->second.deform.initialize(*device_, mesh.deform, mesh.deformPipeline, mesh.deformDescriptorLayout,
                                         error))
        return false;
    found->second.deformDescriptorLayout = mesh.deformDescriptorLayout;
    found->second.topologyGeneration = mesh.topologyGeneration;
    found->second.deformVersion = mesh.deformVersion;
    return true;
}

bool NativeGeometryRuntime::updateMesh(const NativeGeometryMeshUpload& mesh, std::string* error) {
    if (error != nullptr)
        error->clear();
    try {
        if (!ready() || device_ == nullptr)
            throw std::logic_error("native geometry runtime is not initialized");
        const auto found = meshes_.find(mesh.meshId);
        if (found == meshes_.end())
            throw std::invalid_argument("native geometry update references an unknown mesh");
        if (mesh.deformPipeline != found->second.deform.pipeline() ||
            mesh.deformDescriptorLayout != found->second.deformDescriptorLayout) {
            throw std::invalid_argument("native geometry update cannot replace the deform pipeline");
        }
        if (!found->second.deform.update(*device_, mesh.deform, error))
            return false;
        found->second.topologyGeneration = mesh.topologyGeneration;
        found->second.deformVersion = mesh.deformVersion;
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = exception.what();
        return false;
    } catch (...) {
        if (error != nullptr)
            *error = "native geometry update failed";
        return false;
    }
    return true;
}

void NativeGeometryRuntime::recordDeform(CommandList& commands) const {
    if (!ready())
        throw std::logic_error("native geometry runtime is not initialized");
    for (const auto& [meshId, state] : meshes_) {
        static_cast<void>(meshId);
        state.deform.record(commands);
    }
    commands.memoryBarrierEx();
}

void NativeGeometryRuntime::recordAcceleration(CommandList& commands) const {
    if (!ready())
        throw std::logic_error("native geometry runtime is not initialized");
    commands.accelerationStructureBarrierEx();
    acceleration_.recordBlasUpdates(commands);
    // BLAS writes must be visible to the TLAS build, and the completed TLAS
    // must be visible to the following ray-query or ray-tracing pass.
    commands.accelerationStructureBarrierEx();
    acceleration_.recordTlasUpdate(commands);
    commands.accelerationStructureBarrierEx();
}

bool NativeGeometryRuntime::synchronizeAcceleration(std::string* error) {
    if (error != nullptr)
        error->clear();
    try {
        if (!ready())
            throw std::logic_error("native geometry runtime is not initialized");
        for (auto& [meshId, state] : meshes_)
            static_cast<void>(acceleration_.notifyMesh(meshId, state.deform.blasGeometry(), state.topologyGeneration,
                                                       state.deformVersion));
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = exception.what();
        return false;
    } catch (...) {
        if (error != nullptr)
            *error = "native acceleration synchronization failed";
        return false;
    }
    return true;
}

TlasAction NativeGeometryRuntime::synchronizeWorld(std::uint64_t worldGeneration,
                                                    std::span<const WorldInstance> instances) {
    if (!ready())
        throw std::logic_error("native geometry runtime is not initialized");
    const auto action = acceleration_.notifyWorld(worldGeneration, instances);
    const auto tlas = acceleration_.tlas();
    if (!tlas.valid())
        throw std::runtime_error("native geometry did not produce a TLAS");
    const std::array<DescriptorBindingEx, 1> bindings{
        DescriptorBindingEx{.slot = 0, .arrayElement = 0, .accelerationStructure = tlas}};
    if (descriptorSet_.valid()) {
        device_->updateDescriptorSetEx(descriptorSet_, bindings);
    } else {
        descriptorSet_ = device_->allocateDescriptorSetEx(descriptorLayout_, bindings);
        if (!descriptorSet_.valid())
            throw std::runtime_error("native geometry acceleration descriptor set is invalid");
    }
    return action;
}

const NativeDeformRuntime* NativeGeometryRuntime::deform(std::uint32_t meshId) const noexcept {
    const auto found = meshes_.find(meshId);
    return found == meshes_.end() ? nullptr : &found->second.deform;
}

void NativeGeometryRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        if (descriptorSet_.valid()) {
            try {
                device->destroyDescriptorSetEx(descriptorSet_);
            } catch (...) {
            }
        }
        if (descriptorLayout_.valid()) {
            try {
                device->destroyDescriptorSetLayoutEx(descriptorLayout_);
            } catch (...) {
            }
        }
    }
    acceleration_.reset();
    meshes_.clear();
    device_ = nullptr;
    descriptorLayout_ = {};
    descriptorSet_ = {};
}

} // namespace dayo::graphics
