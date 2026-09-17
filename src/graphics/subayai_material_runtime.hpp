#pragma once

#include "graphics/device.hpp"
#include "graphics/subayai_material_gpu.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace dayo::graphics {

// Owns the persistent storage buffer that carries SubayaiMaterialGpu values
// to native shaders. The CPU mirror remains available for diagnostics and
// backend-neutral tests, while the typed handle is what a pass binds.
class SubayaiMaterialGpuRuntime {
  public:
    SubayaiMaterialGpuRuntime() = default;
    ~SubayaiMaterialGpuRuntime();

    SubayaiMaterialGpuRuntime(const SubayaiMaterialGpuRuntime&) = delete;
    SubayaiMaterialGpuRuntime& operator=(const SubayaiMaterialGpuRuntime&) = delete;

    [[nodiscard]] bool sync(Device& device, std::span<const core::MaterialParameterBlock> materials,
                            std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && !materials_.empty() &&
               std::all_of(buffers_.begin(), buffers_.end(), [](const auto buffer) { return buffer.valid(); });
    }
    [[nodiscard]] handles::BufferHandle buffer() const noexcept {
        return device_ == nullptr ? handles::BufferHandle{}
                                   : buffers_[device_->currentFrameSlot() % kNativeFramesInFlight];
    }
    [[nodiscard]] std::size_t count() const noexcept {
        return materials_.size();
    }
    [[nodiscard]] std::span<const SubayaiMaterialGpu> materials() const noexcept {
        return materials_;
    }

  private:
    Device* device_{};
    std::array<handles::BufferHandle, kNativeFramesInFlight> buffers_{};
    std::array<bool, kNativeFramesInFlight> uploaded_{};
    std::vector<SubayaiMaterialGpu> materials_;
};

} // namespace dayo::graphics
