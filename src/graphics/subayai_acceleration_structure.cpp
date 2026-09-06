#include "graphics/subayai_acceleration_structure.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace dayo::graphics {

bool AccelerationStructureService::canBuildNative(const DeviceCapabilities& capabilities,
                                                  RendererKind renderer) noexcept {
    switch (renderer) {
    case RendererKind::preview:
        return capabilities.supportsPreview();
    case RendererKind::subayai:
        return capabilities.hardwareSupportsSubayai();
    case RendererKind::bdpt:
        return capabilities.hardwareSupportsBdpt();
    }
    return false;
}

BlasAction AccelerationStructureService::notifyMesh(std::uint32_t meshId, BufferHandle vertexBuffer,
                                                    std::uint64_t topologyGeneration, std::uint64_t deformVersion) {
    auto found = meshes_.find(meshId);
    if (found == meshes_.end()) {
        MeshState state;
        state.topologyGeneration = topologyGeneration;
        state.deformVersion = deformVersion;
        state.vertexBuffer = vertexBuffer;
        if (backend_ != nullptr) {
            state.blas = backend_->createBlas(vertexBuffer);
        }
        state.built = true;
        meshes_.emplace(meshId, state);
        blasChangedSinceTlas_ = true;
        ++blasRebuilds_;
        log::info("BLAS rebuild: mesh ", meshId, " topology ", topologyGeneration);
        return BlasAction::rebuild;
    }
    MeshState& state = found->second;
    if (state.topologyGeneration != topologyGeneration) {
        state.topologyGeneration = topologyGeneration;
        state.deformVersion = deformVersion;
        state.vertexBuffer = vertexBuffer;
        if (backend_ != nullptr) {
            backend_->rebuildBlas(state.blas);
        }
        blasChangedSinceTlas_ = true;
        ++blasRebuilds_;
        log::info("BLAS rebuild: mesh ", meshId, " topology ", topologyGeneration);
        return BlasAction::rebuild;
    }
    if (state.deformVersion != deformVersion) {
        state.deformVersion = deformVersion;
        state.vertexBuffer = vertexBuffer;
        if (backend_ != nullptr) {
            backend_->refitBlas(state.blas);
        }
        ++blasRefits_;
        log::debug("BLAS refit: mesh ", meshId, " deform ", deformVersion);
        return BlasAction::refit;
    }
    log::debug("BLAS unchanged: mesh ", meshId);
    return BlasAction::none;
}

TlasAction AccelerationStructureService::notifyWorld(std::uint64_t worldGeneration,
                                                     std::span<const std::uint32_t> cloneCountsPerMesh) {
    std::vector<std::uint32_t> meshIds;
    meshIds.reserve(meshes_.size());
    for (const auto& [meshId, state] : meshes_) {
        static_cast<void>(state);
        meshIds.push_back(meshId);
    }
    std::sort(meshIds.begin(), meshIds.end());
    if (cloneCountsPerMesh.size() != meshIds.size())
        throw std::invalid_argument("TLAS clone counts must have one entry per registered mesh");

    std::vector<WorldInstance> legacyInstances;
    for (std::size_t meshIndex = 0; meshIndex < cloneCountsPerMesh.size(); ++meshIndex) {
        const auto value = cloneCountsPerMesh[meshIndex];
        if (value > std::numeric_limits<std::size_t>::max() - legacyInstances.size())
            throw std::overflow_error("TLAS instance count overflow");
        legacyInstances.reserve(legacyInstances.size() + value);
        const auto meshId = meshIds[meshIndex];
        for (std::uint32_t clone = 0; clone < value; ++clone)
            legacyInstances.push_back(WorldInstance{.meshId = meshId});
    }
    return notifyWorld(worldGeneration, std::span<const WorldInstance>(legacyInstances.data(), legacyInstances.size()));
}

TlasAction AccelerationStructureService::notifyWorld(std::uint64_t worldGeneration,
                                                     std::span<const WorldInstance> instances) {
    const std::size_t instanceCount = instances.size();
    for (const auto& instance : instances) {
        if (!meshes_.contains(instance.meshId))
            throw std::invalid_argument("TLAS instance references an unknown mesh");
    }
    const bool countChanged = instanceCount != tlasInstanceCount_;
    const bool worldChanged = !hasCachedWorld_ || cachedWorldGeneration_ != worldGeneration;
    const bool blasChanged = blasChangedSinceTlas_;

    const auto rebuildInstances = [&] {
        tlasScratch_.clear();
        tlasScratch_.reserve(instanceCount);
        std::uint32_t instanceId = 0;
        for (const auto& instance : instances) {
            if (instanceId == std::numeric_limits<std::uint32_t>::max())
                throw std::overflow_error("TLAS instance id overflow");
            const auto& state = meshes_.at(instance.meshId);
            tlasScratch_.push_back(TlasInstanceDesc{.blas = state.blas,
                                                    .transform = instance.transform,
                                                    .instanceId = instanceId++,
                                                    .mask = instance.mask,
                                                    .sbtRecordOffset = instance.sbtRecordOffset,
                                                    .flags = instance.flags});
        }
        if (tlasScratch_.size() != instanceCount)
            throw std::logic_error("TLAS instance expansion did not match CloneCount");
    };

    if (!tlasBuilt_) {
        rebuildInstances();
        if (backend_ != nullptr) {
            tlas_ = backend_->createTlas(std::span<const TlasInstanceDesc>(tlasScratch_.data(), tlasScratch_.size()));
        }
        tlasBuilt_ = true;
        tlasInstanceCount_ = instanceCount;
        cachedWorldGeneration_ = worldGeneration;
        hasCachedWorld_ = true;
        blasChangedSinceTlas_ = false;
        ++tlasRebuilds_;
        log::info("TLAS rebuild: instances ", instanceCount);
        return TlasAction::rebuild;
    }

    if (countChanged || blasChanged) {
        rebuildInstances();
        if (backend_ != nullptr) {
            backend_->rebuildTlas(tlas_, std::span<const TlasInstanceDesc>(tlasScratch_.data(), tlasScratch_.size()));
        }
        tlasInstanceCount_ = instanceCount;
        cachedWorldGeneration_ = worldGeneration;
        hasCachedWorld_ = true;
        blasChangedSinceTlas_ = false;
        ++tlasRebuilds_;
        log::info("TLAS rebuild: instances ", instanceCount);
        return TlasAction::rebuild;
    }
    if (worldChanged) {
        rebuildInstances();
        if (backend_ != nullptr) {
            backend_->updateTlas(tlas_, std::span<const TlasInstanceDesc>(tlasScratch_.data(), tlasScratch_.size()));
        }
        cachedWorldGeneration_ = worldGeneration;
        ++tlasUpdates_;
        log::debug("TLAS update: world ", worldGeneration);
        return TlasAction::update;
    }
    log::debug("TLAS unchanged: instances ", instanceCount);
    return TlasAction::none;
}

bool AccelerationStructureService::removeMesh(std::uint32_t meshId) {
    const auto found = meshes_.find(meshId);
    if (found == meshes_.end())
        return false;

    if (tlasBuilt_) {
        if (backend_ != nullptr && tlas_ != 0)
            backend_->destroyTlas(tlas_);
        tlas_ = {};
        tlasBuilt_ = false;
        tlasInstanceCount_ = 0;
        tlasScratch_.clear();
        hasCachedWorld_ = false;
        cachedWorldGeneration_ = 0;
    }
    if (backend_ != nullptr && found->second.built && found->second.blas != 0)
        backend_->destroyBlas(found->second.blas);
    meshes_.erase(found);
    blasChangedSinceTlas_ = true;
    log::info("BLAS removed: mesh ", meshId);
    return true;
}

const char* toString(BlasAction action) noexcept {
    switch (action) {
    case BlasAction::none:
        return "none";
    case BlasAction::rebuild:
        return "rebuild";
    case BlasAction::refit:
        return "refit";
    }
    return "none";
}

const char* toString(TlasAction action) noexcept {
    switch (action) {
    case TlasAction::none:
        return "none";
    case TlasAction::rebuild:
        return "rebuild";
    case TlasAction::update:
        return "update";
    }
    return "none";
}

} // namespace dayo::graphics
