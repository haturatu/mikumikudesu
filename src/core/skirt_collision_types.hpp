#pragma once

#include <array>
#include <cstdint>

namespace dayo::core {

struct SkirtVertex {
    std::uint32_t vertex{};
    std::uint32_t firstTriangle{};
    std::uint32_t triangleCount{};
    std::uint32_t capsuleStart{};
    float mobility{1};
    std::uint32_t capsuleCount{};
    // Packed SNORM16 octahedral waist-relative direction and validity flag.
    std::uint32_t outwardNormal[2]{};
};
static_assert(sizeof(SkirtVertex) == 32);

// Capsule endpoints use the same normalized model space as the deformed mesh.
struct SkirtCapsule {
    std::array<float, 4> aRadius{};
    std::array<float, 4> bMargin{};
    std::array<float, 4> previousA{};
    std::array<float, 4> previousB{};
    std::array<float, 4> rotationDelta{0, 0, 0, 1};
    // Waist rotation transports the cloth reference; the capsule axis follows the leg.
    std::array<float, 4> surfaceRotation{0, 0, 0, 1};
};
static_assert(sizeof(SkirtCapsule) == 96);

struct SkirtCollisionStats {
    std::uint32_t rigidContactCount{};
    std::uint32_t penetratingVertexCount{};
    std::uint32_t penetratingTriangleCount{};
    float maxRigidPenetration{};
    float maxMeshPenetration{};
    float totalCorrectionDistance{};
};

} // namespace dayo::core
