#pragma once

#include "core/effect.hpp"
#include "graphics/native_renderer_requirements.hpp"
#include "graphics/bdpt_runtime.hpp"
#include "graphics/subayai_runtime.hpp"

#include <optional>
#include <span>
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
    void setEnvironmentBackend(IEnvironmentBackend* backend) noexcept;
    [[nodiscard]] bool updateEnvironment(const EnvironmentDesc& description);
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
    [[nodiscard]] std::optional<NativeFrameOutput>
    recordFrame(CommandList& commands, const fx::FxFrameContext& context, core::DirtyFlag dirty,
                std::span<const core::MaterialParameterBlock> materials, std::span<const AliasEntry> lightSampling,
                const EnvironmentGpuResult& environment);

  private:
    NativeRendererStatus status_{};
    SubayaiRuntime subayai_;
    BdptRuntime bdpt_;
    IEnvironmentBackend* environmentBackend_{};
};

} // namespace dayo::graphics
