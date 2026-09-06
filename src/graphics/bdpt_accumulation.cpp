#include "graphics/bdpt_accumulation.hpp"

#include "core/log.hpp"
#include "core/scene.hpp"

#include <limits>

namespace dayo::graphics {

void BdptAccumulation::ensurePersistent() {
    if (persistentReady_) {
        log::debug("BDPT persistent resources reused");
        return;
    }
    spectralLut_.assign(64U * 3U, 1.0F);
    blackbodyLut_.assign(256U * 3U, 1.0F);
    for (std::size_t index = 0; index < volumes_.size(); ++index) {
        volumes_[index].handle = static_cast<std::uint64_t>(index + 1U);
        volumes_[index].valid = true;
    }
    spectralReady_ = true;
    blackbodyReady_ = true;
    persistentReady_ = true;
    ++generations_;
    log::info("BDPT persistent LUTs and 8 volume slots ready");
}

bool BdptAccumulation::beginFrame(core::DirtyFlag dirty) noexcept {
    if (dirty != core::DirtyFlag::none) {
        sampleIndex_ = 0;
        needsClear_ = true;
        log::debug("BDPT accumulation reset: dirty, sample 0");
        return true;
    }
    if (sampleIndex_ < std::numeric_limits<std::uint32_t>::max()) {
        ++sampleIndex_;
    }
    needsClear_ = false;
    log::debug("BDPT accumulation continue: sample ", sampleIndex_);
    return false;
}

bool BdptAccumulation::syncScene(core::Scene& scene) {
    ensurePersistent();
    const core::DirtyFlag dirty = scene.dirtyFlags();
    if (dirty != core::DirtyFlag::none) {
        sampleIndex_ = 0;
        needsClear_ = true;
        scene.invalidateAccumulation();
        log::debug("BDPT accumulation reset with scene: sample 0");
        return true;
    }
    scene.advanceAccumulation();
    const auto samples = scene.accumulatedSamples();
    sampleIndex_ = samples > std::numeric_limits<std::uint32_t>::max()
                       ? std::numeric_limits<std::uint32_t>::max()
                       : static_cast<std::uint32_t>(samples);
    needsClear_ = false;
    log::debug("BDPT accumulation synced with scene: sample ", sampleIndex_);
    return false;
}

} // namespace dayo::graphics
