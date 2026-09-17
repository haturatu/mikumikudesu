#include "graphics/subayai_environment_runtime.hpp"

#include "graphics/native_scene_bindings.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {
namespace {

[[nodiscard]] ShaderStageMask nativeEnvironmentStages() noexcept {
    return ShaderStageMask::compute | ShaderStageMask::fragment | ShaderStageMask::rayGeneration |
           ShaderStageMask::miss | ShaderStageMask::closestHit | ShaderStageMask::anyHit |
           ShaderStageMask::intersection | ShaderStageMask::callable;
}

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

[[nodiscard]] bool sameResult(const EnvironmentGpuResult& left, const EnvironmentGpuResult& right) noexcept {
    return left.cubemap == right.cubemap && left.prefiltered == right.prefiltered &&
           left.sphericalHarmonics == right.sphericalHarmonics && left.skywalkerVersion == right.skywalkerVersion;
}

} // namespace

DescriptorSetLayoutDesc subayaiEnvironmentBindingLayout() noexcept {
    return {.bindings = {{nativeSceneBinding(NativeSceneRegisterClass::sampled, 0), DescriptorKind::sampledImage, 1,
                          nativeEnvironmentStages()},
                         {nativeSceneBinding(NativeSceneRegisterClass::sampled, 1), DescriptorKind::sampledImage, 1,
                          nativeEnvironmentStages()},
                         {nativeSceneBinding(NativeSceneRegisterClass::sampled, 2), DescriptorKind::storageBuffer, 1,
                          nativeEnvironmentStages()}}};
}

SubayaiEnvironmentRuntime::~SubayaiEnvironmentRuntime() {
    reset();
}

bool SubayaiEnvironmentRuntime::initialize(Device& device, std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    device_ = &device;
    try {
        layout_ = device.createDescriptorSetLayoutEx(subayaiEnvironmentBindingLayout());
        if (!layout_.valid())
            throw std::runtime_error("Subayai environment descriptor layout is invalid");
    } catch (const std::exception& exception) {
        setError(error, std::string("Subayai environment initialization failed: ") + exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "Subayai environment initialization failed");
        reset();
        return false;
    }
    return true;
}

bool SubayaiEnvironmentRuntime::sync(Device& device, const EnvironmentGpuResult& result, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (device_ == nullptr && !initialize(device, error))
        return false;
    if (device_ != &device) {
        setError(error, "Subayai environment resources belong to a different device");
        return false;
    }
    if (!result.cubemap.valid() && !result.prefiltered.valid()) {
        if (descriptorSet_.valid()) {
            try {
                device.destroyDescriptorSetEx(descriptorSet_);
            } catch (const std::exception& exception) {
                setError(error, exception.what());
                return false;
            }
            descriptorSet_ = {};
        }
        if (sphericalHarmonicsBuffer_.valid()) {
            try {
                device.destroyBufferEx(sphericalHarmonicsBuffer_);
            } catch (const std::exception& exception) {
                setError(error, exception.what());
                return false;
            }
            sphericalHarmonicsBuffer_ = {};
        }
        current_ = {};
        return true;
    }
    if (!result.cubemap.valid() || !result.prefiltered.valid()) {
        setError(error, "Subayai environment requires both cubemap and prefiltered handles");
        return false;
    }
    if (sameResult(current_, result) && descriptorSet_.valid() && sphericalHarmonicsBuffer_.valid())
        return true;
    return bind(result, error);
}

bool SubayaiEnvironmentRuntime::bind(const EnvironmentGpuResult& result, std::string* error) {
    try {
        if (!sphericalHarmonicsBuffer_.valid()) {
            sphericalHarmonicsBuffer_ = device_->createBufferEx({
                .size = result.sphericalHarmonics.size() * sizeof(float),
                .usage = ResourceUsage::storageRead | ResourceUsage::transferDst,
                .cpuVisible = false,
                .lifetime = ResourceLifetime::persistent,
            });
            if (!sphericalHarmonicsBuffer_.valid())
                throw std::runtime_error("Subayai environment SH buffer is invalid");
        }
        device_->uploadBufferEx(sphericalHarmonicsBuffer_,
                                std::as_bytes(std::span<const float>(result.sphericalHarmonics)), 0);
        const std::array<DescriptorBindingEx, 3> bindings{
            DescriptorBindingEx{.slot = nativeSceneBinding(NativeSceneRegisterClass::sampled, 0),
                                .arrayElement = 0,
                                .texture = result.cubemap},
            DescriptorBindingEx{.slot = nativeSceneBinding(NativeSceneRegisterClass::sampled, 1),
                                .arrayElement = 0,
                                .texture = result.prefiltered},
            DescriptorBindingEx{.slot = nativeSceneBinding(NativeSceneRegisterClass::sampled, 2),
                                .arrayElement = 0,
                                .buffer = sphericalHarmonicsBuffer_}};
        if (descriptorSet_.valid()) {
            device_->updateDescriptorSetEx(descriptorSet_, bindings);
        } else {
            descriptorSet_ = device_->allocateDescriptorSetEx(layout_, bindings);
            if (!descriptorSet_.valid())
                throw std::runtime_error("Subayai environment descriptor set is invalid");
        }
        current_ = result;
    } catch (const std::exception& exception) {
        setError(error, std::string("Subayai environment binding failed: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "Subayai environment binding failed");
        return false;
    }
    return true;
}

void SubayaiEnvironmentRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        if (descriptorSet_.valid()) {
            try {
                device->destroyDescriptorSetEx(descriptorSet_);
            } catch (...) {
            }
        }
        if (sphericalHarmonicsBuffer_.valid()) {
            try {
                device->destroyBufferEx(sphericalHarmonicsBuffer_);
            } catch (...) {
            }
        }
        if (layout_.valid()) {
            try {
                device->destroyDescriptorSetLayoutEx(layout_);
            } catch (...) {
            }
        }
    }
    device_ = nullptr;
    layout_ = {};
    descriptorSet_ = {};
    sphericalHarmonicsBuffer_ = {};
    current_ = {};
}

} // namespace dayo::graphics
