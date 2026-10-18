#pragma once

#include "core/animation.hpp"
#include "core/physics.hpp"
#include "core/skirt_collision_types.hpp"

#include <array>
#include <span>
#include <vector>

namespace dayo::core {

struct SkirtCollisionTopology {
    std::vector<SkirtVertex> vertices;
    std::vector<std::array<std::uint32_t, 4>> triangles;
    std::vector<std::uint32_t> adjacency;
    std::vector<std::size_t> legBodies;
    std::vector<std::size_t> skirtBodies;
    std::vector<std::array<std::uint32_t, 2>> legRanges;
    std::vector<std::uint32_t> legVertices;
    [[nodiscard]] bool empty() const noexcept {
        return vertices.empty() || legBodies.empty();
    }
};

[[nodiscard]] SkirtCollisionTopology identifySkirtCollision(const PmxModel& model);
[[nodiscard]] std::vector<SkirtCapsule> makeSkirtCapsules(const PmxModel& model, const SkirtCollisionTopology& topology,
                                                          const MmdPhysics& physics,
                                                          std::span<const AnimatedModelFrame::BoneTransform> bones,
                                                          PreviewNormalization normalization,
                                                          std::span<const mmd::PhysicsTransform> previous = {});
[[nodiscard]] SkirtCollisionStats skirtRigidStats(const SkirtCollisionTopology& topology, const MmdPhysics& physics);

} // namespace dayo::core
