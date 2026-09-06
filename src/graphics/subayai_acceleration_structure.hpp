#pragma once

#include "graphics/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace dayo::graphics {

// Host-side bookkeeping for Subayai/BDPT acceleration structures.
// Vulkan builds stay behind the RT feature gate; this service only decides
// rebuild / refit / update and tracks TLAS instance counts derived from
// Scene::cloneCount. nativeSubayai/nativeBdpt remain false, so callers must
// fall back to Preview when DeviceCapabilities::supports() is false.
enum class BlasAction { none, rebuild, refit };
enum class TlasAction { none, rebuild, update };

struct Matrix3x4 {
    std::array<float, 12> values{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
};

struct TlasInstanceDesc {
    AccelerationStructureHandle blas{};
    Matrix3x4 transform{};
    std::uint32_t instanceId{};
    std::uint32_t mask{0xFFU};
    std::uint32_t sbtRecordOffset{};
    std::uint32_t flags{};
};

// World-facing instance input. Mesh identity is explicit so clone transforms
// cannot be detached from the BLAS they address.
struct WorldInstance {
    std::uint32_t meshId{};
    Matrix3x4 transform{};
    std::uint32_t mask{0xFFU};
    std::uint32_t sbtRecordOffset{};
    std::uint32_t flags{};
};

class IAccelerationBackend {
  public:
    virtual ~IAccelerationBackend() = default;
    virtual AccelerationStructureHandle createBlas(BufferHandle vertexBuffer) = 0;
    virtual void rebuildBlas(AccelerationStructureHandle blas) = 0;
    virtual void refitBlas(AccelerationStructureHandle blas) = 0;
    virtual AccelerationStructureHandle createTlas(std::span<const TlasInstanceDesc> instances) = 0;
    virtual void rebuildTlas(AccelerationStructureHandle tlas, std::span<const TlasInstanceDesc> instances) = 0;
    virtual void updateTlas(AccelerationStructureHandle tlas, std::span<const TlasInstanceDesc> instances) = 0;
    virtual void destroyBlas(AccelerationStructureHandle) {}
    virtual void destroyTlas(AccelerationStructureHandle) {}
};

class AccelerationStructureService {
  public:
    explicit AccelerationStructureService(IAccelerationBackend* backend) : backend_(backend) {}

    // Returns true when the requested renderer can use native RT paths.
    // RT-incapable GPUs keep running Preview; this never enables native passes.
    [[nodiscard]] static bool canBuildNative(const DeviceCapabilities& capabilities, RendererKind renderer) noexcept;

    [[nodiscard]] BlasAction notifyMesh(std::uint32_t meshId, BufferHandle vertexBuffer,
                                        std::uint64_t topologyGeneration, std::uint64_t deformVersion);
    // cloneCountsPerMesh holds visible-model clone counts only; the TLAS
    // instance count is their sum so CloneCount is reflected directly. Meshes
    // are ordered by mesh ID for this compatibility overload. New callers
    // should use the transform-aware overload below.
    [[nodiscard]] TlasAction notifyWorld(std::uint64_t worldGeneration,
                                         std::span<const std::uint32_t> cloneCountsPerMesh);
    [[nodiscard]] TlasAction notifyWorld(std::uint64_t worldGeneration, std::span<const WorldInstance> instances);

    [[nodiscard]] bool removeMesh(std::uint32_t meshId);

    [[nodiscard]] std::size_t blasCount() const noexcept {
        return meshes_.size();
    }
    [[nodiscard]] std::size_t tlasInstanceCount() const noexcept {
        return tlasInstanceCount_;
    }
    [[nodiscard]] bool tlasBuilt() const noexcept {
        return tlasBuilt_;
    }
    [[nodiscard]] std::uint64_t blasRebuilds() const noexcept {
        return blasRebuilds_;
    }
    [[nodiscard]] std::uint64_t blasRefits() const noexcept {
        return blasRefits_;
    }
    [[nodiscard]] std::uint64_t tlasRebuilds() const noexcept {
        return tlasRebuilds_;
    }
    [[nodiscard]] std::uint64_t tlasUpdates() const noexcept {
        return tlasUpdates_;
    }

  private:
    struct MeshState {
        std::uint64_t topologyGeneration{};
        std::uint64_t deformVersion{};
        BufferHandle vertexBuffer{};
        AccelerationStructureHandle blas{};
        bool built{false};
    };

    IAccelerationBackend* backend_{nullptr};
    std::unordered_map<std::uint32_t, MeshState> meshes_;
    AccelerationStructureHandle tlas_{};
    bool tlasBuilt_{false};
    bool blasChangedSinceTlas_{false};
    std::uint64_t cachedWorldGeneration_{0};
    bool hasCachedWorld_{false};
    std::size_t tlasInstanceCount_{0};
    std::vector<TlasInstanceDesc> tlasScratch_;
    std::uint64_t blasRebuilds_{0};
    std::uint64_t blasRefits_{0};
    std::uint64_t tlasRebuilds_{0};
    std::uint64_t tlasUpdates_{0};
};

[[nodiscard]] const char* toString(BlasAction action) noexcept;
[[nodiscard]] const char* toString(TlasAction action) noexcept;

} // namespace dayo::graphics
