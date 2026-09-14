#pragma once

#include "graphics/native_scene_data.hpp"
#include "graphics/native_scene_resource_runtime.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace dayo::graphics {

// Owns the per-model storage buffers consumed by resources.hlsli. This is a
// separate owner from NativeSceneResourceStore so descriptor fallback policy
// never takes ownership of scene geometry.
class NativeSceneModelRuntime {
  public:
    NativeSceneModelRuntime() = default;
    ~NativeSceneModelRuntime();

    NativeSceneModelRuntime(const NativeSceneModelRuntime&) = delete;
    NativeSceneModelRuntime& operator=(const NativeSceneModelRuntime&) = delete;

    [[nodiscard]] bool sync(Device& device, std::span<const NativeSceneModelData> models,
                             std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && !vertices_.empty();
    }
    [[nodiscard]] std::size_t modelCount() const noexcept {
        return vertices_.size();
    }
    [[nodiscard]] NativeSceneDescriptorCounts descriptorCounts() const noexcept;
    [[nodiscard]] NativeSceneResourceBindings bindings() const noexcept;

  private:
    Device* device_{};
    std::vector<handles::BufferHandle> vertices_;
    std::vector<handles::BufferHandle> indices_;
    std::vector<handles::BufferHandle> materials_;
    std::vector<handles::BufferHandle> faces_;
    std::vector<handles::BufferHandle> materialFaces_;
    std::vector<handles::BufferHandle> faceWalkers_;
};

} // namespace dayo::graphics
