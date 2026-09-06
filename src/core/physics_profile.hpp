#pragma once

#include "core/model_probe.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace dayo::core {

// Bullet solver policy retained as data so fixtures can pin behavior.
enum class PhysicsSolvePolicy : std::uint8_t { sequentialImpulse = 0, directMlcp = 1 };

// Compatibility knobs quarantined from the default fast path.
struct PhysicsCompatibilityProfile {
    // Constraint force mixing kept from the upstream Dayo baseline.
    float constraintForceMixing{0.00001F};
    // Apply the one-frame offset upstream uses when binding kinematic bones.
    bool useFrameOffset{true};
    // Restrict dynamics to mode-2/0 mirrors from the 1.30 reference capture.
    bool only20{};
    PhysicsSolvePolicy solvePolicy{PhysicsSolvePolicy::sequentialImpulse};
    // Keep decorative static-static joints disabled for determinism.
    bool stableJoints{true};
};

enum class PhysicsRuntimeKind : std::uint8_t { bullet = 0, fallback = 1, nativeSoftBody = 2 };

// Fixed-step accumulator with prewarm/seek support. The runtime (Bullet vs
// fallback) stays behind this facade so callers never branch on handles.
class PhysicsStepper {
  public:
    explicit PhysicsStepper(PhysicsCompatibilityProfile profile = {}, float fixedStep = 1.0F / 120.0F);

    void prewarm(std::size_t steps) noexcept;
    void seekTo(float frame);
    // Advances by deltaSeconds using fixed substeps. Returns substep count.
    std::size_t stepFixed(float deltaSeconds);
    void setGravity(Float3 gravity) noexcept;
    void setGravityNoise(float amplitude, float frequency) noexcept;
    void reset() noexcept;

    [[nodiscard]] float frame() const noexcept {
        return frame_;
    }
    [[nodiscard]] float fixedStep() const noexcept {
        return fixedStep_;
    }
    [[nodiscard]] std::size_t accumulatedSteps() const noexcept {
        return accumulatedSteps_;
    }
    [[nodiscard]] const PhysicsCompatibilityProfile& profile() const noexcept {
        return profile_;
    }
    [[nodiscard]] const Float3& gravity() const noexcept {
        return gravity_;
    }

  private:
    PhysicsCompatibilityProfile profile_;
    float fixedStep_;
    float frame_{};
    float remainder_{};
    Float3 gravity_{0.0F, -98.0F, 0.0F};
    float noiseAmplitude_{};
    float noiseFrequency_{};
    std::size_t accumulatedSteps_{};
};

} // namespace dayo::core
