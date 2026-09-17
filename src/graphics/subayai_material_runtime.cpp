#include "graphics/subayai_material_runtime.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {

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
        if (device_ == &device && buffer_.valid() && materials_.size() == linked.size()) {
            device.uploadBufferEx(buffer_, std::as_bytes(std::span<const SubayaiMaterialGpu>(linked)), 0);
            materials_ = std::move(linked);
            return true;
        }
        reset();
        device_ = &device;
        buffer_ = device.createBufferEx({
            .size = linked.size() * sizeof(SubayaiMaterialGpu),
            .usage = ResourceUsage::storageRead | ResourceUsage::transferDst,
            .cpuVisible = false,
            .lifetime = ResourceLifetime::persistent,
        });
        device.uploadBufferEx(buffer_, std::as_bytes(std::span<const SubayaiMaterialGpu>(linked)), 0);
        materials_ = std::move(linked);
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
    if (device != nullptr && buffer_.valid()) {
        try {
            device->waitIdle();
            device->destroyBufferEx(buffer_);
        } catch (...) {
        }
    }
    device_ = nullptr;
    buffer_ = {};
    materials_.clear();
}

} // namespace dayo::graphics
