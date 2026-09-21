#pragma once

#include "core/image.hpp"
#include "graphics/dayo_host_resources.hpp"

#include <cstddef>
#include <string>

namespace dayo::graphics {

enum class NativeScreenSource {
    previousFrame,
    external,
    white,
};

enum class NativeScreenCrop {
    none,
    crop4x3,
};

// Owns the persistent screen resources used by the upstream host contract.
// ScreenBMP is the host-provided background semantic, ScreenTexture is the
// current effect-chain input, and PreviousFrame is the temporal history.
// PreviousFrame is one logical resource. It is deliberately not indexed by
// the frame-in-flight slot: slot rotation would expose frame N-2 as the
// previous frame when two graphics submissions overlap.
class NativeScreenRuntime {
  public:
    NativeScreenRuntime() = default;
    ~NativeScreenRuntime();

    NativeScreenRuntime(const NativeScreenRuntime&) = delete;
    NativeScreenRuntime& operator=(const NativeScreenRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, Extent3D extent, std::string* error = nullptr);
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool matchesExtent(Extent3D extent) const noexcept;
    void reset() noexcept;

    [[nodiscard]] handles::TextureHandle screenBmp() const noexcept;
    [[nodiscard]] handles::TextureHandle screenTexture() const noexcept;
    [[nodiscard]] handles::TextureHandle previousFrame() const noexcept;

    // Prepares the host resources for a new frame. ScreenTexture always starts
    // from the previous completed effect-chain output. ScreenBMP is either
    // the same history, a previously uploaded external image, or white.
    void prepareFrame(CommandList& commands, NativeScreenSource source, bool enabled = true);

    // Uploads an RGBA8 source into the fixed-size native ScreenBMP texture.
    // Crop is applied before resampling so the resulting image has the same
    // screen.bmp semantics as the upstream system shader.
    [[nodiscard]] bool uploadScreenBmp(const core::ImageRgba8& image, NativeScreenCrop crop,
                                       std::string* error = nullptr);

    // Publishes the completed effect-chain output as the next frame's history
    // and as the current ScreenTexture input. The caller records this after
    // all passes that produce currentFinal.
    void publishFrame(CommandList& commands, handles::TextureHandle currentFinal);

    // Compatibility name retained for callers/tests that only exercise the
    // history rotation operation.
    void rotatePreviousFrame(CommandList& commands, handles::TextureHandle currentFinal);

    // Adds only the three screen semantics owned by this runtime. Other host
    // bindings remain untouched.
    void bindScreenSemantics(NativeSceneResourceBindings& bindings) const noexcept;

  private:
    Device* device_{};
    Extent3D extent_{};
    handles::TextureHandle screenBmp_{};
    handles::TextureHandle screenTexture_{};
    handles::TextureHandle previousFrame_{};
};

} // namespace dayo::graphics
