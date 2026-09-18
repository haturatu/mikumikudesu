#include "graphics/native_scene_binding_runtime.hpp"

#include <algorithm>
#include <exception>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

[[nodiscard]] std::size_t setIndex(NativeSceneDescriptorSet set) noexcept {
    return static_cast<std::size_t>(set);
}

[[nodiscard]] bool hasResource(const DescriptorBindingEx& binding) noexcept {
    const auto count = static_cast<unsigned>(binding.buffer.valid()) + static_cast<unsigned>(binding.texture.valid()) +
                       static_cast<unsigned>(binding.sampler.valid()) +
                       static_cast<unsigned>(binding.accelerationStructure.valid());
    return count == 1;
}

} // namespace

NativeSceneBindingRuntime::~NativeSceneBindingRuntime() {
    reset();
}

bool NativeSceneBindingRuntime::initialize(Device& device, const NativeSceneDescriptorCounts& counts,
                                           std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    counts_ = counts;
    device_ = &device;
    try {
        const auto descriptions = nativeSceneDescriptorLayouts(counts_);
        for (std::size_t index = 0; index < descriptions.size(); ++index) {
            layouts_[index] = device.createDescriptorSetLayoutEx(descriptions[index]);
            if (!layouts_[index].valid())
                throw std::runtime_error("native scene descriptor layout is invalid");
        }
    } catch (const std::exception& exception) {
        setError(error, std::string("native scene descriptor initialization failed: ") + exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "native scene descriptor initialization failed");
        reset();
        return false;
    }
    return true;
}

bool NativeSceneBindingRuntime::validateBindings(NativeSceneDescriptorSet set,
                                                 std::span<const DescriptorBindingEx> bindings,
                                                 std::string* error) const {
    if (error != nullptr)
        error->clear();
    const auto index = setIndex(set);
    if (device_ == nullptr || index >= layouts_.size() || !layouts_[index].valid()) {
        setError(error, "native scene descriptor set is unavailable");
        return false;
    }
    const auto layout = nativeSceneDescriptorLayout(set, counts_);
    std::size_t expectedCount = 0;
    for (const auto& binding : layout.bindings)
        expectedCount += binding.count;
    if (bindings.size() != expectedCount) {
        setError(error, "native scene descriptor set must provide every array element");
        return false;
    }
    for (const auto& binding : layout.bindings) {
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto& candidate) {
                return candidate.slot == binding.binding && candidate.arrayElement == element;
            });
            if (found == bindings.end() || !hasResource(*found)) {
                setError(error, "native scene descriptor set has a missing or invalid binding");
                return false;
            }
        }
    }
    return true;
}

bool NativeSceneBindingRuntime::bind(NativeSceneDescriptorSet set, std::span<const DescriptorBindingEx> bindings,
                                     std::string* error) {
    if (!validateBindings(set, bindings, error))
        return false;
    const auto index = setIndex(set);
    try {
        auto& descriptorSet = descriptorSets_[currentSlot()][index];
        if (descriptorSet.valid()) {
            device_->updateDescriptorSetEx(descriptorSet, bindings);
        } else {
            descriptorSet = device_->allocateDescriptorSetEx(layouts_[index], bindings);
            if (!descriptorSet.valid())
                throw std::runtime_error("native scene descriptor set allocation returned an invalid handle");
        }
    } catch (const std::exception& exception) {
        setError(error, std::string("native scene descriptor binding failed: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "native scene descriptor binding failed");
        return false;
    }
    return true;
}

handles::DescriptorSetLayoutHandle NativeSceneBindingRuntime::layout(NativeSceneDescriptorSet set) const noexcept {
    const auto index = setIndex(set);
    return index < layouts_.size() ? layouts_[index] : handles::DescriptorSetLayoutHandle{};
}

handles::DescriptorSetHandle NativeSceneBindingRuntime::descriptorSet(NativeSceneDescriptorSet set) const noexcept {
    const auto index = setIndex(set);
    return index < kNativeSceneDescriptorSetCount ? descriptorSets_[currentSlot()][index]
                                                  : handles::DescriptorSetHandle{};
}

void NativeSceneBindingRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        for (auto& frameSets : descriptorSets_) {
            for (const auto& set : std::ranges::reverse_view(frameSets)) {
                if (!set.valid())
                    continue;
                try {
                    device->destroyDescriptorSetEx(set);
                } catch (...) {
                }
            }
        }
        for (const auto& layout : std::ranges::reverse_view(layouts_)) {
            if (!layout.valid())
                continue;
            try {
                device->destroyDescriptorSetLayoutEx(layout);
            } catch (...) {
            }
        }
    }
    device_ = nullptr;
    counts_ = {};
    layouts_ = {};
    descriptorSets_ = {};
}

} // namespace dayo::graphics
