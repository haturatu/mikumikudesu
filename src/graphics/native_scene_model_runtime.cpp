#include "graphics/native_scene_model_runtime.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

[[nodiscard]] std::size_t allocationSize(std::size_t byteSize) {
    return std::max<std::size_t>(1, byteSize);
}

template <typename T>
handles::BufferHandle upload(Device& device, std::span<const T> values, ResourceUsage usage, std::string_view name) {
    const auto byteSize = values.size_bytes();
    const auto handle = device.createBufferEx({.size = allocationSize(byteSize),
                                               .usage = usage | ResourceUsage::transferDst,
                                               .cpuVisible = false,
                                               .lifetime = ResourceLifetime::persistent});
    if (!handle.valid())
        throw std::runtime_error("native scene buffer allocation returned an invalid handle: " + std::string(name));
    if (!values.empty())
        device.uploadBufferEx(handle, std::as_bytes(values), 0);
    return handle;
}

void destroy(Device& device, std::vector<handles::BufferHandle>& handles) noexcept {
    for (const auto handle : handles) {
        if (!handle.valid())
            continue;
        try {
            device.destroyBufferEx(handle);
        } catch (...) {
        }
    }
    handles.clear();
}

} // namespace

NativeSceneModelRuntime::~NativeSceneModelRuntime() {
    reset();
}

bool NativeSceneModelRuntime::sync(Device& device, std::span<const NativeSceneModelData> models,
                                   std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    if (models.empty()) {
        setError(error, "native scene model runtime requires at least one model");
        return false;
    }
    try {
        device_ = &device;
        vertices_.reserve(models.size());
        indices_.reserve(models.size());
        materials_.reserve(models.size());
        faces_.reserve(models.size());
        materialFaces_.reserve(models.size());
        faceWalkers_.reserve(models.size());

        for (const auto& model : models) {
            vertices_.push_back(upload(device, std::span<const NativeSceneVertex>(model.vertices),
                                       ResourceUsage::storageRead | ResourceUsage::vertexRead |
                                           ResourceUsage::asBuildRead | ResourceUsage::rayTracingRead,
                                       "vertices"));
            indices_.push_back(upload(device, std::span<const std::uint32_t>(model.indices),
                                      ResourceUsage::storageRead | ResourceUsage::indexRead | ResourceUsage::asBuildRead |
                                          ResourceUsage::rayTracingRead,
                                      "indices"));
            materials_.push_back(upload(device, std::span<const NativeSceneMaterial>(model.materials),
                                        ResourceUsage::storageRead | ResourceUsage::rayTracingRead, "materials"));
            faces_.push_back(upload(device, std::span<const std::uint32_t>(model.faces),
                                    ResourceUsage::storageRead | ResourceUsage::rayTracingRead, "faces"));
            materialFaces_.push_back(upload(device, std::span<const NativeSceneMaterialFace>(model.materialFaces),
                                            ResourceUsage::storageRead | ResourceUsage::rayTracingRead, "material faces"));
            faceWalkers_.push_back(upload(device, std::span<const NativeSceneWalkerAlias>(model.faceWalker),
                                          ResourceUsage::storageRead | ResourceUsage::rayTracingRead, "face walkers"));
        }
    } catch (const std::exception& exception) {
        setError(error, std::string("native scene model upload failed: ") + exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "native scene model upload failed");
        reset();
        return false;
    }
    return true;
}

NativeSceneDescriptorCounts NativeSceneModelRuntime::descriptorCounts() const noexcept {
    const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(modelCount(), std::numeric_limits<std::uint32_t>::max()));
    return {.textures = 1,
            .vertexBuffers = count,
            .indexBuffers = count,
            .materials = count,
            .faces = count,
            .materialFaces = count,
            .faceWalkers = count,
            .previousVertices = count,
            .rawVertices = count};
}

NativeSceneResourceBindings NativeSceneModelRuntime::bindings() const noexcept {
    NativeSceneResourceBindings result;
    result.vertexBuffers = vertices_;
    result.indexBuffers = indices_;
    result.materials = materials_;
    result.faces = faces_;
    result.materialFaces = materialFaces_;
    result.faceWalkers = faceWalkers_;
    // Until the deform bridge owns a second canonical vertex allocation, the
    // initial raw vertex stream is also the previous-frame stream.
    result.previousVertices = vertices_;
    result.rawVertices = vertices_;
    return result;
}

void NativeSceneModelRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        destroy(*device, vertices_);
        destroy(*device, indices_);
        destroy(*device, materials_);
        destroy(*device, faces_);
        destroy(*device, materialFaces_);
        destroy(*device, faceWalkers_);
    } else {
        vertices_.clear();
        indices_.clear();
        materials_.clear();
        faces_.clear();
        materialFaces_.clear();
        faceWalkers_.clear();
    }
    device_ = nullptr;
}

} // namespace dayo::graphics
