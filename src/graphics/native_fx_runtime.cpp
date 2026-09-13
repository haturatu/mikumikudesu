#include "graphics/native_fx_runtime.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string message) {
    if (error != nullptr)
        *error = std::move(message);
}

} // namespace

NativeFxRuntime::~NativeFxRuntime() {
    reset();
}

bool NativeFxRuntime::initialize(Device& device, fx::FxProgram program, const fx::FxShaderCompiler& compiler,
                                 std::span<const handles::DescriptorSetLayoutHandle> sharedLayouts,
                                 std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    device_ = &device;
    try {
        if (!resources_.initialize(device, program, fx::makeFxFrameContext(0.0F, 0, 1, 1, 0, 0, 1, 1, 1,
                                                                            program.meshCloneCount),
                                   error))
            throw std::runtime_error(error != nullptr && !error->empty() ? *error
                                                                            : "FX resource initialization failed");

        std::vector<handles::DescriptorSetLayoutHandle> setLayouts;
        setLayouts.reserve(sharedLayouts.size() + 1U);
        for (const auto layout : sharedLayouts) {
            if (!layout.valid())
                throw std::invalid_argument("native FX pipeline layout contains an invalid shared descriptor layout");
            setLayouts.push_back(layout);
        }
        resourceSetIndex_ = static_cast<std::uint32_t>(setLayouts.size());
        if (resources_.descriptorLayout().valid())
            setLayouts.push_back(resources_.descriptorLayout());

        pipelineLayout_ = device.createPipelineLayoutEx({.setLayouts = std::move(setLayouts)});
        if (!pipelineLayout_.valid())
            throw std::runtime_error("native FX pipeline layout allocation returned an invalid handle");
        const auto layout = pipelineLayout_;
        if (!pipelines_.build(device, program, compiler,
                              [layout](const fx::FxDispatch&) -> std::optional<handles::PipelineLayoutHandle> {
                                  return layout;
                              },
                              error))
            throw std::runtime_error(error != nullptr && !error->empty() ? *error
                                                                            : "FX pipeline initialization failed");
        program_ = std::move(program);
    } catch (const std::exception& exception) {
        if (error == nullptr || error->empty())
            setError(error, exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "native FX runtime initialization failed");
        reset();
        return false;
    }
    ready_ = true;
    return true;
}

void NativeFxRuntime::reset() noexcept {
    pipelines_.reset();
    if (device_ != nullptr && pipelineLayout_.valid()) {
        try {
            device_->destroyPipelineLayoutEx(pipelineLayout_);
        } catch (...) {
        }
    }
    pipelineLayout_ = {};
    resources_.reset();
    program_ = {};
    device_ = nullptr;
    resourceSetIndex_ = 0;
    ready_ = false;
}

NativeFxFrame NativeFxRuntime::prepareFrame(const fx::FxFrameContext& context) const {
    if (!ready_)
        throw std::logic_error("native FX runtime is not initialized");
    NativeFxFrame frame;
    frame.context = context;
    frame.plan = fx::FxCompiler{}.plan(program_, context);
    return frame;
}

VulkanFxExecutor::Stats NativeFxRuntime::execute(NativeFxFrame& frame, CommandList& commands,
                                                 const FxExecutionResources& resources) const {
    if (!ready_ || device_ == nullptr)
        throw std::logic_error("native FX runtime is not initialized");

    auto nativeResources = resources;
    if (!nativeResources.resolveTypedResource) {
        nativeResources.resolveTypedResource = [this](std::string_view name)
            -> std::optional<FxExecutionResources::TypedResource> {
            if (const auto texture = resources_.resolveTexture(name); texture.has_value())
                return FxExecutionResources::TypedResource{.texture = *texture};
            if (const auto buffer = resources_.resolveBuffer(name); buffer.has_value())
                return FxExecutionResources::TypedResource{.buffer = *buffer};
            if (const auto sampler = resources_.resolveSampler(name); sampler.has_value())
                return FxExecutionResources::TypedResource{.sampler = *sampler};
            return std::nullopt;
        };
    }
    if (!nativeResources.resolveTypedPipeline) {
        nativeResources.resolveTypedPipeline = [this](const fx::FxDispatch& dispatch) {
            return pipelines_.resolvePipeline(dispatch);
        };
    }
    if (!nativeResources.resolveShaderBindingTable) {
        nativeResources.resolveShaderBindingTable = [this](const fx::FxDispatch& dispatch) {
            return pipelines_.resolveShaderBindingTable(dispatch);
        };
    }

    if (resources_.descriptorSet().valid() && !nativeResources.resolveDescriptorSets) {
        const auto set = resources_.descriptorSet();
        const auto setIndex = resourceSetIndex_;
        const auto existingSingle = nativeResources.resolveDescriptorSet;
        nativeResources.resolveDescriptorSets = [existingSingle, set, setIndex](const fx::FxDispatch& dispatch) {
            std::vector<FxExecutionResources::TypedDescriptorSetBinding> result;
            if (existingSingle) {
                const auto shared = existingSingle(dispatch);
                if (shared.has_value())
                    result.push_back({*shared, 0});
            }
            result.push_back({set, setIndex});
            return result;
        };
        nativeResources.resolveDescriptorSet = {};
    }
    return VulkanFxExecutor{*device_}.execute(frame.plan, commands, frame.context, nativeResources);
}

} // namespace dayo::graphics
