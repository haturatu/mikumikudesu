#pragma once

#include "fx/fx_condition_runtime.hpp"
#include "graphics/dayo_host_resources.hpp"
#include "graphics/native_fx_runtime.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::graphics {

struct FxResourceBinding {
    handles::TextureHandle texture{};
    handles::BufferHandle buffer{};
    handles::SamplerHandle sampler{};
    handles::AccelerationStructureHandle accelerationStructure{};

    [[nodiscard]] bool valid() const noexcept {
        return texture.valid() || buffer.valid() || sampler.valid() || accelerationStructure.valid();
    }
};

class FxExternalResourceProvider {
  public:
    virtual ~FxExternalResourceProvider() = default;
    [[nodiscard]] virtual bool supports(std::string_view semantic) const = 0;
    [[nodiscard]] virtual FxResourceBinding resolve(std::string_view semantic, const fx::FxFrameContext& context) = 0;
    virtual void beforePass(const fx::FxDispatch&, CommandList&) {}
    virtual void afterPass(const fx::FxDispatch&, CommandList&) {}
};

// Adapter for the canonical scene host provider. Renderer-specific providers
// can implement the same interface for TLAS/light/accumulation resources.
class DayoSceneHostProvider final : public FxExternalResourceProvider {
  public:
    DayoSceneHostProvider() noexcept = default;
    explicit DayoSceneHostProvider(const DayoHostResourceProvider& provider) noexcept : provider_(&provider) {}

    void setProvider(const DayoHostResourceProvider& provider) noexcept {
        provider_ = &provider;
    }

    [[nodiscard]] bool supports(std::string_view semantic) const override;
    [[nodiscard]] FxResourceBinding resolve(std::string_view semantic, const fx::FxFrameContext& context) override;

  private:
    const DayoHostResourceProvider* provider_{};
};

// Generic YRZFX owner. It delegates shader/resource lifetime to the existing
// NativeFxRuntime and only adds provider resolution and pass lifecycle hooks.
class DayoFxRuntime {
  public:
    DayoFxRuntime() = default;
    ~DayoFxRuntime() = default;

    DayoFxRuntime(const DayoFxRuntime&) = delete;
    DayoFxRuntime& operator=(const DayoFxRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, fx::FxProgram program, const fx::FxShaderCompiler& compiler,
                                  std::span<const handles::DescriptorSetLayoutHandle> sharedLayouts = {},
                                  std::string* error = nullptr,
                                  std::span<const handles::DescriptorSetHandle> sharedDescriptorSets = {},
                                  fx::FxNativeShaderSourceOptions sourceOptions = {});
    [[nodiscard]] bool initializeForFrame(Device& device, fx::FxProgram program, const fx::FxShaderCompiler& compiler,
                                          const fx::FxFrameContext& context,
                                          std::span<const handles::DescriptorSetLayoutHandle> sharedLayouts = {},
                                          std::string* error = nullptr,
                                          std::span<const handles::DescriptorSetHandle> sharedDescriptorSets = {},
                                          fx::FxNativeShaderSourceOptions sourceOptions = {});
    [[nodiscard]] bool refresh(const fx::FxFrameContext& context, std::string* error = nullptr);
    void reset() noexcept;

    void addProvider(FxExternalResourceProvider& provider);
    void clearProviders() noexcept;
    [[nodiscard]] std::size_t providerCount() const noexcept {
        return providers_.size();
    }

    [[nodiscard]] bool ready() const noexcept {
        return runtime_.ready();
    }
    [[nodiscard]] const fx::FxProgram* program() const noexcept {
        return runtime_.program();
    }
    [[nodiscard]] NativeFxFrame prepareFrame(const fx::FxFrameContext& context,
                                             std::span<const handles::DescriptorSetHandle> sharedSets = {}) const;
    [[nodiscard]] VulkanFxExecutor::Stats execute(NativeFxFrame& frame, CommandList& commands,
                                                  const FxExecutionResources& resources = {}) const;
    [[nodiscard]] std::optional<NativeFrameOutput> output(const NativeFxFrame& frame) const;

    [[nodiscard]] const NativeFxRuntime& nativeRuntime() const noexcept {
        return runtime_;
    }

  private:
    NativeFxRuntime runtime_;
    std::vector<FxExternalResourceProvider*> providers_;
    mutable fx::FxConditionRuntime conditionRuntime_;
    mutable bool hasExecuted_{};
    mutable std::uint32_t lastRenderWidth_{};
    mutable std::uint32_t lastRenderHeight_{};
};

} // namespace dayo::graphics
