#include "graphics/subayai_acceleration_structure.hpp"

#include "core/log.hpp"

#include <numeric>

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
                                                   std::uint64_t topologyGeneration,
                                                   std::uint64_t deformVersion) {
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
        blasChangedSinceTlas_ = true;
        ++blasRefits_;
        log::debug("BLAS refit: mesh ", meshId, " deform ", deformVersion);
        return BlasAction::refit;
    }
    log::debug("BLAS unchanged: mesh ", meshId);
    return BlasAction::none;
}

TlasAction AccelerationStructureService::notifyWorld(std::uint64_t worldGeneration,
                                                    std::span<const std::uint32_t> cloneCountsPerMesh) {
    std::size_t instances = 0;
    for (const auto value : cloneCountsPerMesh) {
        instances += static_cast<std::size_t>(value);
    }
    const bool countChanged = instances != tlasInstanceCount_;
    const bool worldChanged = !hasCachedWorld_ || cachedWorldGeneration_ != worldGeneration;
    const bool blasChanged = blasChangedSinceTlas_;

    if (!tlasBuilt_) {
        tlasScratch_.clear();
        tlasScratch_.reserve(instances);
        for (const auto& [id, state] : meshes_) {
            static_cast<void>(id);
            tlasScratch_.push_back(state.blas);
            if (tlasScratch_.size() >= instances) {
                break;
            }
        }
        if (backend_ != nullptr) {
            tlas_ = backend_->createTlas(std::span<const AccelerationStructureHandle>(
                tlasScratch_.data(), tlasScratch_.size()));
        }
        tlasBuilt_ = true;
        tlasInstanceCount_ = instances;
        cachedWorldGeneration_ = worldGeneration;
        hasCachedWorld_ = true;
        blasChangedSinceTlas_ = false;
        ++tlasRebuilds_;
        log::info("TLAS rebuild: instances ", instances);
        return TlasAction::rebuild;
    }

    if (countChanged || blasChanged) {
        if (backend_ != nullptr) {
            backend_->rebuildTlas(tlas_);
        }
        tlasInstanceCount_ = instances;
        cachedWorldGeneration_ = worldGeneration;
        hasCachedWorld_ = true;
        blasChangedSinceTlas_ = false;
        ++tlasRebuilds_;
        log::info("TLAS rebuild: instances ", instances);
        return TlasAction::rebuild;
    }
    if (worldChanged) {
        if (backend_ != nullptr) {
            backend_->updateTlas(tlas_);
        }
        cachedWorldGeneration_ = worldGeneration;
        ++tlasUpdates_;
        log::debug("TLAS update: world ", worldGeneration);
        return TlasAction::update;
    }
    log::debug("TLAS unchanged: instances ", instances);
    return TlasAction::none;
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
