#pragma once

#include "graphics/dayo_host_resources.hpp"

#include <array>
#include <cstddef>
#include <string>

namespace dayo::graphics {

// Owns the persistent screen resources used by the upstream host contract.
// ScreenBMP is the host-provided background semantic, ScreenTexture is the
// current effect-chain input, and PreviousFrame is the temporal history.
// Each in-flight slot has its own handles so descriptor updates never race a
// command buffer submitted by the previous slot.
class NativeScreenRuntime {
  public:
    NativeScreenRuntime() = default;
    ~NativeScreenRuntime();

    NativeScreenRuntime(const NativeScreenRuntime&) = delete;
    NativeScreenRuntime& operator=(const NativeScreenRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, Extent3D extent, std::string* error = nullptr);
    [[nodiscard]] bool ready() const noexcept;
    void reset() noexcept;

    [[nodiscard]] handles::TextureHandle screenBmp() const noexcept;
    [[nodiscard]] handles::TextureHandle screenTexture() const noexcept;
    [[nodiscard]] handles::TextureHandle previousFrame() const noexcept;

    // Copies the completed effect-chain output into the current slot's
    // persistent history. The caller remains responsible for ensuring the
    // source is in a transfer-readable state.
    void rotatePreviousFrame(CommandList& commands, handles::TextureHandle currentFinal);

    // Adds only the three screen semantics owned by this runtime. Other host
    // bindings remain untouched.
    void bindScreenSemantics(NativeSceneResourceBindings& bindings) const noexcept;

  private:
    struct Slot {
        handles::TextureHandle screenBmp{};
        handles::TextureHandle screenTexture{};
        handles::TextureHandle previousFrame{};
    };

    Device* device_{};
    Extent3D extent_{};
    std::array<Slot, kNativeFramesInFlight> slots_{};

    [[nodiscard]] const Slot& currentSlot() const noexcept;
};

} // namespace dayo::graphics
