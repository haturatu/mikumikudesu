#pragma once

#include "graphics/native_scene_data.hpp"
#include "graphics/native_scene_resource_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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

    [[nodiscard]] bool sync(Device& device, std::span<const NativeSceneModelData> models, std::string* error = nullptr);
    [[nodiscard]] bool update(Device& device, std::span<const NativeSceneModelData> models,
                              std::string* error = nullptr);
    // Records animated and dirty static buffer transfers into the current
    // frame. CPU-visible staging writes are non-blocking; the device-local
    // copies are ordered with the native passes by the command-list barrier.
    [[nodiscard]] bool updateFrame(Device& device, CommandList& commands, std::span<const NativeSceneModelData> models,
                                   std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && !frameResources_[0].vertices.empty();
    }
    [[nodiscard]] std::size_t modelCount() const noexcept {
        return vertexBytes_.size();
    }
    [[nodiscard]] NativeSceneDescriptorCounts descriptorCounts() const noexcept;
    [[nodiscard]] NativeSceneResourceBindings bindings() const noexcept;

  private:
    struct StaticHashes {
        std::uint64_t indices{};
        std::uint64_t materials{};
        std::uint64_t faces{};
        std::uint64_t materialFaces{};
        std::uint64_t faceWalker{};
    };

    struct FrameResources {
        std::vector<handles::BufferHandle> vertices;
        std::vector<handles::BufferHandle> previousVertices;
        std::vector<handles::BufferHandle> rawVertices;
        std::vector<handles::BufferHandle> vertexStaging;
        std::vector<handles::BufferHandle> indices;
        std::vector<handles::BufferHandle> indexStaging;
        std::vector<handles::BufferHandle> materials;
        std::vector<handles::BufferHandle> materialStaging;
        std::vector<handles::BufferHandle> faces;
        std::vector<handles::BufferHandle> faceStaging;
        std::vector<handles::BufferHandle> materialFaces;
        std::vector<handles::BufferHandle> materialFaceStaging;
        std::vector<handles::BufferHandle> faceWalkers;
        std::vector<handles::BufferHandle> faceWalkerStaging;
        std::vector<StaticHashes> staticHashes;
    };

    [[nodiscard]] bool sameLayout(Device& device, std::span<const NativeSceneModelData> models) const noexcept;
    [[nodiscard]] static StaticHashes makeStaticHashes(const NativeSceneModelData& model) noexcept;
    [[nodiscard]] std::size_t currentSlot() const noexcept {
        return device_ == nullptr ? 0 : device_->currentFrameSlot() % kNativeFramesInFlight;
    }

    Device* device_{};
    std::array<FrameResources, kNativeFramesInFlight> frameResources_;
    std::vector<std::size_t> vertexBytes_;
    std::vector<std::size_t> indexBytes_;
    std::vector<std::size_t> materialBytes_;
    std::vector<std::size_t> faceBytes_;
    std::vector<std::size_t> materialFaceBytes_;
    std::vector<std::size_t> faceWalkerBytes_;
};

} // namespace dayo::graphics
