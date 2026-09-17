#pragma once

#include "graphics/native_scene_bindings.hpp"

#include <array>
#include <span>
#include <string>

namespace dayo::graphics {

// Owns the descriptor layouts and complete descriptor sets for the fixed
// native scene ABI. Each bind call supplies every descriptor in one set,
// which keeps partially updated scene arrays from reaching a command list.
class NativeSceneBindingRuntime {
  public:
    NativeSceneBindingRuntime() = default;
    ~NativeSceneBindingRuntime();

    NativeSceneBindingRuntime(const NativeSceneBindingRuntime&) = delete;
    NativeSceneBindingRuntime& operator=(const NativeSceneBindingRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, const NativeSceneDescriptorCounts& counts = {},
                                  std::string* error = nullptr);
    [[nodiscard]] bool bind(NativeSceneDescriptorSet set, std::span<const DescriptorBindingEx> bindings,
                            std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr;
    }
    [[nodiscard]] handles::DescriptorSetLayoutHandle layout(NativeSceneDescriptorSet set) const noexcept;
    [[nodiscard]] handles::DescriptorSetHandle descriptorSet(NativeSceneDescriptorSet set) const noexcept;
    [[nodiscard]] std::span<const handles::DescriptorSetLayoutHandle> layouts() const noexcept {
        return layouts_;
    }
    [[nodiscard]] std::span<const handles::DescriptorSetHandle> descriptorSets() const noexcept {
        return descriptorSets_;
    }
    [[nodiscard]] const NativeSceneDescriptorCounts& counts() const noexcept {
        return counts_;
    }

  private:
    [[nodiscard]] bool validateBindings(NativeSceneDescriptorSet set, std::span<const DescriptorBindingEx> bindings,
                                        std::string* error) const;

    Device* device_{};
    NativeSceneDescriptorCounts counts_{};
    std::array<handles::DescriptorSetLayoutHandle, kNativeSceneDescriptorSetCount> layouts_{};
    std::array<handles::DescriptorSetHandle, kNativeSceneDescriptorSetCount> descriptorSets_{};
};

} // namespace dayo::graphics
