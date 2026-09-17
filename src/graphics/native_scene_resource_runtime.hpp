#pragma once

#include "graphics/native_scene_binding_runtime.hpp"

#include <span>
#include <string>

namespace dayo::graphics {

// Handles consumed by the upstream resources.hlsli declarations. Resource
// ownership stays with the scene/renderer runtimes; this value only describes
// the complete descriptor contents for one frame.
struct NativeSceneResourceBindings {
    handles::TextureHandle rtOutput{};
    handles::BufferHandle oidnBuffer{};
    handles::TextureHandle normalDepth{};
    handles::TextureHandle gbuffer1{};
    handles::TextureHandle gbuffer2{};
    handles::AccelerationStructureHandle tlas{};
    handles::BufferHandle modelToMaterial{};
    handles::BufferHandle materialToModel{};
    handles::BufferHandle peekaboo{};
    handles::BufferHandle materialSelected{};
    handles::TextureHandle skybox{};
    handles::BufferHandle skywalker{};
    handles::BufferHandle skywalkerRow{};
    handles::BufferHandle skyboxSh{};
    handles::TextureHandle screenBmp{};
    handles::BufferHandle cloneCount{};
    handles::TextureHandle screenTexture{};
    handles::BufferHandle viewConstants{};
    handles::BufferHandle controllerConstants{};

    handles::BufferHandle textureTable{};
    std::span<const handles::TextureHandle> textures{};
    handles::BufferHandle passConstants{};

    std::span<const handles::BufferHandle> vertexBuffers{};
    std::span<const handles::BufferHandle> indexBuffers{};
    std::span<const handles::BufferHandle> materials{};
    std::span<const handles::BufferHandle> faces{};
    std::span<const handles::BufferHandle> materialFaces{};
    std::span<const handles::BufferHandle> faceWalkers{};
    std::span<const handles::BufferHandle> previousVertices{};
    std::span<const handles::BufferHandle> rawVertices{};
};

// Converts scene-owned handles into complete descriptor sets for native FX.
// The runtime arrays must match NativeSceneDescriptorCounts exactly; callers
// should provide a valid placeholder resource for an intentionally empty
// array because the Vulkan layout uses fixed descriptor counts.
class NativeSceneResourceRuntime {
  public:
    NativeSceneResourceRuntime() = default;
    ~NativeSceneResourceRuntime() = default;

    NativeSceneResourceRuntime(const NativeSceneResourceRuntime&) = delete;
    NativeSceneResourceRuntime& operator=(const NativeSceneResourceRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, const NativeSceneDescriptorCounts& counts = {},
                                  std::string* error = nullptr) {
        return bindings_.initialize(device, counts, error);
    }
    [[nodiscard]] bool sync(const NativeSceneResourceBindings& resources, std::string* error = nullptr);
    void reset() noexcept {
        bindings_.reset();
    }

    [[nodiscard]] bool ready() const noexcept {
        return bindings_.ready();
    }
    [[nodiscard]] const NativeSceneDescriptorCounts& counts() const noexcept {
        return bindings_.counts();
    }
    [[nodiscard]] std::span<const handles::DescriptorSetLayoutHandle> layouts() const noexcept {
        return bindings_.layouts();
    }
    [[nodiscard]] std::span<const handles::DescriptorSetHandle> descriptorSets() const noexcept {
        return bindings_.descriptorSets();
    }
    [[nodiscard]] handles::DescriptorSetLayoutHandle layout(NativeSceneDescriptorSet set) const noexcept {
        return bindings_.layout(set);
    }
    [[nodiscard]] handles::DescriptorSetHandle descriptorSet(NativeSceneDescriptorSet set) const noexcept {
        return bindings_.descriptorSet(set);
    }

  private:
    NativeSceneBindingRuntime bindings_;
};

} // namespace dayo::graphics
