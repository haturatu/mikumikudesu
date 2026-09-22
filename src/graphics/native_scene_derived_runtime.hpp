#pragma once

#include "core/image.hpp"
#include "graphics/native_scene_resource_runtime.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dayo::graphics {

struct NativeSceneDerivedModel {
    std::uint32_t materialCount{};
    std::uint32_t textureBase{};
    std::uint32_t cloneCount{1};
    std::int32_t selectedMaterial{-1};
    bool visible{true};
};

// CPU representation of the scene tables declared by upstream resources.hlsli.
// Vector order is the model/material order used by NativeSceneModelRuntime.
struct NativeSceneDerivedData {
    std::vector<std::array<std::uint32_t, 2>> modelToMaterial;
    std::vector<std::uint32_t> materialToModel;
    std::vector<std::int32_t> peekaboo;
    std::vector<std::int32_t> materialSelected;
    std::vector<std::uint32_t> cloneCount;
    std::vector<std::uint32_t> textureTable;
};

[[nodiscard]] NativeSceneDerivedData makeNativeSceneDerivedData(std::span<const NativeSceneDerivedModel> models);

// Owns concrete PMX textures and per-frame GPU copies of the canonical
// model/material lookup tables consumed through resources.hlsli.
class NativeSceneDerivedRuntime {
  public:
    NativeSceneDerivedRuntime() = default;
    ~NativeSceneDerivedRuntime();

    NativeSceneDerivedRuntime(const NativeSceneDerivedRuntime&) = delete;
    NativeSceneDerivedRuntime& operator=(const NativeSceneDerivedRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, std::span<const core::ImageRgba8> images,
                                  std::string* error = nullptr);
    [[nodiscard]] bool sync(std::span<const NativeSceneDerivedModel> models, Extent3D outputExtent,
                            std::string* error = nullptr);
    void apply(NativeSceneResourceBindings& bindings) const noexcept;
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && !textures_.empty();
    }
    [[nodiscard]] const NativeSceneDerivedData& data() const noexcept {
        return data_;
    }
    [[nodiscard]] std::span<const handles::TextureHandle> textures() const noexcept {
        return textures_;
    }

  private:
    struct FrameBuffers {
        handles::BufferHandle modelToMaterial{};
        handles::BufferHandle materialToModel{};
        handles::BufferHandle peekaboo{};
        handles::BufferHandle materialSelected{};
        handles::BufferHandle cloneCount{};
        handles::BufferHandle textureTable{};
        std::array<std::size_t, 6> capacity{};
        handles::TextureHandle rtOutput{};
        handles::BufferHandle oidnBuffer{};
        handles::TextureHandle normalDepth{};
        handles::TextureHandle gbuffer1{};
        handles::TextureHandle gbuffer2{};
        Extent3D outputExtent{};
    };

    [[nodiscard]] bool ensureTableBufferCapacity(const NativeSceneDerivedData& data, std::string* error);
    [[nodiscard]] bool createTableBuffers(std::string* error);
    [[nodiscard]] bool ensureOutputResources(Extent3D extent, std::string* error);
    [[nodiscard]] bool createOutputResources(Extent3D extent, std::string* error);
    void destroyOwnedResources() noexcept;

    Device* device_{};
    handles::TextureHandle fallbackTexture_{};
    std::vector<handles::TextureHandle> textures_;
    std::vector<handles::TextureHandle> ownedTextures_;
    std::array<FrameBuffers, kNativeFramesInFlight> frameBuffers_{};
    NativeSceneDerivedData data_;
};

} // namespace dayo::graphics
