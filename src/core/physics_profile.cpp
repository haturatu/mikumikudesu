#include "core/physics_profile.hpp"

#include <algorithm>
#include <cmath>

namespace dayo::core {

PhysicsStepper::PhysicsStepper(PhysicsCompatibilityProfile profile, float fixedStep)
    : profile_(profile), fixedStep_(fixedStep > 0.0F && std::isfinite(fixedStep) ? fixedStep : 1.0F / 120.0F) {}

void PhysicsStepper::prewarm(std::size_t steps) noexcept {
    accumulatedSteps_ += steps;
}

void PhysicsStepper::seekTo(float frame) {
    if (!std::isfinite(frame))
        return;
    frame_ = std::max(frame, 0.0F);
    remainder_ = 0.0F;
}

std::size_t PhysicsStepper::stepFixed(float deltaSeconds) {
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0F)
        return 0;
    const float clamped = std::min(deltaSeconds, 0.25F);
    remainder_ += clamped;
    std::size_t steps = 0;
    while (remainder_ >= fixedStep_) {
        remainder_ -= fixedStep_;
        frame_ += fixedStep_ * 30.0F;
        ++accumulatedSteps_;
        ++steps;
    }
    return steps;
}

void PhysicsStepper::setGravity(Float3 gravity) noexcept {
    gravity_ = gravity;
}

void PhysicsStepper::setGravityNoise(float amplitude, float frequency) noexcept {
    noiseAmplitude_ = std::max(amplitude, 0.0F);
    noiseFrequency_ = std::max(frequency, 0.0F);
}

void PhysicsStepper::reset() noexcept {
    frame_ = 0.0F;
    remainder_ = 0.0F;
    accumulatedSteps_ = 0;
}

} // namespace dayo::core
