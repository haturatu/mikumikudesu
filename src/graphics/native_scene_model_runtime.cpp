#include "graphics/native_scene_model_runtime.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

template <typename T> [[nodiscard]] std::size_t byteSize(const std::vector<T>& values) noexcept {
    return values.size() * sizeof(T);
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

template <typename T>
handles::BufferHandle createStaging(Device& device, std::span<const T> values, std::string_view name) {
    const auto handle = device.createBufferEx({.size = allocationSize(values.size_bytes()),
                                               .usage = ResourceUsage::transferSrc,
                                               .cpuVisible = true,
                                               .lifetime = ResourceLifetime::persistent});
    if (!handle.valid())
        throw std::runtime_error("native scene staging buffer allocation returned an invalid handle: " +
                                 std::string(name));
    return handle;
}

template <typename T> [[nodiscard]] std::uint64_t hashValues(std::span<const T> values) noexcept {
    constexpr std::uint64_t offset = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    auto result = offset ^ static_cast<std::uint64_t>(values.size_bytes());
    for (const auto value : std::as_bytes(values)) {
        result ^= std::to_integer<std::uint8_t>(value);
        result *= prime;
    }
    return result;
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

template <std::size_t N>
void destroy(Device& device, std::vector<std::array<handles::BufferHandle, N>>& slots) noexcept {
    for (auto& slot : slots)
        for (const auto handle : slot) {
            if (!handle.valid())
                continue;
            try {
                device.destroyBufferEx(handle);
            } catch (...) {
            }
        }
    slots.clear();
}

} // namespace

NativeSceneModelRuntime::StaticHashes
NativeSceneModelRuntime::makeStaticHashes(const NativeSceneModelData& model) noexcept {
    return {.indices = hashValues(std::span<const std::uint32_t>(model.indices)),
            .materials = hashValues(std::span<const NativeSceneMaterial>(model.materials)),
            .faces = hashValues(std::span<const std::uint32_t>(model.faces)),
            .materialFaces = hashValues(std::span<const NativeSceneMaterialFace>(model.materialFaces)),
            .faceWalker = hashValues(std::span<const NativeSceneWalkerAlias>(model.faceWalker))};
}

bool NativeSceneModelRuntime::sameLayout(Device& device, std::span<const NativeSceneModelData> models) const noexcept {
    return ready() && device_ == &device && models.size() == modelCount() &&
           std::all_of(models.begin(), models.end(), [&](const auto& model) {
               const auto index = static_cast<std::size_t>(&model - models.data());
               return byteSize(model.vertices) == vertexBytes_[index] &&
                      byteSize(model.indices) == indexBytes_[index] &&
                      byteSize(model.materials) == materialBytes_[index] &&
                      byteSize(model.faces) == faceBytes_[index] &&
                      byteSize(model.materialFaces) == materialFaceBytes_[index] &&
                      byteSize(model.faceWalker) == faceWalkerBytes_[index];
           });
}

NativeSceneModelRuntime::~NativeSceneModelRuntime() {
    reset();
}

bool NativeSceneModelRuntime::sync(Device& device, std::span<const NativeSceneModelData> models, std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    if (models.empty()) {
        setError(error, "native scene model runtime requires at least one model");
        return false;
    }
    try {
        device_ = &device;
        vertexBytes_.reserve(models.size());
        indexBytes_.reserve(models.size());
        materialBytes_.reserve(models.size());
        faceBytes_.reserve(models.size());
        materialFaceBytes_.reserve(models.size());
        faceWalkerBytes_.reserve(models.size());

        for (const auto& model : models) {
            vertexBytes_.push_back(byteSize(model.vertices));
            indexBytes_.push_back(byteSize(model.indices));
            materialBytes_.push_back(byteSize(model.materials));
            faceBytes_.push_back(byteSize(model.faces));
            materialFaceBytes_.push_back(byteSize(model.materialFaces));
            faceWalkerBytes_.push_back(byteSize(model.faceWalker));
        }

        for (auto& frame : frameResources_) {
            frame.vertices.reserve(models.size());
            frame.previousVertices.reserve(models.size());
            frame.rawVertices.reserve(models.size());
            frame.vertexStaging.reserve(models.size());
            frame.indices.reserve(models.size());
            frame.indexStaging.reserve(models.size());
            frame.materials.reserve(models.size());
            frame.materialStaging.reserve(models.size());
            frame.faces.reserve(models.size());
            frame.faceStaging.reserve(models.size());
            frame.materialFaces.reserve(models.size());
            frame.materialFaceStaging.reserve(models.size());
            frame.faceWalkers.reserve(models.size());
            frame.faceWalkerStaging.reserve(models.size());
            frame.staticHashes.reserve(models.size());
            for (const auto& model : models) {
                frame.vertices.push_back(upload(device, std::span<const NativeSceneVertex>(model.vertices),
                                                ResourceUsage::storageRead | ResourceUsage::vertexRead |
                                                    ResourceUsage::asBuildRead | ResourceUsage::rayTracingRead |
                                                    ResourceUsage::transferSrc,
                                                "vertices"));
                frame.previousVertices.push_back(
                    upload(device, std::span<const NativeSceneVertex>(model.vertices),
                           ResourceUsage::storageRead | ResourceUsage::rayTracingRead | ResourceUsage::transferDst,
                           "previous vertices"));
                frame.vertexStaging.push_back(
                    createStaging(device, std::span<const NativeSceneVertex>(model.vertices), "vertex staging"));
                frame.rawVertices.push_back(upload(device, std::span<const NativeSceneVertex>(model.vertices),
                                                   ResourceUsage::storageRead | ResourceUsage::rayTracingRead,
                                                   "raw vertices"));
                frame.indices.push_back(upload(device, std::span<const std::uint32_t>(model.indices),
                                               ResourceUsage::storageRead | ResourceUsage::indexRead |
                                                   ResourceUsage::asBuildRead | ResourceUsage::rayTracingRead,
                                               "indices"));
                frame.materials.push_back(upload(device, std::span<const NativeSceneMaterial>(model.materials),
                                                 ResourceUsage::storageRead | ResourceUsage::rayTracingRead,
                                                 "materials"));
                frame.faces.push_back(upload(device, std::span<const std::uint32_t>(model.faces),
                                             ResourceUsage::storageRead | ResourceUsage::rayTracingRead, "faces"));
                frame.materialFaces.push_back(
                    upload(device, std::span<const NativeSceneMaterialFace>(model.materialFaces),
                           ResourceUsage::storageRead | ResourceUsage::rayTracingRead, "material faces"));
                frame.faceWalkers.push_back(upload(device, std::span<const NativeSceneWalkerAlias>(model.faceWalker),
                                                   ResourceUsage::storageRead | ResourceUsage::rayTracingRead,
                                                   "face walkers"));
                frame.indexStaging.push_back({});
                frame.materialStaging.push_back({});
                frame.faceStaging.push_back({});
                frame.materialFaceStaging.push_back({});
                frame.faceWalkerStaging.push_back({});
                frame.staticHashes.push_back(makeStaticHashes(model));
            }
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

bool NativeSceneModelRuntime::update(Device& device, std::span<const NativeSceneModelData> models, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (models.empty()) {
        reset();
        setError(error, "native scene model runtime requires at least one model");
        return false;
    }
    if (!sameLayout(device, models))
        return sync(device, models, error);
    try {
        for (auto& frame : frameResources_) {
            for (std::size_t index = 0; index < models.size(); ++index) {
                const auto& model = models[index];
                // Preserve the exact GPU stream used by the preceding frame
                // before replacing the current animated data. copyBufferEx is
                // an ordered transfer, so this does not require a CPU
                // readback or a second host-side vertex snapshot.
                device.copyBufferEx(frame.vertices[index], frame.previousVertices[index]);
                if (!model.vertices.empty())
                    device.uploadBufferEx(frame.vertices[index],
                                          std::as_bytes(std::span<const NativeSceneVertex>(model.vertices)), 0);
                const auto hashes = makeStaticHashes(model);
                if (hashes.indices != frame.staticHashes[index].indices && !model.indices.empty())
                    device.uploadBufferEx(frame.indices[index],
                                          std::as_bytes(std::span<const std::uint32_t>(model.indices)), 0);
                if (hashes.materials != frame.staticHashes[index].materials && !model.materials.empty())
                    device.uploadBufferEx(frame.materials[index],
                                          std::as_bytes(std::span<const NativeSceneMaterial>(model.materials)), 0);
                if (hashes.faces != frame.staticHashes[index].faces && !model.faces.empty())
                    device.uploadBufferEx(frame.faces[index],
                                          std::as_bytes(std::span<const std::uint32_t>(model.faces)), 0);
                if (hashes.materialFaces != frame.staticHashes[index].materialFaces && !model.materialFaces.empty())
                    device.uploadBufferEx(frame.materialFaces[index],
                                          std::as_bytes(std::span<const NativeSceneMaterialFace>(model.materialFaces)),
                                          0);
                if (hashes.faceWalker != frame.staticHashes[index].faceWalker && !model.faceWalker.empty())
                    device.uploadBufferEx(frame.faceWalkers[index],
                                          std::as_bytes(std::span<const NativeSceneWalkerAlias>(model.faceWalker)), 0);
                frame.staticHashes[index] = hashes;
            }
        }
    } catch (const std::exception& exception) {
        setError(error, std::string("native scene model update failed: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "native scene model update failed");
        return false;
    }
    return true;
}

bool NativeSceneModelRuntime::updateFrame(Device& device, CommandList& commands,
                                          std::span<const NativeSceneModelData> models, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (models.empty()) {
        reset();
        setError(error, "native scene model runtime requires at least one model");
        return false;
    }
    if (!sameLayout(device, models))
        return sync(device, models, error);

    try {
        const auto slot = device.currentFrameSlot() % kNativeFramesInFlight;
        const auto previousSlot = (slot + kNativeFramesInFlight - 1) % kNativeFramesInFlight;
        auto& frame = frameResources_[slot];
        const auto& previousFrame = frameResources_[previousSlot];
        bool recordedTransfer = false;
        bool recordedHistoryBarrier = false;
        for (std::size_t index = 0; index < models.size(); ++index) {
            const auto& model = models[index];
            if (!model.vertices.empty()) {
                if (!recordedHistoryBarrier) {
                    // The source is written by the preceding frame
                    // submission. A queue submission boundary alone does not
                    // make that write visible to this frame's copy read.
                    commands.memoryBarrierEx();
                    recordedHistoryBarrier = true;
                }
                device.uploadBufferEx(frame.vertexStaging[index],
                                      std::as_bytes(std::span<const NativeSceneVertex>(model.vertices)), 0);
                // Temporal history comes from the current stream recorded in
                // the preceding frame slot. Preserve it in this slot's PreVB
                // before replacing this slot's current vertex stream.
                commands.copyBufferEx(previousFrame.vertices[index], frame.previousVertices[index]);
                commands.copyBufferEx(frame.vertexStaging[index], frame.vertices[index]);
                recordedTransfer = true;
            }

            const auto hashes = makeStaticHashes(model);
            const auto copyIfChanged = [&](auto& staging, auto destination, const auto& values, std::uint64_t oldHash,
                                           std::uint64_t newHash, std::string_view name) {
                if (oldHash == newHash || values.empty())
                    return false;
                if (!staging[index].valid())
                    staging[index] = createStaging(device, std::span(values), name);
                device.uploadBufferEx(staging[index], std::as_bytes(std::span(values)), 0);
                commands.copyBufferEx(staging[index], destination);
                return true;
            };
            const bool indicesChanged =
                copyIfChanged(frame.indexStaging, frame.indices[index], model.indices,
                              frame.staticHashes[index].indices, hashes.indices, "index staging");
            const bool materialsChanged =
                copyIfChanged(frame.materialStaging, frame.materials[index], model.materials,
                              frame.staticHashes[index].materials, hashes.materials, "material staging");
            const bool facesChanged = copyIfChanged(frame.faceStaging, frame.faces[index], model.faces,
                                                    frame.staticHashes[index].faces, hashes.faces, "face staging");
            const bool materialFacesChanged =
                copyIfChanged(frame.materialFaceStaging, frame.materialFaces[index], model.materialFaces,
                              frame.staticHashes[index].materialFaces, hashes.materialFaces, "material face staging");
            const bool faceWalkerChanged =
                copyIfChanged(frame.faceWalkerStaging, frame.faceWalkers[index], model.faceWalker,
                              frame.staticHashes[index].faceWalker, hashes.faceWalker, "face walker staging");
            recordedTransfer = recordedTransfer || indicesChanged || materialsChanged || facesChanged ||
                               materialFacesChanged || faceWalkerChanged;
            frame.staticHashes[index] = hashes;
        }
        if (recordedTransfer)
            commands.memoryBarrierEx();
    } catch (const std::exception& exception) {
        setError(error, std::string("native scene frame update failed: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "native scene frame update failed");
        return false;
    }
    return true;
}

NativeSceneDescriptorCounts NativeSceneModelRuntime::descriptorCounts() const noexcept {
    const auto count =
        static_cast<std::uint32_t>(std::min<std::size_t>(modelCount(), std::numeric_limits<std::uint32_t>::max()));
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
    const auto& frame = frameResources_[currentSlot()];
    NativeSceneResourceBindings result;
    result.vertexBuffers = frame.vertices;
    result.indexBuffers = frame.indices;
    result.materials = frame.materials;
    result.faces = frame.faces;
    result.materialFaces = frame.materialFaces;
    result.faceWalkers = frame.faceWalkers;
    result.previousVertices = frame.previousVertices;
    result.rawVertices = frame.rawVertices;
    return result;
}

void NativeSceneModelRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        for (auto& frame : frameResources_) {
            destroy(*device, frame.vertices);
            destroy(*device, frame.previousVertices);
            destroy(*device, frame.rawVertices);
            destroy(*device, frame.vertexStaging);
            destroy(*device, frame.indices);
            destroy(*device, frame.indexStaging);
            destroy(*device, frame.materials);
            destroy(*device, frame.materialStaging);
            destroy(*device, frame.faces);
            destroy(*device, frame.faceStaging);
            destroy(*device, frame.materialFaces);
            destroy(*device, frame.materialFaceStaging);
            destroy(*device, frame.faceWalkers);
            destroy(*device, frame.faceWalkerStaging);
        }
    } else {
        frameResources_ = {};
    }
    vertexBytes_.clear();
    indexBytes_.clear();
    materialBytes_.clear();
    faceBytes_.clear();
    materialFaceBytes_.clear();
    faceWalkerBytes_.clear();
    frameResources_ = {};
    device_ = nullptr;
}

} // namespace dayo::graphics
