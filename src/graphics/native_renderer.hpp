#pragma once

#include "core/effect.hpp"
#include "graphics/native_renderer_requirements.hpp"
#include "graphics/bdpt_runtime.hpp"
#include "graphics/subayai_runtime.hpp"

#include <string>

namespace dayo::graphics {

// Application-facing lifecycle owner for native Subayai/BDPT activation.
// Resource binding and command recording stay in the renderer runtimes; this
// class makes effect loading, feature checks, initialization, and fallback one
// atomic decision so Application never leaves the device claiming an
// uninitialized native renderer.
class NativeRendererCoordinator {
  public:
    NativeRendererCoordinator() = default;
    ~NativeRendererCoordinator() = default;

    NativeRendererCoordinator(const NativeRendererCoordinator&) = delete;
    NativeRendererCoordinator& operator=(const NativeRendererCoordinator&) = delete;

    [[nodiscard]] NativeRendererStatus prepare(Device& device, RendererKind requested,
                                                const core::EffectGraph& graph);
    [[nodiscard]] NativeRendererStatus prepare(Device& device, RendererKind requested, fx::FxProgram program);
    void reset() noexcept;

    [[nodiscard]] const NativeRendererStatus& status() const noexcept {
        return status_;
    }
    [[nodiscard]] const fx::FxProgram* program() const noexcept;
    [[nodiscard]] SubayaiRuntime* subayai() noexcept {
        return status_.nativeReady && status_.active == RendererKind::subayai ? &subayai_ : nullptr;
    }
    [[nodiscard]] BdptRuntime* bdpt() noexcept {
        return status_.nativeReady && status_.active == RendererKind::bdpt ? &bdpt_ : nullptr;
    }

  private:
    NativeRendererStatus status_{};
    SubayaiRuntime subayai_;
    BdptRuntime bdpt_;
};

} // namespace dayo::graphics
