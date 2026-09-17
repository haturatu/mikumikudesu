#include "graphics/subayai_material_runtime.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {

namespace {

[[nodiscard]] bool sameMaterials(std::span<const SubayaiMaterialGpu> left,
                                 std::span<const SubayaiMaterialGpu> right) noexcept {
    return left.size() == right.size() &&
           (left.empty() || std::memcmp(left.data(), right.data(), left.size_bytes()) == 0);
}

} // namespace

SubayaiMaterialGpuRuntime::~SubayaiMaterialGpuRuntime() {
    reset();
}

bool SubayaiMaterialGpuRuntime::sync(Device& device, std::span<const core::MaterialParameterBlock> materials,
                                     std::string* error) {
    if (error != nullptr)
        error->clear();
    if (materials.empty()) {
        if (device_ == &device)
            reset();
        else if (device_ != nullptr) {
            if (error != nullptr)
                *error = "Subayai material buffer belongs to a different device";
            return false;
        }
        return true;
    }
    if (materials.size() > std::numeric_limits<std::size_t>::max() / sizeof(SubayaiMaterialGpu)) {
        if (error != nullptr)
            *error = "Subayai material buffer size overflow";
        return false;
    }
    if (device_ != nullptr && device_ != &device) {
        if (error != nullptr)
            *error = "Subayai material buffer belongs to a different device";
        return false;
    }

    std::vector<SubayaiMaterialGpu> linked;
    linked.reserve(materials.size());
    for (const auto& material : materials)
        linked.push_back(linkSubayaiMaterial(material));

    try {
        const auto oldMaterials = std::span<const SubayaiMaterialGpu>(materials_);
        const bool sameShape = device_ == &device && ready() && materials_.size() == linked.size();
        const bool changed = !sameShape || !sameMaterials(oldMaterials, linked);
        if (sameShape) {
            materials_ = std::move(linked);
            const auto slot = device.currentFrameSlot() % kNativeFramesInFlight;
            if (!changed && uploaded_[slot])
                return true;
            device.uploadBufferEx(buffers_[slot], std::as_bytes(std::span<const SubayaiMaterialGpu>(materials_)), 0);
            uploaded_[slot] = true;
            return true;
        }

        reset();
        device_ = &device;
        for (auto& buffer : buffers_) {
            buffer = device.createBufferEx({
                .size = linked.size() * sizeof(SubayaiMaterialGpu),
                .usage = ResourceUsage::storageRead | ResourceUsage::hostRead,
                .cpuVisible = true,
                .lifetime = ResourceLifetime::persistent,
            });
            if (!buffer.valid())
                throw std::runtime_error("Subayai material buffer is invalid");
        }
        materials_ = std::move(linked);
        for (std::size_t slot = 0; slot < kNativeFramesInFlight; ++slot) {
            device.uploadBufferEx(buffers_[slot], std::as_bytes(std::span<const SubayaiMaterialGpu>(materials_)), 0);
            uploaded_[slot] = true;
        }
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = std::string("Subayai material GPU upload failed: ") + exception.what();
        reset();
        return false;
    } catch (...) {
        if (error != nullptr)
            *error = "Subayai material GPU upload failed";
        reset();
        return false;
    }
    return true;
}

void SubayaiMaterialGpuRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        for (const auto buffer : buffers_) {
            if (buffer.valid()) {
                try {
                    device->destroyBufferEx(buffer);
                } catch (...) {
                }
            }
        }
    }
    device_ = nullptr;
    buffers_.fill({});
    uploaded_.fill(false);
    materials_.clear();
}

} // namespace dayo::graphics
