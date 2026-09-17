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
        vertexStaging_.reserve(models.size());
        indices_.reserve(models.size());
        indexStaging_.reserve(models.size());
        materials_.reserve(models.size());
        materialStaging_.reserve(models.size());
        faces_.reserve(models.size());
        faceStaging_.reserve(models.size());
        materialFaces_.reserve(models.size());
        materialFaceStaging_.reserve(models.size());
        faceWalkers_.reserve(models.size());
        faceWalkerStaging_.reserve(models.size());
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
            vertices_.push_back(upload(device, std::span<const NativeSceneVertex>(model.vertices),
                                       ResourceUsage::storageRead | ResourceUsage::vertexRead |
                                           ResourceUsage::asBuildRead | ResourceUsage::rayTracingRead |
                                           ResourceUsage::transferSrc,
                                       "vertices"));
            previousVertices_.push_back(
                upload(device, std::span<const NativeSceneVertex>(model.vertices),
                       ResourceUsage::storageRead | ResourceUsage::rayTracingRead | ResourceUsage::transferDst,
                       "previous vertices"));
            StagingSlots vertexStaging{};
            for (auto& slot : vertexStaging)
                slot = createStaging(device, std::span<const NativeSceneVertex>(model.vertices), "vertex staging");
            vertexStaging_.push_back(vertexStaging);
            rawVertices_.push_back(upload(device, std::span<const NativeSceneVertex>(model.vertices),
                                          ResourceUsage::storageRead | ResourceUsage::rayTracingRead, "raw vertices"));
            indices_.push_back(upload(device, std::span<const std::uint32_t>(model.indices),
                                      ResourceUsage::storageRead | ResourceUsage::indexRead | ResourceUsage::asBuildRead |
                                          ResourceUsage::rayTracingRead,
                                      "indices"));
            materials_.push_back(upload(device, std::span<const NativeSceneMaterial>(model.materials),
                                        ResourceUsage::storageRead | ResourceUsage::rayTracingRead, "materials"));
            faces_.push_back(upload(device, std::span<const std::uint32_t>(model.faces),
                                    ResourceUsage::storageRead | ResourceUsage::rayTracingRead, "faces"));
            materialFaces_.push_back(upload(device, std::span<const NativeSceneMaterialFace>(model.materialFaces),
                                            ResourceUsage::storageRead | ResourceUsage::rayTracingRead,
                                            "material faces"));
            faceWalkers_.push_back(upload(device, std::span<const NativeSceneWalkerAlias>(model.faceWalker),
                                          ResourceUsage::storageRead | ResourceUsage::rayTracingRead, "face walkers"));
            indexStaging_.push_back({});
            materialStaging_.push_back({});
            faceStaging_.push_back({});
            materialFaceStaging_.push_back({});
            faceWalkerStaging_.push_back({});
            staticHashes_.push_back(makeStaticHashes(model));
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

bool NativeSceneModelRuntime::update(Device& device, std::span<const NativeSceneModelData> models,
                                     std::string* error) {
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
        for (std::size_t index = 0; index < models.size(); ++index) {
            const auto& model = models[index];
            // Preserve the exact GPU stream used by the preceding frame
            // before replacing the current animated data. copyBufferEx is an
            // ordered transfer, so this does not require a CPU readback or a
            // second host-side vertex snapshot.
            device.copyBufferEx(vertices_[index], previousVertices_[index]);
            if (!model.vertices.empty())
                device.uploadBufferEx(vertices_[index],
                                      std::as_bytes(std::span<const NativeSceneVertex>(model.vertices)), 0);
            const auto hashes = makeStaticHashes(model);
            if (hashes.indices != staticHashes_[index].indices && !model.indices.empty())
                device.uploadBufferEx(indices_[index], std::as_bytes(std::span<const std::uint32_t>(model.indices)), 0);
            if (hashes.materials != staticHashes_[index].materials && !model.materials.empty())
                device.uploadBufferEx(materials_[index],
                                      std::as_bytes(std::span<const NativeSceneMaterial>(model.materials)), 0);
            if (hashes.faces != staticHashes_[index].faces && !model.faces.empty())
                device.uploadBufferEx(faces_[index], std::as_bytes(std::span<const std::uint32_t>(model.faces)), 0);
            if (hashes.materialFaces != staticHashes_[index].materialFaces && !model.materialFaces.empty())
                device.uploadBufferEx(materialFaces_[index],
                                      std::as_bytes(std::span<const NativeSceneMaterialFace>(model.materialFaces)), 0);
            if (hashes.faceWalker != staticHashes_[index].faceWalker && !model.faceWalker.empty())
                device.uploadBufferEx(faceWalkers_[index],
                                      std::as_bytes(std::span<const NativeSceneWalkerAlias>(model.faceWalker)), 0);
            staticHashes_[index] = hashes;
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
        const auto slot = transferSlot_;
        bool recordedTransfer = false;
        for (std::size_t index = 0; index < models.size(); ++index) {
            const auto& model = models[index];
            if (!model.vertices.empty()) {
                device.uploadBufferEx(vertexStaging_[index][slot],
                                      std::as_bytes(std::span<const NativeSceneVertex>(model.vertices)), 0);
                commands.copyBufferEx(vertices_[index], previousVertices_[index]);
                commands.copyBufferEx(vertexStaging_[index][slot], vertices_[index]);
                recordedTransfer = true;
            }

            const auto hashes = makeStaticHashes(model);
            const auto copyIfChanged = [&](auto& staging, auto destination, const auto& values, std::uint64_t oldHash,
                                           std::uint64_t newHash, std::string_view name) {
                if (oldHash == newHash || values.empty())
                    return false;
                if (!staging[index][slot].valid())
                    staging[index][slot] = createStaging(device, std::span(values), name);
                device.uploadBufferEx(staging[index][slot], std::as_bytes(std::span(values)), 0);
                commands.copyBufferEx(staging[index][slot], destination);
                return true;
            };
            const bool indicesChanged = copyIfChanged(indexStaging_, indices_[index], model.indices,
                                                      staticHashes_[index].indices, hashes.indices, "index staging");
            const bool materialsChanged =
                copyIfChanged(materialStaging_, materials_[index], model.materials, staticHashes_[index].materials,
                              hashes.materials, "material staging");
            const bool facesChanged = copyIfChanged(faceStaging_, faces_[index], model.faces,
                                                    staticHashes_[index].faces, hashes.faces, "face staging");
            const bool materialFacesChanged =
                copyIfChanged(materialFaceStaging_, materialFaces_[index], model.materialFaces,
                              staticHashes_[index].materialFaces, hashes.materialFaces, "material face staging");
            const bool faceWalkerChanged =
                copyIfChanged(faceWalkerStaging_, faceWalkers_[index], model.faceWalker,
                              staticHashes_[index].faceWalker, hashes.faceWalker, "face walker staging");
            recordedTransfer = recordedTransfer || indicesChanged || materialsChanged || facesChanged ||
                               materialFacesChanged || faceWalkerChanged;
            staticHashes_[index] = hashes;
        }
        if (recordedTransfer)
            commands.memoryBarrierEx();
        transferSlot_ = (transferSlot_ + 1U) % kTransferSlots;
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
    NativeSceneResourceBindings result;
    result.vertexBuffers = vertices_;
    result.indexBuffers = indices_;
    result.materials = materials_;
    result.faces = faces_;
    result.materialFaces = materialFaces_;
    result.faceWalkers = faceWalkers_;
    result.previousVertices = previousVertices_;
    result.rawVertices = rawVertices_;
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
        destroy(*device, previousVertices_);
        destroy(*device, rawVertices_);
        destroy(*device, vertexStaging_);
        destroy(*device, indices_);
        destroy(*device, indexStaging_);
        destroy(*device, materials_);
        destroy(*device, materialStaging_);
        destroy(*device, faces_);
        destroy(*device, faceStaging_);
        destroy(*device, materialFaces_);
        destroy(*device, materialFaceStaging_);
        destroy(*device, faceWalkers_);
        destroy(*device, faceWalkerStaging_);
    } else {
        vertices_.clear();
        previousVertices_.clear();
        rawVertices_.clear();
        vertexStaging_.clear();
        indices_.clear();
        indexStaging_.clear();
        materials_.clear();
        materialStaging_.clear();
        faces_.clear();
        faceStaging_.clear();
        materialFaces_.clear();
        materialFaceStaging_.clear();
        faceWalkers_.clear();
        faceWalkerStaging_.clear();
    }
    vertexBytes_.clear();
    indexBytes_.clear();
    materialBytes_.clear();
    faceBytes_.clear();
    materialFaceBytes_.clear();
    faceWalkerBytes_.clear();
    staticHashes_.clear();
    transferSlot_ = 0;
    device_ = nullptr;
}

} // namespace dayo::graphics
