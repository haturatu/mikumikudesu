#pragma once

#include "core/fx/fx_material.hpp"
#include "graphics/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dayo::graphics {

struct FxMaterialGpuBindings {
    handles::BufferHandle materialIndices{};
    handles::BufferHandle textureIndices2D{};
    handles::BufferHandle textureIndices3D{};
    handles::BufferHandle values{};
    std::span<const handles::TextureHandle> textures2D;
    std::span<const handles::TextureHandle> textures3D;
};

// Owns GPU storage and sampled textures for one generic MatDesc table. The
// pass binding plan supplies descriptor locations; this owner supplies the
// physical resources and keeps dynamic table buffers frame-slot safe.
class FxMaterialGpuRuntime {
  public:
    FxMaterialGpuRuntime() = default;
    ~FxMaterialGpuRuntime();

    FxMaterialGpuRuntime(const FxMaterialGpuRuntime&) = delete;
    FxMaterialGpuRuntime& operator=(const FxMaterialGpuRuntime&) = delete;

    [[nodiscard]] bool sync(Device& device, const core::fx::MaterialGpuTableData& table, std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] FxMaterialGpuBindings bindings() const noexcept;
    [[nodiscard]] std::span<const handles::TextureHandle> textures2D() const noexcept;
    [[nodiscard]] std::span<const handles::TextureHandle> textures3D() const noexcept;

  private:
    struct FrameBuffers {
        handles::BufferHandle materialIndices{};
        handles::BufferHandle textureIndices2D{};
        handles::BufferHandle textureIndices3D{};
        handles::BufferHandle values{};
        std::array<std::size_t, 4> capacity{};
    };

    [[nodiscard]] bool ensureFallbackTextures(std::string* error);
    [[nodiscard]] bool ensureTextureCatalog(std::span<const core::fx::MaterialTextureDesc> textures,
                                            core::fx::MaterialTextureDimension dimension, std::string* error);
    [[nodiscard]] bool ensureTableBuffers(const core::fx::MaterialGpuTableData& table, std::string* error);
    [[nodiscard]] bool uploadFrame(const core::fx::MaterialGpuTableData& table, std::size_t frameSlot,
                                   std::string* error);
    void destroyBuffers(std::span<FrameBuffers> buffers) noexcept;
    void destroyTextures(std::span<handles::TextureHandle> textures) noexcept;

    Device* device_{};
    std::array<FrameBuffers, kNativeFramesInFlight> frameBuffers_{};
    handles::TextureHandle fallbackTexture2D_{};
    handles::TextureHandle fallbackTexture3D_{};
    std::vector<handles::TextureHandle> textures2D_;
    std::vector<handles::TextureHandle> textures3D_;
    std::vector<handles::TextureHandle> textureBindings2D_;
    std::vector<handles::TextureHandle> textureBindings3D_;
    std::vector<std::string> textureKeys2D_;
    std::vector<std::string> textureKeys3D_;
};

} // namespace dayo::graphics
