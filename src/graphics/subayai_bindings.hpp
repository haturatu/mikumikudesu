#pragma once

#include "graphics/device.hpp"

#include <string>

namespace dayo::graphics {

struct SubayaiBindingLayouts {
    handles::DescriptorSetLayoutHandle material{};
    handles::DescriptorSetLayoutHandle lightSampling{};

    [[nodiscard]] bool valid() const noexcept {
        return material.valid() && lightSampling.valid();
    }
};

// Both bindings are storage buffers so native compute, fragment ray-query,
// and full ray-tracing stages can share the same ABI. The Preview descriptor
// layouts remain separate and are not changed by this contract.
[[nodiscard]] DescriptorSetLayoutDesc subayaiMaterialBindingLayout();
[[nodiscard]] DescriptorSetLayoutDesc subayaiLightSamplingBindingLayout();

// Owns the typed descriptor layouts and the current descriptor sets for the
// native Subayai material/light resources. Buffer runtimes own the buffers;
// this object owns only the binding state that points at them.
class SubayaiBindingRuntime {
  public:
    SubayaiBindingRuntime() = default;
    ~SubayaiBindingRuntime();

    SubayaiBindingRuntime(const SubayaiBindingRuntime&) = delete;
    SubayaiBindingRuntime& operator=(const SubayaiBindingRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, std::string* error = nullptr);
    [[nodiscard]] bool bindMaterial(handles::BufferHandle buffer, std::string* error = nullptr);
    [[nodiscard]] bool bindLightSampling(handles::BufferHandle buffer, std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] const SubayaiBindingLayouts& layouts() const noexcept {
        return layouts_;
    }
    [[nodiscard]] handles::DescriptorSetHandle materialSet() const noexcept {
        return materialSet_;
    }
    [[nodiscard]] handles::DescriptorSetHandle lightSamplingSet() const noexcept {
        return lightSamplingSet_;
    }

  private:
    [[nodiscard]] bool bindBuffer(handles::DescriptorSetLayoutHandle layout,
                                  handles::DescriptorSetHandle& set, handles::BufferHandle buffer,
                                  const char* label, std::string* error);

    Device* device_{};
    SubayaiBindingLayouts layouts_{};
    handles::DescriptorSetHandle materialSet_{};
    handles::DescriptorSetHandle lightSamplingSet_{};
};

} // namespace dayo::graphics
