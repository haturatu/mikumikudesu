#pragma once

#include "core/skirt_collision_types.hpp"
#include "graphics/device.hpp"

namespace dayo::graphics {

struct SkirtCollisionUpload {
    std::span<const core::SkirtVertex> vertices;
    std::span<const std::array<std::uint32_t, 4>> triangles;
    std::span<const std::uint32_t> adjacency;
    std::span<const core::SkirtCapsule> capsules;
    std::uint64_t historyGeneration{1};
    std::uint32_t iterations{32};
    std::span<const std::array<std::uint32_t, 2>> legRanges{};
    std::span<const std::uint32_t> legVertices{};
};

struct SkirtCollisionPushConstants {
    std::uint32_t vertexCount{};
    std::uint32_t vertexStride{};
    std::uint32_t normalOffset{};
    std::uint32_t phase{};
    std::uint32_t sourceSlice{};
    std::uint32_t destinationSlice{1};
    std::uint32_t resetHistory{};
    std::uint32_t padding{};
};
static_assert(sizeof(SkirtCollisionPushConstants) == 32);

struct SkirtCollisionGpuStats {
    float maxPenetration{};
    float correctionDistance{};
    float padding[2]{};
    std::uint32_t penetratingVertexCount{};
    std::uint32_t penetratingTriangleCount{};
    std::uint32_t padding2[2]{};
};
static_assert(sizeof(SkirtCollisionGpuStats) == 32);

[[nodiscard]] DescriptorSetLayoutDesc skirtCollisionDescriptorLayout();
[[nodiscard]] PipelineLayoutDesc skirtCollisionPipelineLayout(handles::DescriptorSetLayoutHandle layout);

// One history buffer is ordered across submissions by the compute barriers.
// Frame-local descriptors and uploads avoid overwriting inputs still in flight.
class SkirtCollisionRuntime {
  public:
    SkirtCollisionRuntime() = default;
    SkirtCollisionRuntime(const SkirtCollisionRuntime&) = delete;
    SkirtCollisionRuntime& operator=(const SkirtCollisionRuntime&) = delete;
    ~SkirtCollisionRuntime();
    void record(Device& device, CommandList& commands, handles::BufferHandle output, const SkirtCollisionUpload& upload,
                std::uint32_t vertexStride, std::uint32_t normalOffset);
    void reset() noexcept;
    [[nodiscard]] core::SkirtCollisionStats readStats() const;
    [[nodiscard]] std::vector<core::SkirtCapsule> readCapsules() const;

  private:
    struct Slot {
        std::array<handles::BufferHandle, 8> buffers{};
        handles::DescriptorSetHandle set{};
        handles::BufferHandle output{};
    };
    Device* device_{};
    std::array<Slot, kNativeFramesInFlight> slots_{};
    handles::BufferHandle history_{};
    handles::BufferHandle capsuleHistory_{};
    std::array<std::size_t, 6> shape_{};
    std::uint64_t historyGeneration_{};
    bool historyInitialized_{};
    std::size_t lastSlot_{};
};
} // namespace dayo::graphics
