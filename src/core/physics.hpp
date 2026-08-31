#pragma once

#include "core/model_probe.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

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
    [[nodiscard]] std::uint8_t bodyMode(std::size_t body) const noexcept;
    void reset();
    void step(float deltaSeconds);
    void setGravity(const Float3& gravity);
    void setGravityNoise(float amplitude, float frequency);
    void setFloorCollision(bool enabled);
    void setKinematicTransform(std::size_t body, const PhysicsTransform& transform);
    void teleportBody(std::size_t body, const PhysicsTransform& transform);
    void shiftBodyPosition(std::size_t body, const Float3& delta);
    void applyImpulse(std::size_t body, const Float3& linear, const Float3& angular, bool local);
    void clearMotion(std::size_t body);
    [[nodiscard]] PhysicsTransform bodyTransform(std::size_t body) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// PMX 2.1 soft-body fallback. Bullet's rigid-body world remains the fast
// path; this deterministic solver keeps cloth/hair moving when Bullet's
// optional soft-body module is unavailable.
class SoftBodySimulation {
public:
    explicit SoftBodySimulation(const PmxModel& model);
    [[nodiscard]] bool available() const noexcept { return bodyCount_ != 0 && !positions_.empty(); }
    [[nodiscard]] std::size_t bodyCount() const noexcept { return bodyCount_; }
    void reset();
    void step(float deltaSeconds, const Float3& gravity);
    void apply(std::span<PmxVertex> vertices) const;

private:
    std::vector<Float3> initial_;
    std::vector<Float3> positions_;
    std::vector<Float3> velocities_;
    std::vector<std::uint8_t> pinned_;
    std::vector<std::uint8_t> active_;
    std::size_t bodyCount_ {};
};

} // namespace dayo::core
