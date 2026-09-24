#pragma once

#include "core/effect.hpp"
#include "fx/fx_compiler.hpp"
#include "graphics/dayo_fx_runtime.hpp"
#include "graphics/fx_executor.hpp"
#include "graphics/fx_material_scene_runtime.hpp"
#include "graphics/native_scene_frame_runtime.hpp"
#include "graphics/subayai_bindings.hpp"
#include "graphics/subayai_environment.hpp"
#include "graphics/subayai_environment_runtime.hpp"
#include "graphics/subayai_geometry.hpp"
#include "graphics/subayai_light_sampling.hpp"
#include "graphics/subayai_material_gpu.hpp"
#include "graphics/subayai_material_runtime.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dayo::graphics {

struct SubayaiFrame {
    fx::FxFrameContext context;
    fx::FxFramePlan plan;
    std::vector<SubayaiMaterialGpu> materials;
    handles::BufferHandle materialBuffer{};
    handles::DescriptorSetHandle materialDescriptorSet{};
    std::vector<AliasEntry> lightSampling;
    handles::BufferHandle lightSamplingBuffer{};
    handles::DescriptorSetHandle lightSamplingDescriptorSet{};
    handles::DescriptorSetHandle geometryDescriptorSet{};
    handles::DescriptorSetHandle environmentDescriptorSet{};
    bool usesCanonicalSceneBindings{};
    EnvironmentGpuResult environment;
    std::optional<NativeFxFrame> nativeFx;
};

// Native Subayai runtime. It validates compiled effect requirements against
// the device before exposing execution, and keeps the Preview path entirely
// outside this object. The actual command recording is supplied by the typed
// Vulkan command list.
class SubayaiRuntime {
  public:
    bool initialize(Device& device, fx::FxProgram program, std::string* error = nullptr);
    void reset() noexcept;

    // When supplied, the upstream resources.hlsli descriptor ABI occupies
    // sets 0..9 and the FX-local set is appended at set 10. A null provider
    // keeps the small legacy binding layout available to older callers.
    void setSceneFrameRuntime(NativeSceneFrameRuntime* runtime) noexcept {
        sceneFrame_ = runtime;
    }
    void setExternalResourceProvider(FxExternalResourceProvider* provider) noexcept {
        externalResourceProvider_ = provider;
        dayoFx_.clearProviders();
        if (provider != nullptr)
            dayoFx_.addProvider(*provider);
    }
    void setControllerDeclarations(std::span<const core::EffectController> declarations) {
        controllerDeclarations_.assign(declarations.begin(), declarations.end());
    }

    [[nodiscard]] bool ready() const noexcept {
        return ready_;
    }
    [[nodiscard]] bool nativeReady() const noexcept {
        return dayoFx_.ready();
    }
    [[nodiscard]] const fx::FxProgram* program() const noexcept {
        return ready_ ? &program_ : nullptr;
    }
    [[nodiscard]] const FxResourceStore* liveResourceStore() const noexcept {
        const auto& resources = dayoFx_.nativeRuntime().resources();
        return resources.ready() ? &resources.store() : nullptr;
    }
    [[nodiscard]] bool syncMaterials(std::span<const core::MaterialParameterBlock> materials);
    void setEnvironmentBackend(IEnvironmentBackend* backend) noexcept {
        environmentService_ = EnvironmentService(backend);
    }
    [[nodiscard]] bool updateEnvironment(const EnvironmentDesc& description);
    void recordEnvironment(CommandList& commands) const {
        environmentService_.record(commands);
    }
    [[nodiscard]] const EnvironmentGpuResult& environment() const noexcept {
        return environmentService_.gpuResult();
    }
    [[nodiscard]] bool syncGeometry(std::span<const NativeGeometryMeshUpload> meshes, std::string* error = nullptr);
    void recordGeometry(CommandList& commands) const;
    void recordGeometry(CommandList& commands, std::span<const NativeGeometryMeshUpload> meshes);
    void recordAcceleration(CommandList& commands) const;
    [[nodiscard]] bool synchronizeAcceleration(std::string* error = nullptr);
    [[nodiscard]] TlasAction synchronizeWorld(std::uint64_t worldGeneration, std::span<const WorldInstance> instances);
    [[nodiscard]] const NativeGeometryRuntime& geometry() const noexcept {
        return geometry_;
    }
    [[nodiscard]] const SubayaiBindingLayouts& bindingLayouts() const noexcept {
        return bindings_.layouts();
    }
    [[nodiscard]] SubayaiFrame prepareFrame(const fx::FxFrameContext& context,
                                            std::span<const core::MaterialParameterBlock> materials,
                                            std::span<const AliasEntry> lightSampling,
                                            const EnvironmentGpuResult& environment,
                                            std::span<const FxMaterialSceneModel> materialModels = {},
                                            const FxMaterialTextureResolver& textureResolver = {});
    [[nodiscard]] VulkanFxExecutor::Stats execute(SubayaiFrame& frame, CommandList& commands,
                                                  const FxExecutionResources& resources = {}) const;
    [[nodiscard]] std::optional<NativeFrameOutput> output(const SubayaiFrame& frame) const;

  private:
    Device* device_{};
    fx::FxProgram program_;
    std::vector<SubayaiMaterialGpu> materials_;
    SubayaiMaterialGpuRuntime materialRuntime_;
    FxMaterialSceneRuntime materialSceneRuntime_;
    LightSamplingGpuRuntime lightRuntime_;
    SubayaiBindingRuntime bindings_;
    NativeGeometryRuntime geometry_;
    SubayaiEnvironmentRuntime environmentRuntime_;
    EnvironmentService environmentService_{nullptr};
    DayoFxRuntime dayoFx_;
    FxExternalResourceProvider* externalResourceProvider_{};
    std::vector<core::EffectController> controllerDeclarations_;
    NativeSceneFrameRuntime* sceneFrame_{};
    bool nativeAttempted_{};
    bool ready_{};
};

} // namespace dayo::graphics
