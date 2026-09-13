#pragma once

#include "core/effect.hpp"
#include "fx/fx_compiler.hpp"
#include "graphics/fx_executor.hpp"
#include "graphics/subayai_environment.hpp"
#include "graphics/subayai_light_sampling.hpp"
#include "graphics/subayai_material_gpu.hpp"
#include "graphics/subayai_material_runtime.hpp"

#include <span>
#include <string>
#include <vector>

namespace dayo::graphics {

struct SubayaiFrame {
    fx::FxFrameContext context;
    fx::FxFramePlan plan;
    std::vector<SubayaiMaterialGpu> materials;
    handles::BufferHandle materialBuffer{};
    std::vector<AliasEntry> lightSampling;
    EnvironmentGpuResult environment;
};

// Native Subayai runtime. It validates compiled effect requirements against
// the device before exposing execution, and keeps the Preview path entirely
// outside this object. The actual command recording is supplied by the typed
// Vulkan command list.
class SubayaiRuntime {
  public:
    bool initialize(Device& device, fx::FxProgram program, std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return ready_;
    }
    [[nodiscard]] const fx::FxProgram* program() const noexcept {
        return ready_ ? &program_ : nullptr;
    }
    [[nodiscard]] bool syncMaterials(std::span<const core::MaterialParameterBlock> materials);
    [[nodiscard]] SubayaiFrame prepareFrame(const fx::FxFrameContext& context,
                                            std::span<const core::MaterialParameterBlock> materials,
                                            std::span<const AliasEntry> lightSampling,
                                            const EnvironmentGpuResult& environment);
    [[nodiscard]] VulkanFxExecutor::Stats execute(SubayaiFrame& frame, CommandList& commands,
                                                  const FxExecutionResources& resources = {}) const;

  private:
    Device* device_{};
    fx::FxProgram program_;
    std::vector<SubayaiMaterialGpu> materials_;
    SubayaiMaterialGpuRuntime materialRuntime_;
    bool ready_{};
};

} // namespace dayo::graphics
