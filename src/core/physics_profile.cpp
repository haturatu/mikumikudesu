#include "core/physics_profile.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dayo::core {
namespace {

constexpr float kMinimumFixedStep = 1.0e-6F;
constexpr std::size_t kMaxSubstepsPerCall = 4096;

} // namespace

PhysicsStepper::PhysicsStepper(PhysicsCompatibilityProfile profile, float fixedStep)
    : profile_(profile),
      fixedStep_(fixedStep >= kMinimumFixedStep && std::isfinite(fixedStep) ? fixedStep : 1.0F / 120.0F) {}

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
    const double total = static_cast<double>(remainder_) + static_cast<double>(clamped);
    const double maxProcessable = static_cast<double>(fixedStep_) * static_cast<double>(kMaxSubstepsPerCall);
    const double processable = std::min(total, maxProcessable);
    const double requested = std::floor(processable / static_cast<double>(fixedStep_));
    const auto steps = std::min<std::size_t>(requested >= static_cast<double>(std::numeric_limits<std::size_t>::max())
                                                 ? std::numeric_limits<std::size_t>::max()
                                                 : static_cast<std::size_t>(requested),
                                             kMaxSubstepsPerCall);
    // Drop elapsed time beyond the per-call budget instead of carrying an
    // unbounded backlog into every subsequent frame.
    remainder_ = static_cast<float>(processable - static_cast<double>(steps) * static_cast<double>(fixedStep_));
    frame_ += static_cast<float>(static_cast<double>(steps) * static_cast<double>(fixedStep_) * 30.0);
    accumulatedSteps_ += steps;
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
