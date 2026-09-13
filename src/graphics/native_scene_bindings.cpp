#include "graphics/native_scene_bindings.hpp"

#include <algorithm>

namespace dayo::graphics {
namespace {

[[nodiscard]] constexpr ShaderStageMask nativeSceneStages() noexcept {
    return ShaderStageMask::vertex | ShaderStageMask::fragment | ShaderStageMask::compute |
           ShaderStageMask::rayGeneration | ShaderStageMask::miss | ShaderStageMask::closestHit |
           ShaderStageMask::anyHit | ShaderStageMask::intersection | ShaderStageMask::callable;
}

[[nodiscard]] constexpr std::uint32_t descriptorCount(std::uint32_t count) noexcept {
    return std::max(1U, count);
}

void add(DescriptorSetLayoutDesc& layout, NativeSceneRegisterClass registerClass, std::uint32_t registerIndex,
         DescriptorKind kind, std::uint32_t count, ShaderStageMask stages) {
    layout.bindings.push_back(
        {nativeSceneBinding(registerClass, registerIndex), kind, descriptorCount(count), stages});
}

} // namespace

DescriptorSetLayoutDesc nativeSceneDescriptorLayout(NativeSceneDescriptorSet set,
                                                    const NativeSceneDescriptorCounts& counts) noexcept {
    const auto stages = nativeSceneStages();
    DescriptorSetLayoutDesc result;
    switch (set) {
    case NativeSceneDescriptorSet::frame:
        result.bindings.reserve(15);
        for (std::uint32_t index = 0; index < 5; ++index)
            add(result, NativeSceneRegisterClass::uav, index, DescriptorKind::storageImage, 1, stages);
        add(result, NativeSceneRegisterClass::sampled, 0, DescriptorKind::accelerationStructure, 1, stages);
        for (std::uint32_t index = 1; index <= 4; ++index)
            add(result, NativeSceneRegisterClass::sampled, index, DescriptorKind::storageBuffer, 1, stages);
        add(result, NativeSceneRegisterClass::sampled, 5, DescriptorKind::sampledImage, 1, stages);
        for (std::uint32_t index = 6; index <= 8; ++index)
            add(result, NativeSceneRegisterClass::sampled, index, DescriptorKind::storageBuffer, 1, stages);
        add(result, NativeSceneRegisterClass::sampled, 9, DescriptorKind::sampledImage, 1, stages);
        add(result, NativeSceneRegisterClass::sampled, 10, DescriptorKind::storageBuffer, 1, stages);
        add(result, NativeSceneRegisterClass::sampled, 11, DescriptorKind::sampledImage, 1, stages);
        break;
    case NativeSceneDescriptorSet::textures:
        result.bindings.reserve(3);
        add(result, NativeSceneRegisterClass::sampled, 0, DescriptorKind::storageBuffer, 1, stages);
        add(result, NativeSceneRegisterClass::sampled, 1, DescriptorKind::sampledImage, counts.textures, stages);
        add(result, NativeSceneRegisterClass::uniform, 0, DescriptorKind::uniformBuffer, 1, stages);
        break;
    case NativeSceneDescriptorSet::vertexBuffers:
        add(result, NativeSceneRegisterClass::sampled, 0, DescriptorKind::storageBuffer, counts.vertexBuffers, stages);
        break;
    case NativeSceneDescriptorSet::indexBuffers:
        add(result, NativeSceneRegisterClass::sampled, 0, DescriptorKind::storageBuffer, counts.indexBuffers, stages);
        break;
    case NativeSceneDescriptorSet::materials:
        add(result, NativeSceneRegisterClass::sampled, 0, DescriptorKind::storageBuffer, counts.materials, stages);
        break;
    case NativeSceneDescriptorSet::faces:
        add(result, NativeSceneRegisterClass::sampled, 0, DescriptorKind::storageBuffer, counts.faces, stages);
        break;
    case NativeSceneDescriptorSet::materialFaces:
        add(result, NativeSceneRegisterClass::sampled, 0, DescriptorKind::storageBuffer, counts.materialFaces, stages);
        break;
    case NativeSceneDescriptorSet::faceWalkers:
        add(result, NativeSceneRegisterClass::sampled, 0, DescriptorKind::storageBuffer, counts.faceWalkers, stages);
        break;
    case NativeSceneDescriptorSet::previousVertices:
        add(result, NativeSceneRegisterClass::sampled, 0, DescriptorKind::storageBuffer, counts.previousVertices,
            stages);
        break;
    case NativeSceneDescriptorSet::rawVertices:
        add(result, NativeSceneRegisterClass::sampled, 0, DescriptorKind::storageBuffer, counts.rawVertices, stages);
        break;
    }
    return result;
}

std::array<DescriptorSetLayoutDesc, kNativeSceneDescriptorSetCount>
nativeSceneDescriptorLayouts(const NativeSceneDescriptorCounts& counts) noexcept {
    std::array<DescriptorSetLayoutDesc, kNativeSceneDescriptorSetCount> result;
    for (std::uint32_t index = 0; index < kNativeSceneDescriptorSetCount; ++index)
        result[index] = nativeSceneDescriptorLayout(static_cast<NativeSceneDescriptorSet>(index), counts);
    return result;
}

} // namespace dayo::graphics
