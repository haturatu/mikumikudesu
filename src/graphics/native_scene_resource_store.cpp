#include "graphics/native_scene_resource_store.hpp"

#include "graphics/dayo_host_resources.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

[[nodiscard]] std::uint32_t descriptorCount(std::uint32_t count) noexcept {
    return std::max(1U, count);
}

[[nodiscard]] bool valid(std::span<const handles::TextureHandle> values) noexcept {
    return std::all_of(values.begin(), values.end(), [](const auto value) { return value.valid(); });
}

[[nodiscard]] bool valid(std::span<const handles::BufferHandle> values) noexcept {
    return std::all_of(values.begin(), values.end(), [](const auto value) { return value.valid(); });
}

bool checkArray(std::size_t actual, std::uint32_t expected, std::string_view name, std::string* error) {
    if (actual == descriptorCount(expected))
        return true;
    setError(error, "native scene resource override count mismatch: " + std::string(name));
    return false;
}

template <typename Handle> std::vector<Handle> fallbackArray(std::uint32_t count, Handle placeholder) {
    return std::vector<Handle>(descriptorCount(count), placeholder);
}

} // namespace

NativeSceneResourceStore::~NativeSceneResourceStore() {
    reset();
}

bool NativeSceneResourceStore::initialize(Device& device, NativeSceneDescriptorCounts counts, std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    counts_ = counts;
    try {
        device_ = &device;
        placeholderTexture_ = device.createTextureEx({
            .dimension = TextureDimension::d2,
            .extent = {1, 1, 1},
            .format = PixelFormat::rgba16Float,
            .mipLevels = 1,
            .arrayLayers = 1,
            .usage = ResourceUsage::sampledRead | ResourceUsage::storageReadWrite | ResourceUsage::transferDst,
            .lifetime = ResourceLifetime::persistent,
        });
        placeholderBuffer_ = device.createBufferEx({
            .size = 64,
            .usage = ResourceUsage::storageRead | ResourceUsage::uniformRead | ResourceUsage::transferDst,
            .cpuVisible = false,
            .lifetime = ResourceLifetime::persistent,
        });
        if (!placeholderTexture_.valid() || !placeholderBuffer_.valid())
            throw std::runtime_error("native scene placeholder resource allocation returned an invalid handle");

        textures_ = fallbackArray(counts_.textures, placeholderTexture_);
        vertexBuffers_ = fallbackArray(counts_.vertexBuffers, placeholderBuffer_);
        indexBuffers_ = fallbackArray(counts_.indexBuffers, placeholderBuffer_);
        materials_ = fallbackArray(counts_.materials, placeholderBuffer_);
        faces_ = fallbackArray(counts_.faces, placeholderBuffer_);
        materialFaces_ = fallbackArray(counts_.materialFaces, placeholderBuffer_);
        faceWalkers_ = fallbackArray(counts_.faceWalkers, placeholderBuffer_);
        previousVertices_ = fallbackArray(counts_.previousVertices, placeholderBuffer_);
        rawVertices_ = fallbackArray(counts_.rawVertices, placeholderBuffer_);
        // Initialization deliberately leaves TLAS empty. A placeholder
        // acceleration structure would make a missing geometry sync look
        // healthy and could be submitted to the ray-tracing descriptor.
        bindings_.rtOutput = placeholderTexture_;
        bindings_.oidnBuffer = placeholderBuffer_;
        bindings_.normalDepth = placeholderTexture_;
        bindings_.gbuffer1 = placeholderTexture_;
        bindings_.gbuffer2 = placeholderTexture_;
        bindings_.gbuffer = placeholderTexture_;
        bindings_.modelToMaterial = placeholderBuffer_;
        bindings_.materialToModel = placeholderBuffer_;
        bindings_.peekaboo = placeholderBuffer_;
        bindings_.materialSelected = placeholderBuffer_;
        bindings_.skybox = placeholderTexture_;
        bindings_.skywalker = placeholderBuffer_;
        bindings_.skywalkerRow = placeholderBuffer_;
        bindings_.skyboxSh = placeholderBuffer_;
        bindings_.screenBmp = placeholderTexture_;
        bindings_.cloneCount = placeholderBuffer_;
        bindings_.screenTexture = placeholderTexture_;
        bindings_.viewConstants = placeholderBuffer_;
        bindings_.controllerConstants = placeholderBuffer_;
        bindings_.globalConstants = placeholderBuffer_;
        bindings_.textureTable = placeholderBuffer_;
        bindings_.passConstants = placeholderBuffer_;
        if (!replaceArrays({}, error))
            throw std::runtime_error(
                error != nullptr && !error->empty() ? *error : "native scene placeholder composition failed");
    } catch (const std::exception& exception) {
        setError(error, std::string("native scene resource store initialization failed: ") + exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "native scene resource store initialization failed");
        reset();
        return false;
    }
    return true;
}

handles::TextureHandle NativeSceneResourceStore::textureOrPlaceholder(handles::TextureHandle value) const noexcept {
    return value.valid() ? value : placeholderTexture_;
}

handles::BufferHandle NativeSceneResourceStore::bufferOrPlaceholder(handles::BufferHandle value) const noexcept {
    return value.valid() ? value : placeholderBuffer_;
}

bool NativeSceneResourceStore::replaceScalars(const NativeSceneResourceBindings& overrides, std::string* error) {
    if (!overrides.tlas.valid()) {
        setError(error, "native scene resource store requires a real TLAS");
        return false;
    }
    bindings_.rtOutput = textureOrPlaceholder(overrides.rtOutput);
    bindings_.oidnBuffer = bufferOrPlaceholder(overrides.oidnBuffer);
    bindings_.normalDepth = textureOrPlaceholder(overrides.normalDepth);
    bindings_.gbuffer1 = textureOrPlaceholder(overrides.gbuffer1);
    bindings_.gbuffer2 = textureOrPlaceholder(overrides.gbuffer2);
    const auto legacyGBuffer =
        overrides.gbuffer.valid() && overrides.gbuffer != placeholderTexture_ ? overrides.gbuffer : overrides.gbuffer1;
    bindings_.gbuffer = textureOrPlaceholder(legacyGBuffer);
    bindings_.tlas = overrides.tlas;
    bindings_.modelToMaterial = bufferOrPlaceholder(overrides.modelToMaterial);
    bindings_.materialToModel = bufferOrPlaceholder(overrides.materialToModel);
    bindings_.peekaboo = bufferOrPlaceholder(overrides.peekaboo);
    bindings_.materialSelected = bufferOrPlaceholder(overrides.materialSelected);
    bindings_.skybox = textureOrPlaceholder(overrides.skybox);
    bindings_.skywalker = bufferOrPlaceholder(overrides.skywalker);
    bindings_.skywalkerRow = bufferOrPlaceholder(overrides.skywalkerRow);
    bindings_.skyboxSh = bufferOrPlaceholder(overrides.skyboxSh);
    bindings_.screenBmp = textureOrPlaceholder(overrides.screenBmp);
    bindings_.cloneCount = bufferOrPlaceholder(overrides.cloneCount);
    bindings_.screenTexture = textureOrPlaceholder(overrides.screenTexture);
    bindings_.viewConstants = bufferOrPlaceholder(overrides.viewConstants);
    bindings_.controllerConstants = bufferOrPlaceholder(overrides.controllerConstants);
    bindings_.globalConstants = bufferOrPlaceholder(overrides.globalConstants);
    bindings_.textureTable = bufferOrPlaceholder(overrides.textureTable);
    bindings_.passConstants = bufferOrPlaceholder(overrides.passConstants);
    bindings_.hostResourceMask = 0;
    const auto mark = [this](DayoSemantic semantic, bool present) {
        if (present)
            bindings_.hostResourceMask |= dayoSemanticBit(semantic);
    };
    const auto realTexture = [this](handles::TextureHandle value) {
        return value.valid() && value != placeholderTexture_;
    };
    const auto realBuffer = [this](handles::BufferHandle value) {
        return value.valid() && value != placeholderBuffer_;
    };
    mark(DayoSemantic::RTOutput, realTexture(overrides.rtOutput));
    mark(DayoSemantic::OIDNBuf, realBuffer(overrides.oidnBuffer));
    mark(DayoSemantic::NormalDepth, realTexture(overrides.normalDepth));
    mark(DayoSemantic::GBuffer1, realTexture(overrides.gbuffer1));
    mark(DayoSemantic::GBuffer2, realTexture(overrides.gbuffer2));
    mark(DayoSemantic::GBuffer, realTexture(legacyGBuffer));
    mark(DayoSemantic::TLAS, overrides.tlas.valid());
    mark(DayoSemantic::Model2Mat, realBuffer(overrides.modelToMaterial));
    mark(DayoSemantic::Mat2Model, realBuffer(overrides.materialToModel));
    mark(DayoSemantic::Peekaboo, realBuffer(overrides.peekaboo));
    mark(DayoSemantic::MatSelected, realBuffer(overrides.materialSelected));
    mark(DayoSemantic::Skybox, realTexture(overrides.skybox));
    mark(DayoSemantic::Skywalker, realBuffer(overrides.skywalker));
    mark(DayoSemantic::SkywalkerRow, realBuffer(overrides.skywalkerRow));
    mark(DayoSemantic::SkyboxSH, realBuffer(overrides.skyboxSh));
    mark(DayoSemantic::ScreenBMP, realTexture(overrides.screenBmp));
    mark(DayoSemantic::CloneCount, realBuffer(overrides.cloneCount));
    mark(DayoSemantic::ScreenTexture, realTexture(overrides.screenTexture));
    mark(DayoSemantic::ViewCB, realBuffer(overrides.viewConstants));
    mark(DayoSemantic::ControllerCB, realBuffer(overrides.controllerConstants));
    mark(DayoSemantic::TextureTable, realBuffer(overrides.textureTable));
    mark(DayoSemantic::CBuff1, realBuffer(overrides.passConstants));
    return true;
}

bool NativeSceneResourceStore::replaceArrays(const NativeSceneResourceBindings& overrides, std::string* error) {
    const auto copyTextures = [&](std::vector<handles::TextureHandle>& destination,
                                  std::span<const handles::TextureHandle> source, std::uint32_t expected,
                                  std::string_view name) {
        if (source.empty())
            return true;
        if (!checkArray(source.size(), expected, name, error) || !valid(source))
            return false;
        destination.assign(source.begin(), source.end());
        return true;
    };
    const auto copyBuffers = [&](std::vector<handles::BufferHandle>& destination,
                                 std::span<const handles::BufferHandle> source, std::uint32_t expected,
                                 std::string_view name) {
        if (source.empty())
            return true;
        if (!checkArray(source.size(), expected, name, error) || !valid(source))
            return false;
        destination.assign(source.begin(), source.end());
        return true;
    };
    if (!copyTextures(textures_, overrides.textures, counts_.textures, "textures") ||
        !copyBuffers(vertexBuffers_, overrides.vertexBuffers, counts_.vertexBuffers, "vertex buffers") ||
        !copyBuffers(indexBuffers_, overrides.indexBuffers, counts_.indexBuffers, "index buffers") ||
        !copyBuffers(materials_, overrides.materials, counts_.materials, "materials") ||
        !copyBuffers(faces_, overrides.faces, counts_.faces, "faces") ||
        !copyBuffers(materialFaces_, overrides.materialFaces, counts_.materialFaces, "material faces") ||
        !copyBuffers(faceWalkers_, overrides.faceWalkers, counts_.faceWalkers, "face walkers") ||
        !copyBuffers(previousVertices_, overrides.previousVertices, counts_.previousVertices, "previous vertices") ||
        !copyBuffers(rawVertices_, overrides.rawVertices, counts_.rawVertices, "raw vertices"))
        return false;
    bindings_.textures = textures_;
    bindings_.vertexBuffers = vertexBuffers_;
    bindings_.indexBuffers = indexBuffers_;
    bindings_.materials = materials_;
    bindings_.faces = faces_;
    bindings_.materialFaces = materialFaces_;
    bindings_.faceWalkers = faceWalkers_;
    bindings_.previousVertices = previousVertices_;
    bindings_.rawVertices = rawVertices_;
    return true;
}

bool NativeSceneResourceStore::compose(const NativeSceneResourceBindings& overrides, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (!ready()) {
        setError(error, "native scene resource store is not initialized");
        return false;
    }
    if (!replaceScalars(overrides, error) || !replaceArrays(overrides, error))
        return false;
    return true;
}

void NativeSceneResourceStore::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        if (placeholderBuffer_.valid()) {
            try {
                device->destroyBufferEx(placeholderBuffer_);
            } catch (...) {
            }
        }
        if (placeholderTexture_.valid()) {
            try {
                device->destroyTextureEx(placeholderTexture_);
            } catch (...) {
            }
        }
    }
    device_ = nullptr;
    counts_ = {};
    placeholderTexture_ = {};
    placeholderBuffer_ = {};
    bindings_ = {};
    textures_.clear();
    vertexBuffers_.clear();
    indexBuffers_.clear();
    materials_.clear();
    faces_.clear();
    materialFaces_.clear();
    faceWalkers_.clear();
    previousVertices_.clear();
    rawVertices_.clear();
}

} // namespace dayo::graphics
