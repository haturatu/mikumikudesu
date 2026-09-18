#pragma once

#include "graphics/subayai_environment.hpp"

#include <array>
#include <span>
#include <string>

namespace dayo::graphics {

// Owns the descriptor-facing part of the native environment ABI. The
// environment backend owns cubemap generation; this object keeps the current
// cubemap, prefiltered cubemap, and SH coefficients available to Subayai FX
// passes without changing Preview descriptors.
class SubayaiEnvironmentRuntime {
  public:
    SubayaiEnvironmentRuntime() = default;
    ~SubayaiEnvironmentRuntime();

    SubayaiEnvironmentRuntime(const SubayaiEnvironmentRuntime&) = delete;
    SubayaiEnvironmentRuntime& operator=(const SubayaiEnvironmentRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, std::string* error = nullptr);
    [[nodiscard]] bool sync(Device& device, const EnvironmentGpuResult& result, std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && layout_.valid();
    }
    [[nodiscard]] handles::DescriptorSetLayoutHandle layout() const noexcept {
        return layout_;
    }
    [[nodiscard]] handles::DescriptorSetHandle descriptorSet() const noexcept {
        return device_ == nullptr ? handles::DescriptorSetHandle{}
                                   : descriptorSets_[device_->currentFrameSlot() % kNativeFramesInFlight];
    }
    [[nodiscard]] handles::BufferHandle sphericalHarmonicsBuffer() const noexcept {
        return device_ == nullptr ? handles::BufferHandle{}
                                   : sphericalHarmonicsBuffers_[device_->currentFrameSlot() % kNativeFramesInFlight];
    }

  private:
    [[nodiscard]] std::size_t currentSlot() const noexcept {
        return device_ == nullptr ? 0 : device_->currentFrameSlot() % kNativeFramesInFlight;
    }

    [[nodiscard]] bool bind(const EnvironmentGpuResult& result, std::string* error);

    Device* device_{};
    handles::DescriptorSetLayoutHandle layout_{};
    std::array<handles::DescriptorSetHandle, kNativeFramesInFlight> descriptorSets_{};
    std::array<handles::BufferHandle, kNativeFramesInFlight> sphericalHarmonicsBuffers_{};
    std::array<EnvironmentGpuResult, kNativeFramesInFlight> bound_{};
    EnvironmentGpuResult current_{};
};

[[nodiscard]] DescriptorSetLayoutDesc subayaiEnvironmentBindingLayout() noexcept;

} // namespace dayo::graphics
