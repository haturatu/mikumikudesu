#pragma once

#include "core/effect.hpp"
#include "core/fx/fx_controller_resolver.hpp"
#include "core/scene.hpp"
#include "fx/fx_scheduler.hpp"
#include "graphics/bdpt_runtime.hpp"
#include "graphics/dayo_fx_runtime.hpp"
#include "graphics/deformer_resource_registry.hpp"
#include "graphics/fx_shared_resource_registry.hpp"
#include "graphics/native_fx_global_variable_runtime.hpp"
#include "graphics/native_renderer_requirements.hpp"
#include "graphics/native_scene_frame_runtime.hpp"
#include "graphics/output_sample_accumulator.hpp"
#include "graphics/subayai_runtime.hpp"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::graphics {

struct NativeFrameExecution {
    std::uint32_t sampleIndex{};
    std::uint32_t sampleCount{1};
};

struct NativeFxResourceSnapshot {
    std::string effect;
    std::string name;
    std::string kind;
    std::string format;
    Extent3D extent{};
    std::uint32_t dimension{};
    std::uint64_t allocationBytes{};
    std::uint32_t elementSize{};
    std::string elementType;
};

[[nodiscard]] std::vector<NativeFxResourceSnapshot> snapshotFxResources(std::string_view effect,
                                                                        const FxResourceStore& store);

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

    [[nodiscard]] NativeRendererStatus prepare(Device& device, RendererKind requested, const core::EffectGraph& graph);
    [[nodiscard]] NativeRendererStatus prepare(Device& device, RendererKind requested, fx::FxProgram program);
    void setEnvironmentBackend(IEnvironmentBackend* backend) noexcept;
    void setSceneFrameRuntime(NativeSceneFrameRuntime* runtime) noexcept;
    void setHostResourceBindings(const NativeSceneResourceBindings& bindings) noexcept;
    void setEffectStack(const core::SceneEffectStack& effects);
    void setEffectSchedule(std::span<const fx::ScheduledFx> schedule);
    void setControllerDeclarations(std::span<const core::EffectController> declarations);
    void setEvaluationSnapshot(const core::fx::SceneEvaluationSnapshot* snapshot) noexcept {
        evaluationSnapshot_ = snapshot;
    }
    [[nodiscard]] bool updateEnvironment(const EnvironmentDesc& description);
    void clearEnvironment() noexcept {
        environmentService_.clear();
    }
    [[nodiscard]] const EnvironmentGpuResult& environment() const noexcept {
        return environmentService_.gpuResult();
    }
    void reset() noexcept;

    [[nodiscard]] const NativeRendererStatus& status() const noexcept {
        return status_;
    }
    [[nodiscard]] const DeformerResourceRegistry& deformerResources() const noexcept {
        return deformerResources_;
    }
    [[nodiscard]] std::vector<NativeFxResourceSnapshot> liveResources() const;
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
                const EnvironmentGpuResult& environment, const FxExecutionResources& resources = {},
                NativeFrameExecution execution = {}, std::span<const FxMaterialSceneModel> materialModels = {});

  private:
    struct GenericEffectRuntime {
        Device* device{};
        fx::FxProgram program;
        FxMaterialSceneRuntime materialScene;
        DayoFxRuntime runtime;
        NativeControllerRuntime controller;
        NativeFxGlobalVariableRuntime globalVariables;
        std::optional<NativeControllerBlock> block;
        std::array<handles::DescriptorSetHandle, kNativeFramesInFlight> frameSets{};
        ~GenericEffectRuntime();
    };
    using GenericRuntimeList = std::vector<std::unique_ptr<GenericEffectRuntime>>;

    [[nodiscard]] std::optional<NativeFrameOutput>
    executeGenericEffects(std::span<const core::SceneEffectInstance> effects, GenericRuntimeList& runtimes,
                          CommandList& commands, const fx::FxFrameContext& context,
                          const FxExecutionResources& resources, bool publishToScreen,
                          std::span<const FxMaterialSceneModel> materialModels);
    [[nodiscard]] FxMaterialTextureResolver materialTextureResolver(const FxResourceStore* localStore,
                                                                    bool rendererLocal,
                                                                    std::string_view effectIdentity = {}) const;
    void publishActiveRendererResources();
    [[nodiscard]] FxSharedResourceResolver sharedResourceResolver() const;

    NativeRendererStatus status_{};
    SubayaiRuntime subayai_;
    BdptRuntime bdpt_;
    IEnvironmentBackend* environmentBackend_{};
    EnvironmentService environmentService_{nullptr};
    NativeSceneFrameRuntime* sceneFrameRuntime_{};
    std::optional<DayoHostResourceProvider> hostResourceProvider_;
    NativeSceneResourceBindings hostBindings_;
    DayoSceneHostProvider sceneHostProvider_;
    Device* device_{};
    std::vector<core::SceneEffectInstance> deformEffects_;
    std::vector<core::SceneEffectInstance> postprocessEffects_;
    std::vector<core::EffectController> controllerDeclarations_;
    GenericRuntimeList deformRuntimes_;
    GenericRuntimeList postprocessRuntimes_;
    OutputSampleAccumulator outputSamples_;
    DeformerResourceRegistry deformerResources_;
    FxSharedResourceRegistry sharedResources_;
    std::string rendererSharedResourceOwner_;
    const core::fx::SceneEvaluationSnapshot* evaluationSnapshot_{};
};

} // namespace dayo::graphics
