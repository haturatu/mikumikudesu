#pragma once

#include "fx/fx_compiler.hpp"
#include "fx/fx_frame.hpp"
#include "graphics/device.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::graphics {

struct FxExecutionResources {
    struct TypedDescriptorSetBinding {
        handles::DescriptorSetHandle set{};
        std::uint32_t setIndex{};
    };
    using TextureResolver = std::function<std::optional<TextureHandle>(std::string_view)>;
    using PipelineResolver = std::function<std::optional<PipelineHandle>(const dayo::fx::FxDispatch&)>;
    using TypedPipelineResolver = std::function<std::optional<handles::PipelineHandle>(const dayo::fx::FxDispatch&)>;
    using TypedTextureResolver = std::function<std::optional<handles::TextureHandle>(std::string_view)>;
    using ShaderBindingTableResolver =
        std::function<std::optional<handles::ShaderBindingTableHandle>(const dayo::fx::FxDispatch&)>;
    using DescriptorSetResolver =
        std::function<std::optional<handles::DescriptorSetHandle>(const dayo::fx::FxDispatch&)>;
    using DescriptorSetsResolver =
        std::function<std::vector<TypedDescriptorSetBinding>(const dayo::fx::FxDispatch&)>;
    using ResourceBindingResolver =
        std::function<std::optional<DescriptorBinding>(std::string_view, bool, std::uint32_t)>;
    using PushConstantResolver =
        std::function<std::vector<std::byte>(const dayo::fx::FxDispatch&, const dayo::fx::FxFrameContext&)>;
    using ConditionEvaluator = std::function<bool(std::span<const std::string>, const dayo::fx::FxFrameContext&)>;
    TextureResolver resolveTexture;
    // Generic resource providers keep shader compilation and descriptor
    // allocation backend-specific while making the command contract explicit.
    PipelineResolver resolvePipeline;
    // Native FX/RT paths use generation-checked pipeline/SBT handles. The
    // legacy resolver remains available for Preview and existing callers.
    TypedPipelineResolver resolveTypedPipeline;
    TypedTextureResolver resolveTypedTexture;
    ShaderBindingTableResolver resolveShaderBindingTable;
    DescriptorSetResolver resolveDescriptorSet;
    DescriptorSetsResolver resolveDescriptorSets;
    ResourceBindingResolver resolveBinding;
    PushConstantResolver makePushConstants;
    ConditionEvaluator evaluateConditions;
};

// Generic Vulkan FX executor. Shader passes require a resolved pipeline and
// record resource transitions, descriptor bindings, optional push constants,
// and the draw/dispatch command. Utility passes record their typed operation.
// The executor does not own device resources or shader-cache entries.
// Raytracing fails explicitly so Preview callers notice the missing path
// instead of silently dropping a pass.
class VulkanFxExecutor {
  public:
    struct Stats {
        std::size_t raster{};
        std::size_t postprocess{};
        std::size_t compute{};
        std::size_t copy{};
        std::size_t clear{};
        std::size_t mipmap{};
        std::size_t rayTracing{};
    };

    explicit VulkanFxExecutor(Device& device) noexcept : device_(&device) {}

    VulkanFxExecutor(const VulkanFxExecutor&) = delete;
    VulkanFxExecutor& operator=(const VulkanFxExecutor&) = delete;

    [[nodiscard]] Device& device() const noexcept {
        return *device_;
    }

    Stats execute(const dayo::fx::FxFramePlan& plan, CommandList& commands, const dayo::fx::FxFrameContext& context,
                  const FxExecutionResources& resources = {}) const;

  private:
    Device* device_;
};

class FxRaytracingUnsupported : public std::logic_error {
  public:
    explicit FxRaytracingUnsupported(const std::string& pass)
        : std::logic_error("VulkanFxExecutor: raytracing pass '" + pass + "' is not supported in the generic path") {}
};

} // namespace dayo::graphics
