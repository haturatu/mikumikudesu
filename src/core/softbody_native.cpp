#include "core/softbody_native.hpp"

namespace dayo::core {

BulletSoftBodyNative::BulletSoftBodyNative(const PmxModel& model) {
    // Native Bullet soft-body world is not linked in this skeleton build.
    // Count anchors so the fallback parity check has something deterministic.
    for (const auto& softBody : model.softBodies)
        anchorCount_ += softBody.pinnedVertices.size();
    nativeAvailable_ = false;
}

void BulletSoftBodyNative::reset() {
    // No-op until the native world lands; fallback owns simulation state.
}

void BulletSoftBodyNative::step(float deltaSeconds, const Float3& gravity) {
    static_cast<void>(deltaSeconds);
    static_cast<void>(gravity);
    // Deliberately inert: callers keep stepping SoftBodySimulation.
}

void BulletSoftBodyNative::apply(std::span<PmxVertex> vertices) const {
    static_cast<void>(vertices);
}

} // namespace dayo::core
