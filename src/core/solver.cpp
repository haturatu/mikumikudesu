#include "core/solver.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace dayo::core {

const char* solverStageName(SolverStage stage) noexcept {
    switch (stage) {
    case SolverStage::sampling:
        return "sampling";
    case SolverStage::local:
        return "local";
    case SolverStage::appendInherit:
        return "append/inherit";
    case SolverStage::externalParent:
        return "external-parent";
    case SolverStage::ik:
        return "ik";
    case SolverStage::kinematic:
        return "kinematic";
    case SolverStage::physicsFixed:
        return "physics-fixed";
    case SolverStage::feedback:
        return "feedback";
    case SolverStage::post:
        return "post";
    case SolverStage::final:
        return "final";
    }
    return "final";
}

std::vector<SolverBoneState> MotionSolver::runSampling(const VmdMotion& motion, float frame) const {
    std::vector<SolverBoneState> bones;
    bones.reserve(motion.bones.size());
    const auto index = static_cast<std::uint32_t>(frame < 0.0F ? 0.0F : frame);
    for (const auto& key : motion.bones) {
        // Skeleton sampler: nearest key at or before the frame.
        if (key.frame <= index || bones.empty()) {
            auto found = std::ranges::find_if(bones, [&](const SolverBoneState& state) { return state.name == key.name; });
            if (found == bones.end()) {
                bones.push_back({key.name, key.translation, key.rotation, key.physics});
            } else if (key.frame <= index) {
                found->translation = key.translation;
                found->rotation = key.rotation;
                found->physicsDriven = key.physics;
            }
        }
    }
    return bones;
}

void MotionSolver::runLocal(std::vector<SolverBoneState>& bones) const {
    static_cast<void>(bones);
}

void MotionSolver::runAppendInherit(std::vector<SolverBoneState>& bones) const {
    // Upstream append/rotate+move inherit order is quarantined: when
    // quarantineUnknownOrder is set we run the upstream130-compatible slot
    // (currently identical to the documented slot; divergence lands here).
    static_cast<void>(bones);
    static_cast<void>(profile_.quarantineUnknownOrder);
}

void MotionSolver::runExternalParent(std::vector<SolverBoneState>& bones) const {
    static_cast<void>(bones);
}

void MotionSolver::runIk(std::vector<SolverBoneState>& bones, float frame) const {
    static_cast<void>(bones);
    static_cast<void>(frame);
}

void MotionSolver::runKinematic(std::vector<SolverBoneState>& bones) const {
    static_cast<void>(bones);
}

void MotionSolver::runPhysicsFixed(std::vector<SolverBoneState>& bones, float deltaSeconds) const {
    static_cast<void>(bones);
    static_cast<void>(deltaSeconds);
}

void MotionSolver::runFeedback(std::vector<SolverBoneState>& bones) const {
    static_cast<void>(bones);
}

void MotionSolver::runPost(std::vector<SolverBoneState>& bones) const {
    static_cast<void>(bones);
}

SolverResult MotionSolver::solveModel(const VmdMotion& motion, float frame) const {
    auto bones = runSampling(motion, frame);
    runLocal(bones);
    runAppendInherit(bones);
    runExternalParent(bones);
    runIk(bones, frame);
    runKinematic(bones);
    runPhysicsFixed(bones, 1.0F / 30.0F);
    runFeedback(bones);
    runPost(bones);
    SolverResult result;
    result.bones = std::move(bones);
    result.camera = solveCamera(motion, frame);
    result.usedUpstreamFallback = profile_.upstream130 && profile_.quarantineUnknownOrder;
    result.lastStage = SolverStage::final;
    return result;
}

VmdCameraState MotionSolver::solveCamera(const VmdMotion& motion, float frame) const {
    // Camera chain is intentionally separate from the model chain.
    return evaluateCamera(motion, frame);
}

bool MotionSolver::compareBones(const std::vector<SolverBoneState>& left, const std::vector<SolverBoneState>& right,
                                std::string* report) const {
    if (left.size() != right.size()) {
        if (report != nullptr)
            *report = "bone count mismatch";
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        float positionError = 0.0F;
        for (std::size_t axis = 0; axis < 3; ++axis)
            positionError = std::max(positionError, std::abs(left[index].translation[axis] - right[index].translation[axis]));
        if (positionError > profile_.positionTolerance) {
            if (report != nullptr) {
                std::ostringstream message;
                message << "bone " << left[index].name << " position error " << positionError;
                *report = message.str();
            }
            return false;
        }
    }
    return true;
}

bool MotionSolver::compareCamera(const VmdCameraState& left, const VmdCameraState& right, std::string* report) const {
    const float distanceError = std::abs(left.distance - right.distance);
    if (distanceError > profile_.cameraTolerance) {
        if (report != nullptr)
            *report = "camera distance mismatch";
        return false;
    }
    return true;
}

} // namespace dayo::core
