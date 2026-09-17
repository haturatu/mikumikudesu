#pragma once

#include "graphics/native_scene_resource_runtime.hpp"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace dayo::graphics {

// Owns harmless 1x1/one-element fallback resources for the fixed upstream
// scene ABI. Real scene resources can replace any field at frame sync time;
// TLAS is intentionally excluded from fallback because a fake acceleration
// structure would hide a missing geometry synchronization bug.
class NativeSceneResourceStore {
  public:
    NativeSceneResourceStore() = default;
    ~NativeSceneResourceStore();

    NativeSceneResourceStore(const NativeSceneResourceStore&) = delete;
    NativeSceneResourceStore& operator=(const NativeSceneResourceStore&) = delete;

    [[nodiscard]] bool initialize(Device& device, NativeSceneDescriptorCounts counts = {},
                                  std::string* error = nullptr);
    // Resolves caller-owned handles into a complete binding value. Empty
    // optional fields use owned placeholders; non-empty arrays must match the
    // fixed descriptor counts exactly and are copied into store-owned spans.
    [[nodiscard]] bool compose(const NativeSceneResourceBindings& overrides, std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && placeholderTexture_.valid() && placeholderBuffer_.valid();
    }
    [[nodiscard]] const NativeSceneDescriptorCounts& counts() const noexcept {
        return counts_;
    }
    [[nodiscard]] const NativeSceneResourceBindings& bindings() const noexcept {
        return bindings_;
    }

  private:
    [[nodiscard]] bool replaceArrays(const NativeSceneResourceBindings& overrides, std::string* error);
    [[nodiscard]] bool replaceScalars(const NativeSceneResourceBindings& overrides, std::string* error);
    [[nodiscard]] handles::TextureHandle textureOrPlaceholder(handles::TextureHandle value) const noexcept;
    [[nodiscard]] handles::BufferHandle bufferOrPlaceholder(handles::BufferHandle value) const noexcept;

    Device* device_{};
    NativeSceneDescriptorCounts counts_{};
    handles::TextureHandle placeholderTexture_{};
    handles::BufferHandle placeholderBuffer_{};
    NativeSceneResourceBindings bindings_{};
    std::vector<handles::TextureHandle> textures_;
    std::vector<handles::BufferHandle> vertexBuffers_;
    std::vector<handles::BufferHandle> indexBuffers_;
    std::vector<handles::BufferHandle> materials_;
    std::vector<handles::BufferHandle> faces_;
    std::vector<handles::BufferHandle> materialFaces_;
    std::vector<handles::BufferHandle> faceWalkers_;
    std::vector<handles::BufferHandle> previousVertices_;
    std::vector<handles::BufferHandle> rawVertices_;
};

} // namespace dayo::graphics
