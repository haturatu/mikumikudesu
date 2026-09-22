#pragma once

#include "core/scene.hpp"
#include "fx/fx_compiler.hpp"
#include "graphics/bdpt_accumulation.hpp"
#include "graphics/dayo_fx_runtime.hpp"
#include "graphics/fx_executor.hpp"
#include "graphics/native_scene_frame_runtime.hpp"
#include "graphics/subayai_bindings.hpp"
#include "graphics/subayai_geometry.hpp"
#include "graphics/subayai_light_sampling.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dayo::graphics {

struct BdptFrame {
    fx::FxFrameContext context;
    fx::FxFramePlan plan;
    BdptAccumulation::GpuResources gpu;
    std::uint32_t sampleIndex{};
    bool clearAccumulation{};
    handles::DescriptorSetHandle descriptorSet{};
    handles::BufferHandle lightSamplingBuffer{};
    handles::DescriptorSetHandle lightSamplingDescriptorSet{};
    handles::DescriptorSetHandle geometryDescriptorSet{};
    bool usesCanonicalSceneBindings{};
    std::optional<NativeFxFrame> nativeFx;
};

// Stable BDPT resource ABI. Binding 0 is the progressive output, bindings 1
// and 2 are lookup buffers, and bindings 3..10 are the persistent volume
// slots. The native FX set is appended after this set in the pipeline layout.
[[nodiscard]] DescriptorSetLayoutDesc bdptResourceBindingLayout() noexcept;

// Progressive native BDPT runtime. The runtime owns frame-independent
// accumulation resources and only exposes execution after the compiled graph
// and the device capabilities agree on the full ray-tracing contract.
class BdptRuntime {
  public:
    BdptRuntime() = default;

    bool initialize(Device& device, fx::FxProgram program, std::string* error = nullptr);
    void reset() noexcept;
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
    [[nodiscard]] BdptAccumulation& accumulation() noexcept {
        return accumulation_;
    }
    [[nodiscard]] const BdptAccumulation& accumulation() const noexcept {
        return accumulation_;
    }

    // Allocates persistent accumulation, LUT and volume resources for the
    // requested frame extent. Repeated calls with the same extent reuse them.
    [[nodiscard]] bool ensureResources(std::uint32_t width, std::uint32_t height, std::string* error = nullptr);
    [[nodiscard]] bool syncGeometry(std::span<const NativeGeometryMeshUpload> meshes, std::string* error = nullptr);
    void recordGeometry(CommandList& commands) const;
    void recordGeometry(CommandList& commands, std::span<const NativeGeometryMeshUpload> meshes);
    void recordAcceleration(CommandList& commands) const;
    [[nodiscard]] bool synchronizeAcceleration(std::string* error = nullptr);
    [[nodiscard]] TlasAction synchronizeWorld(std::uint64_t worldGeneration, std::span<const WorldInstance> instances);
    [[nodiscard]] const NativeGeometryRuntime& geometry() const noexcept {
        return geometry_;
    }

    // Scene dirty state controls whether the progressive target is cleared or
    // the next sample is accumulated.
    [[nodiscard]] BdptFrame prepareFrame(const fx::FxFrameContext& context, core::DirtyFlag dirty,
                                         std::span<const AliasEntry> lightSampling = {});
    [[nodiscard]] VulkanFxExecutor::Stats execute(BdptFrame& frame, CommandList& commands,
                                                  const FxExecutionResources& resources = {}) const;
    [[nodiscard]] std::optional<NativeFrameOutput> output(const BdptFrame& frame) const;

  private:
    Device* device_{nullptr};
    fx::FxProgram program_;
    BdptAccumulation accumulation_;
    NativeGeometryRuntime geometry_;
    SubayaiBindingRuntime bindings_;
    LightSamplingGpuRuntime lightRuntime_;
    handles::DescriptorSetLayoutHandle descriptorLayout_{};
    std::array<handles::DescriptorSetHandle, kNativeFramesInFlight> descriptorSets_{};
    DayoFxRuntime dayoFx_;
    FxExternalResourceProvider* externalResourceProvider_{};
    std::vector<core::EffectController> controllerDeclarations_;
    NativeSceneFrameRuntime* sceneFrame_{};
    bool nativeAttempted_{};
    bool ready_{false};
};

} // namespace dayo::graphics
