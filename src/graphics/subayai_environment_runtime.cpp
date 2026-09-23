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
           left.sphericalHarmonics == right.sphericalHarmonics && left.skywalkerVersion == right.skywalkerVersion &&
           left.skybox == right.skybox;
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
    const auto slot = currentSlot();
    if (!result.cubemap.valid() && !result.prefiltered.valid()) {
        if (descriptorSets_[slot].valid()) {
            try {
                device.destroyDescriptorSetEx(descriptorSets_[slot]);
            } catch (const std::exception& exception) {
                setError(error, exception.what());
                return false;
            }
            descriptorSets_[slot] = {};
        }
        if (sphericalHarmonicsBuffers_[slot].valid()) {
            try {
                device.destroyBufferEx(sphericalHarmonicsBuffers_[slot]);
            } catch (const std::exception& exception) {
                setError(error, exception.what());
                return false;
            }
            sphericalHarmonicsBuffers_[slot] = {};
        }
        bound_[slot] = {};
        current_ = {};
        return true;
    }
    if (!result.cubemap.valid() || !result.prefiltered.valid()) {
        setError(error, "Subayai environment requires both cubemap and prefiltered handles");
        return false;
    }
    if (sameResult(bound_[slot], result) && descriptorSets_[slot].valid() && sphericalHarmonicsBuffers_[slot].valid()) {
        current_ = result;
        return true;
    }
    return bind(result, error);
}

bool SubayaiEnvironmentRuntime::bind(const EnvironmentGpuResult& result, std::string* error) {
    try {
        const auto slot = currentSlot();
        if (!sphericalHarmonicsBuffers_[slot].valid()) {
            sphericalHarmonicsBuffers_[slot] = device_->createBufferEx({
                .size = result.sphericalHarmonics.size() * sizeof(float),
                .usage = ResourceUsage::storageRead | ResourceUsage::hostRead,
                .cpuVisible = true,
                .lifetime = ResourceLifetime::persistent,
            });
            if (!sphericalHarmonicsBuffers_[slot].valid())
                throw std::runtime_error("Subayai environment SH buffer is invalid");
        }
        device_->uploadBufferEx(sphericalHarmonicsBuffers_[slot],
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
                                .buffer = sphericalHarmonicsBuffers_[slot]}};
        if (descriptorSets_[slot].valid()) {
            device_->updateDescriptorSetEx(descriptorSets_[slot], bindings);
        } else {
            descriptorSets_[slot] = device_->allocateDescriptorSetEx(layout_, bindings);
            if (!descriptorSets_[slot].valid())
                throw std::runtime_error("Subayai environment descriptor set is invalid");
        }
        bound_[slot] = result;
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
        for (const auto descriptorSet : descriptorSets_) {
            if (descriptorSet.valid()) {
                try {
                    device->destroyDescriptorSetEx(descriptorSet);
                } catch (...) {
                }
            }
        }
        for (const auto buffer : sphericalHarmonicsBuffers_) {
            if (buffer.valid()) {
                try {
                    device->destroyBufferEx(buffer);
                } catch (...) {
                }
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
    descriptorSets_.fill({});
    sphericalHarmonicsBuffers_.fill({});
    bound_.fill({});
    current_ = {};
}

} // namespace dayo::graphics
