#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Forward declaration keeps this header light; the implementation includes
// core/scene.hpp for DirtyFlag and Scene integration.
namespace dayo::core {
enum class DirtyFlag : std::uint32_t;
class Scene;
} // namespace dayo::core

namespace dayo::graphics {

// BDPT progressive accumulation host state.
// DirtyFlag/accumulatedSamples drive the sample index: any dirty flag resets
// to sampleIndex 0 and requests a clear, otherwise the index increments.
// Spectral/BlackBody LUTs and the 8-slot VolumeResourceSet persist across
// frames (created once, never regenerated per frame).
class BdptAccumulation {
  public:
    static constexpr std::size_t kVolumeSlots = 8;

    struct VolumeResource {
        std::uint64_t handle{0};
        bool valid{false};
    };

    BdptAccumulation() = default;

    // Idempotent: first call allocates LUTs + 8 volumes, later calls reuse.
    void ensurePersistent();

    // DirtyFlag driven step. Returns true when the frame must clear.
    bool beginFrame(core::DirtyFlag dirty) noexcept;
    // Scene integrated step using both DirtyFlag and accumulatedSamples.
    bool syncScene(core::Scene& scene);

    [[nodiscard]] std::uint32_t sampleIndex() const noexcept {
        return sampleIndex_;
    }
    [[nodiscard]] bool needsClear() const noexcept {
        return needsClear_;
    }
    [[nodiscard]] bool persistentReady() const noexcept {
        return persistentReady_;
    }
    [[nodiscard]] bool spectralLutReady() const noexcept {
        return spectralReady_;
    }
    [[nodiscard]] bool blackbodyLutReady() const noexcept {
        return blackbodyReady_;
    }
    [[nodiscard]] std::size_t volumeCount() const noexcept {
        return volumes_.size();
    }
    [[nodiscard]] const VolumeResource& volume(std::size_t index) const {
        return volumes_.at(index);
    }
    [[nodiscard]] const std::vector<float>& spectralLut() const noexcept {
        return spectralLut_;
    }
    [[nodiscard]] const std::vector<float>& blackbodyLut() const noexcept {
        return blackbodyLut_;
    }
    [[nodiscard]] std::uint64_t generationCount() const noexcept {
        return generations_;
    }

  private:
    std::vector<float> spectralLut_;
    std::vector<float> blackbodyLut_;
    std::array<VolumeResource, kVolumeSlots> volumes_{};
    std::uint32_t sampleIndex_{0};
    bool needsClear_{true};
    bool persistentReady_{false};
    bool spectralReady_{false};
    bool blackbodyReady_{false};
    std::uint64_t generations_{0};
};

} // namespace dayo::graphics
