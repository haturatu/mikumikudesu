#pragma once

#include "core/model_probe.hpp"

#include <cstddef>
#include <span>

namespace dayo::core {

// Bullet soft-body native path skeleton. The existing deterministic
// SoftBodySimulation fallback remains the default; this class only records
// the native wiring surface so enabling it later is additive.
class BulletSoftBodyNative {
  public:
    explicit BulletSoftBodyNative(const PmxModel& model);
    [[nodiscard]] bool available() const noexcept {
        return nativeAvailable_;
    }
    [[nodiscard]] bool usingFallback() const noexcept {
        return !nativeAvailable_;
    }
    [[nodiscard]] std::size_t anchorCount() const noexcept {
        return anchorCount_;
    }
    void reset();
    void step(float deltaSeconds, const Float3& gravity);
    void apply(std::span<PmxVertex> vertices) const;

  private:
    bool nativeAvailable_{false};
    std::size_t anchorCount_{};
};

} // namespace dayo::core
