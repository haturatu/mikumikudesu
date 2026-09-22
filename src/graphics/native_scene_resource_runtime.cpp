#include "graphics/native_scene_resource_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string value) {
    if (error != nullptr)
        *error = std::move(value);
}

[[nodiscard]] std::uint32_t countOrPlaceholder(std::uint32_t count) noexcept {
    return count == 0 ? 1U : count;
}

[[nodiscard]] bool valid(std::span<const handles::BufferHandle> resources) noexcept {
    return std::all_of(resources.begin(), resources.end(), [](const auto handle) { return handle.valid(); });
}

[[nodiscard]] bool valid(std::span<const handles::TextureHandle> resources) noexcept {
    return std::all_of(resources.begin(), resources.end(), [](const auto handle) { return handle.valid(); });
}

bool requireCount(std::size_t actual, std::uint32_t expected, std::string_view name, std::string* error) {
    if (actual == countOrPlaceholder(expected))
        return true;
    setError(error, "native scene resource count mismatch: " + std::string(name));
    return false;
}

} // namespace

std::vector<DescriptorBindingEx> nativeSceneFrameDescriptorBindings(const NativeSceneResourceBindings& resources) {
    const auto legacyGBuffer = resources.gbuffer.valid() ? resources.gbuffer : resources.gbuffer1;
    std::vector<DescriptorBindingEx> frame;
    frame.reserve(20);
    const auto addTexture = [&frame](NativeSceneRegisterClass registerClass, std::uint32_t index,
                                     handles::TextureHandle handle) {
        frame.push_back({.slot = nativeSceneBinding(registerClass, index), .arrayElement = 0, .texture = handle});
    };
    const auto addBuffer = [&frame](NativeSceneRegisterClass registerClass, std::uint32_t index,
                                    handles::BufferHandle handle) {
        frame.push_back({.slot = nativeSceneBinding(registerClass, index), .arrayElement = 0, .buffer = handle});
    };
    addTexture(NativeSceneRegisterClass::uav, 0, resources.rtOutput);
    addBuffer(NativeSceneRegisterClass::uav, 1, resources.oidnBuffer);
    addTexture(NativeSceneRegisterClass::uav, 2, resources.normalDepth);
    addTexture(NativeSceneRegisterClass::uav, 3, resources.gbuffer1);
    addTexture(NativeSceneRegisterClass::uav, 4, resources.gbuffer2);
    addBuffer(NativeSceneRegisterClass::uniform, 0, resources.viewConstants);
    addBuffer(NativeSceneRegisterClass::uniform, 1, resources.controllerConstants);
    frame.push_back({.slot = nativeSceneBinding(NativeSceneRegisterClass::sampled, 0),
                     .arrayElement = 0,
                     .accelerationStructure = resources.tlas});
    addBuffer(NativeSceneRegisterClass::sampled, 1, resources.modelToMaterial);
    addBuffer(NativeSceneRegisterClass::sampled, 2, resources.materialToModel);
    addBuffer(NativeSceneRegisterClass::sampled, 3, resources.peekaboo);
    addBuffer(NativeSceneRegisterClass::sampled, 4, resources.materialSelected);
    addTexture(NativeSceneRegisterClass::sampled, 5, resources.skybox);
    addBuffer(NativeSceneRegisterClass::sampled, 6, resources.skywalker);
    addBuffer(NativeSceneRegisterClass::sampled, 7, resources.skywalkerRow);
    addBuffer(NativeSceneRegisterClass::sampled, 8, resources.skyboxSh);
    addTexture(NativeSceneRegisterClass::sampled, 9, resources.screenBmp);
    addBuffer(NativeSceneRegisterClass::sampled, 10, resources.cloneCount);
    addTexture(NativeSceneRegisterClass::sampled, 11, resources.screenTexture);
    addTexture(NativeSceneRegisterClass::sampled, 12, legacyGBuffer);
    return frame;
}

bool NativeSceneResourceRuntime::sync(const NativeSceneResourceBindings& resources, std::string* error) {
    if (error != nullptr)
        error->clear();
    if (!ready()) {
        setError(error, "native scene resource runtime is not initialized");
        return false;
    }
    const auto& counts = this->counts();
    const auto legacyGBuffer = resources.gbuffer.valid() ? resources.gbuffer : resources.gbuffer1;
    if (!requireCount(resources.textures.size(), counts.textures, "textures", error) ||
        !requireCount(resources.vertexBuffers.size(), counts.vertexBuffers, "vertex buffers", error) ||
        !requireCount(resources.indexBuffers.size(), counts.indexBuffers, "index buffers", error) ||
        !requireCount(resources.materials.size(), counts.materials, "materials", error) ||
        !requireCount(resources.faces.size(), counts.faces, "faces", error) ||
        !requireCount(resources.materialFaces.size(), counts.materialFaces, "material faces", error) ||
        !requireCount(resources.faceWalkers.size(), counts.faceWalkers, "face walkers", error) ||
        !requireCount(resources.previousVertices.size(), counts.previousVertices, "previous vertices", error) ||
        !requireCount(resources.rawVertices.size(), counts.rawVertices, "raw vertices", error))
        return false;

    const auto checkHandle = [error](bool condition, std::string_view name) {
        if (condition)
            return true;
        setError(error, "native scene resource is unavailable: " + std::string(name));
        return false;
    };
    if (!checkHandle(resources.rtOutput.valid(), "RTOutput") || !checkHandle(resources.oidnBuffer.valid(), "OIDNBuf") ||
        !checkHandle(resources.normalDepth.valid(), "NormalDepth") ||
        !checkHandle(resources.gbuffer1.valid(), "GBuffer1") || !checkHandle(resources.gbuffer2.valid(), "GBuffer2") ||
        !checkHandle(legacyGBuffer.valid(), "GBuffer") || !checkHandle(resources.tlas.valid(), "TLAS") ||
        !checkHandle(resources.modelToMaterial.valid(), "Model2Mat") ||
        !checkHandle(resources.materialToModel.valid(), "Mat2Model") ||
        !checkHandle(resources.peekaboo.valid(), "Peekaboo") ||
        !checkHandle(resources.materialSelected.valid(), "MatSelected") ||
        !checkHandle(resources.skybox.valid(), "Skybox") || !checkHandle(resources.skywalker.valid(), "Skywalker") ||
        !checkHandle(resources.skywalkerRow.valid(), "SkywalkerRow") ||
        !checkHandle(resources.skyboxSh.valid(), "SkyboxSH") ||
        !checkHandle(resources.screenBmp.valid(), "ScreenBMP") ||
        !checkHandle(resources.cloneCount.valid(), "CloneCount") ||
        !checkHandle(resources.screenTexture.valid(), "ScreenTexture") ||
        !checkHandle(resources.viewConstants.valid(), "ViewCB") ||
        !checkHandle(resources.controllerConstants.valid(), "YRZFX_ControllerCB") ||
        !checkHandle(resources.textureTable.valid(), "TextureTable") ||
        !checkHandle(resources.passConstants.valid(), "CBuff1") ||
        !checkHandle(valid(resources.textures), "Textures") || !checkHandle(valid(resources.vertexBuffers), "VB") ||
        !checkHandle(valid(resources.indexBuffers), "IB") || !checkHandle(valid(resources.materials), "MMDMaterials") ||
        !checkHandle(valid(resources.faces), "Faces") || !checkHandle(valid(resources.materialFaces), "Mat2face") ||
        !checkHandle(valid(resources.faceWalkers), "FaceWalker") ||
        !checkHandle(valid(resources.previousVertices), "PreVB") || !checkHandle(valid(resources.rawVertices), "RawVB"))
        return false;

    auto frame = nativeSceneFrameDescriptorBindings(resources);
    if (!bindings_.bind(NativeSceneDescriptorSet::frame, frame, error))
        return false;

    std::vector<DescriptorBindingEx> textures;
    textures.reserve(resources.textures.size() + 2);
    textures.push_back({.slot = nativeSceneBinding(NativeSceneRegisterClass::sampled, 0),
                        .arrayElement = 0,
                        .buffer = resources.textureTable});
    for (std::size_t index = 0; index < resources.textures.size(); ++index)
        textures.push_back({.slot = nativeSceneBinding(NativeSceneRegisterClass::sampled, 1),
                            .arrayElement = static_cast<std::uint32_t>(index),
                            .texture = resources.textures[index]});
    textures.push_back({.slot = nativeSceneBinding(NativeSceneRegisterClass::uniform, 0),
                        .arrayElement = 0,
                        .buffer = resources.passConstants});
    if (!bindings_.bind(NativeSceneDescriptorSet::textures, textures, error))
        return false;

    const auto bindBuffers = [this, error](NativeSceneDescriptorSet set,
                                           std::span<const handles::BufferHandle> source) {
        std::vector<DescriptorBindingEx> descriptors;
        descriptors.reserve(source.size());
        for (std::size_t index = 0; index < source.size(); ++index)
            descriptors.push_back({.slot = nativeSceneBinding(NativeSceneRegisterClass::sampled, 0),
                                   .arrayElement = static_cast<std::uint32_t>(index),
                                   .buffer = source[index]});
        return bindings_.bind(set, descriptors, error);
    };
    return bindBuffers(NativeSceneDescriptorSet::vertexBuffers, resources.vertexBuffers) &&
           bindBuffers(NativeSceneDescriptorSet::indexBuffers, resources.indexBuffers) &&
           bindBuffers(NativeSceneDescriptorSet::materials, resources.materials) &&
           bindBuffers(NativeSceneDescriptorSet::faces, resources.faces) &&
           bindBuffers(NativeSceneDescriptorSet::materialFaces, resources.materialFaces) &&
           bindBuffers(NativeSceneDescriptorSet::faceWalkers, resources.faceWalkers) &&
           bindBuffers(NativeSceneDescriptorSet::previousVertices, resources.previousVertices) &&
           bindBuffers(NativeSceneDescriptorSet::rawVertices, resources.rawVertices);
}

} // namespace dayo::graphics
