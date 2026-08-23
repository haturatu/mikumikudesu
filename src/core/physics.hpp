#pragma once

#include "core/model_probe.hpp"

#include <cstddef>
#include <memory>
#include <span>

namespace dayo::core {

struct PhysicsTransform {
    Float3 position {};
    Float4 rotation { 0.0F, 0.0F, 0.0F, 1.0F };
};

class MmdPhysics {
public:
    explicit MmdPhysics(const PmxModel& model);
    ~MmdPhysics();
    MmdPhysics(MmdPhysics&&) noexcept;
    MmdPhysics& operator=(MmdPhysics&&) noexcept;
    MmdPhysics(const MmdPhysics&) = delete;
    MmdPhysics& operator=(const MmdPhysics&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] std::size_t bodyCount() const noexcept;
    [[nodiscard]] std::size_t jointCount() const noexcept;
    void reset();
    void step(float deltaSeconds);
    void setGravity(const Float3& gravity);
    void setKinematicTransform(std::size_t body, const PhysicsTransform& transform);
    void applyImpulse(std::size_t body, const Float3& linear, const Float3& angular, bool local);
    void clearMotion(std::size_t body);
    [[nodiscard]] PhysicsTransform bodyTransform(std::size_t body) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dayo::core
