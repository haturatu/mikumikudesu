#include "graphics/subayai_bindings.hpp"

#include "graphics/native_scene_bindings.hpp"

#include <array>
#include <exception>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {
namespace {

[[nodiscard]] ShaderStageMask nativeSubayaiStages() noexcept {
    return ShaderStageMask::vertex | ShaderStageMask::fragment | ShaderStageMask::compute |
           ShaderStageMask::rayGeneration | ShaderStageMask::miss | ShaderStageMask::closestHit |
           ShaderStageMask::anyHit | ShaderStageMask::intersection | ShaderStageMask::callable;
}

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

} // namespace

DescriptorSetLayoutDesc subayaiMaterialBindingLayout() {
    return {.bindings = {{nativeSceneBinding(NativeSceneRegisterClass::sampled, 0), DescriptorKind::storageBuffer, 1,
                          nativeSubayaiStages()}}};
}

DescriptorSetLayoutDesc subayaiLightSamplingBindingLayout() {
    return {.bindings = {{nativeSceneBinding(NativeSceneRegisterClass::sampled, 0), DescriptorKind::storageBuffer, 1,
                          nativeSubayaiStages()}}};
}

SubayaiBindingRuntime::~SubayaiBindingRuntime() {
    reset();
}

bool SubayaiBindingRuntime::initialize(Device& device, std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    device_ = &device;
    try {
        layouts_.material = device.createDescriptorSetLayoutEx(subayaiMaterialBindingLayout());
        layouts_.lightSampling = device.createDescriptorSetLayoutEx(subayaiLightSamplingBindingLayout());
        if (!layouts_.valid())
            throw std::runtime_error("Subayai descriptor layout creation returned an invalid handle");
    } catch (const std::exception& exception) {
        setError(error, std::string("Subayai descriptor layout initialization failed: ") + exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "Subayai descriptor layout initialization failed");
        reset();
        return false;
    }
    return true;
}

bool SubayaiBindingRuntime::bindBuffer(handles::DescriptorSetLayoutHandle layout, handles::DescriptorSetHandle& set,
                                       handles::BufferHandle buffer, const char* label, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (device_ == nullptr || !layout.valid()) {
        setError(error, std::string("Subayai ") + label + " descriptor layout is unavailable");
        return false;
    }
    try {
        if (!buffer.valid()) {
            if (set.valid()) {
                device_->destroyDescriptorSetEx(set);
                set = {};
            }
            return true;
        }
        const std::array<DescriptorBindingEx, 1> bindings{DescriptorBindingEx{
            .slot = nativeSceneBinding(NativeSceneRegisterClass::sampled, 0), .arrayElement = 0, .buffer = buffer}};
        if (set.valid()) {
            device_->updateDescriptorSetEx(set, bindings);
        } else {
            set = device_->allocateDescriptorSetEx(layout, bindings);
            if (!set.valid())
                throw std::runtime_error(std::string("Subayai ") + label + " descriptor allocation returned invalid");
        }
    } catch (const std::exception& exception) {
        setError(error, std::string("Subayai ") + label + " descriptor binding failed: " + exception.what());
        return false;
    } catch (...) {
        setError(error, std::string("Subayai ") + label + " descriptor binding failed");
        return false;
    }
    return true;
}

bool SubayaiBindingRuntime::bindMaterial(handles::BufferHandle buffer, std::string* error) {
    return bindBuffer(layouts_.material, materialSets_[currentSlot()], buffer, "material", error);
}

bool SubayaiBindingRuntime::bindLightSampling(handles::BufferHandle buffer, std::string* error) {
    return bindBuffer(layouts_.lightSampling, lightSamplingSets_[currentSlot()], buffer, "light sampling", error);
}

void SubayaiBindingRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        for (const auto set : materialSets_) {
            if (set.valid()) {
                try {
                    device->destroyDescriptorSetEx(set);
                } catch (...) {
                }
            }
        }
        for (const auto set : lightSamplingSets_) {
            if (set.valid()) {
                try {
                    device->destroyDescriptorSetEx(set);
                } catch (...) {
                }
            }
        }
        if (layouts_.lightSampling.valid()) {
            try {
                device->destroyDescriptorSetLayoutEx(layouts_.lightSampling);
            } catch (...) {
            }
        }
        if (layouts_.material.valid()) {
            try {
                device->destroyDescriptorSetLayoutEx(layouts_.material);
            } catch (...) {
            }
        }
    }
    device_ = nullptr;
    layouts_ = {};
    materialSets_.fill({});
    lightSamplingSets_.fill({});
}

} // namespace dayo::graphics
