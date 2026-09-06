#pragma once

#include "core/model_probe.hpp"
#include "core/motion.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dayo::core {

// Upstream-1.30 order ambiguities are quarantined here so the documented
// pipeline stays byte-stable while comparisons tolerate numeric drift.
struct SolverCompatibilityProfile {
    bool upstream130{true};
    float positionTolerance{0.001F};
    float rotationTolerance{0.0001F};
    float morphTolerance{0.0001F};
    float cameraTolerance{0.001F};
    // When true, stages whose upstream order is unknown run in the
    // upstream130-compatible slot instead of the documented ideal slot.
    bool quarantineUnknownOrder{true};
};

enum class SolverStage : std::uint8_t {
    sampling = 0,
    local,
    appendInherit,
    externalParent,
    ik,
    kinematic,
    physicsFixed,
    feedback,
    post,
    final
};

struct SolverBoneState {
    std::string name;
    Float3 translation{};
    Float4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
    bool physicsDriven{true};
};

struct SolverResult {
    std::vector<SolverBoneState> bones;
    VmdCameraState camera;
    bool usedUpstreamFallback{};
    SolverStage lastStage{SolverStage::final};
};

// Documented order:
// sampling -> local -> append/inherit -> external parent -> IK ->
// kinematic -> physics fixed -> feedback -> post -> final.
// Camera runs on a separate chain (sampling -> interpolation -> parent).
class MotionSolver {
  public:
    explicit MotionSolver(SolverCompatibilityProfile profile = {}) : profile_(profile) {}

    [[nodiscard]] SolverResult solveModel(const VmdMotion& motion, float frame) const;
    [[nodiscard]] VmdCameraState solveCamera(const VmdMotion& motion, float frame) const;

    // Tolerance comparison skeleton for fixture oracles.
    [[nodiscard]] bool compareBones(const std::vector<SolverBoneState>& left,
                                    const std::vector<SolverBoneState>& right, std::string* report = nullptr) const;
    [[nodiscard]] bool compareCamera(const VmdCameraState& left, const VmdCameraState& right,
                                     std::string* report = nullptr) const;
    [[nodiscard]] const SolverCompatibilityProfile& profile() const noexcept {
        return profile_;
    }

  private:
    [[nodiscard]] std::vector<SolverBoneState> runSampling(const VmdMotion& motion, float frame) const;
    void runLocal(std::vector<SolverBoneState>& bones) const;
    void runAppendInherit(std::vector<SolverBoneState>& bones) const;
    void runExternalParent(std::vector<SolverBoneState>& bones) const;
    void runIk(std::vector<SolverBoneState>& bones, float frame) const;
    void runKinematic(std::vector<SolverBoneState>& bones) const;
    void runPhysicsFixed(std::vector<SolverBoneState>& bones, float deltaSeconds) const;
    void runFeedback(std::vector<SolverBoneState>& bones) const;
    void runPost(std::vector<SolverBoneState>& bones) const;

    SolverCompatibilityProfile profile_;
};

[[nodiscard]] const char* solverStageName(SolverStage stage) noexcept;

} // namespace dayo::core
