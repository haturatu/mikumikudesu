#pragma once

#include "fx/fx_compiler.hpp"
#include "fx/fx_frame.hpp"
#include "fx/fx_shader_compiler.hpp"
#include "graphics/fx_executor.hpp"
#include "graphics/fx_pipeline_runtime.hpp"
#include "graphics/fx_resource_runtime.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dayo::graphics {

struct NativeFxFrame {
    fx::FxFrameContext context;
    fx::FxFramePlan plan;
};

// Backend-neutral native FX owner. It gives a compiled program one lifetime
// domain for resources, pipeline layouts, shader pipelines, SBTs, and frame
// plans. Shared renderer descriptor sets can be placed before the FX resource
// set in the same pipeline layout.
class NativeFxRuntime {
  public:
    NativeFxRuntime() = default;
    ~NativeFxRuntime();

    NativeFxRuntime(const NativeFxRuntime&) = delete;
    NativeFxRuntime& operator=(const NativeFxRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, fx::FxProgram program, const fx::FxShaderCompiler& compiler,
                                   std::span<const handles::DescriptorSetLayoutHandle> sharedLayouts = {},
                                   std::string* error = nullptr,
                                   std::span<const handles::DescriptorSetHandle> sharedDescriptorSets = {});
    // Initializes resources against the first real frame context. The
    // compatibility overload above remains useful for callers that do not
    // have a frame yet.
    [[nodiscard]] bool initializeForFrame(
        Device& device, fx::FxProgram program, const fx::FxShaderCompiler& compiler,
        const fx::FxFrameContext& context,
        std::span<const handles::DescriptorSetLayoutHandle> sharedLayouts = {}, std::string* error = nullptr,
        std::span<const handles::DescriptorSetHandle> sharedDescriptorSets = {});
    // Rebuilds size-dependent FX resources and their descriptor/pipeline
    // lifetime when a render/model context changes. Callers should invoke
    // this at a frame boundary before prepareFrame().
    [[nodiscard]] bool refresh(const fx::FxFrameContext& context, std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return ready_;
    }
    [[nodiscard]] const fx::FxProgram* program() const noexcept {
        return ready_ ? &program_ : nullptr;
    }
    [[nodiscard]] const FxResourceRuntime& resources() const noexcept {
        return resources_;
    }
    [[nodiscard]] const FxPipelineRuntime& pipelines() const noexcept {
        return pipelines_;
    }
    // Returns the last texture written by the planned native passes. The
    // presentation backend uses the metadata to validate the output without
    // coupling the FX runtime to a swapchain image.
    [[nodiscard]] std::optional<NativeFrameOutput> output(const NativeFxFrame& frame) const;
    [[nodiscard]] handles::PipelineLayoutHandle pipelineLayout() const noexcept {
        return pipelineLayout_;
    }
    [[nodiscard]] std::uint32_t resourceSetIndex() const noexcept {
        return resourceSetIndex_;
    }
    [[nodiscard]] std::size_t sharedDescriptorSetCount() const noexcept {
        return sharedDescriptorSets_.size();
    }

    [[nodiscard]] NativeFxFrame prepareFrame(const fx::FxFrameContext& context) const;
    [[nodiscard]] VulkanFxExecutor::Stats execute(NativeFxFrame& frame, CommandList& commands,
                                                  const FxExecutionResources& resources = {}) const;

  private:
    Device* device_{};
    fx::FxProgram program_;
    fx::FxShaderCompiler compiler_{};
    std::vector<handles::DescriptorSetLayoutHandle> sharedLayouts_;
    std::vector<handles::DescriptorSetHandle> sharedDescriptorSets_;
    FxResourceRuntime resources_;
    FxPipelineRuntime pipelines_;
    handles::PipelineLayoutHandle pipelineLayout_{};
    std::uint32_t resourceSetIndex_{};
    std::optional<fx::FxFrameContext> resourceContext_;
    bool configured_{};
    bool ready_{};

    [[nodiscard]] bool buildForContext(const fx::FxFrameContext& context, std::string* error);
    void releaseGpuState() noexcept;
};

} // namespace dayo::graphics
