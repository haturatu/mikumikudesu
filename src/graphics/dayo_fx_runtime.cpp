#include "graphics/dayo_fx_runtime.hpp"

#include <algorithm>
#include <utility>

namespace dayo::graphics {

bool DayoSceneHostProvider::supports(std::string_view semantic) const {
    return provider_ != nullptr && dayoSemanticFromString(semantic).has_value();
}

FxResourceBinding DayoSceneHostProvider::resolve(std::string_view semantic, const fx::FxFrameContext&) {
    if (provider_ == nullptr)
        return {};
    const auto binding = provider_->resolve(semantic);
    if (!binding.has_value())
        return {};
    return {.texture = binding->texture,
            .buffer = binding->buffer,
            .accelerationStructure = binding->accelerationStructure};
}

bool DayoFxRuntime::initialize(Device& device, fx::FxProgram program, const fx::FxShaderCompiler& compiler,
                               std::span<const handles::DescriptorSetLayoutHandle> sharedLayouts, std::string* error,
                               std::span<const handles::DescriptorSetHandle> sharedDescriptorSets,
                               fx::FxNativeShaderSourceOptions sourceOptions) {
    return runtime_.initialize(device, std::move(program), compiler, sharedLayouts, error, sharedDescriptorSets,
                               std::move(sourceOptions));
}

bool DayoFxRuntime::initializeForFrame(Device& device, fx::FxProgram program,
                                       const fx::FxShaderCompiler& compiler, const fx::FxFrameContext& context,
                                       std::span<const handles::DescriptorSetLayoutHandle> sharedLayouts,
                                       std::string* error,
                                       std::span<const handles::DescriptorSetHandle> sharedDescriptorSets,
                                       fx::FxNativeShaderSourceOptions sourceOptions) {
    return runtime_.initializeForFrame(device, std::move(program), compiler, context, sharedLayouts, error,
                                       sharedDescriptorSets, std::move(sourceOptions));
}

bool DayoFxRuntime::refresh(const fx::FxFrameContext& context, std::string* error) {
    return runtime_.refresh(context, error);
}

void DayoFxRuntime::reset() noexcept {
    runtime_.reset();
}

void DayoFxRuntime::addProvider(FxExternalResourceProvider& provider) {
    if (std::find(providers_.begin(), providers_.end(), &provider) == providers_.end())
        providers_.push_back(&provider);
}

void DayoFxRuntime::clearProviders() noexcept {
    providers_.clear();
}

NativeFxFrame DayoFxRuntime::prepareFrame(const fx::FxFrameContext& context,
                                          std::span<const handles::DescriptorSetHandle> sharedSets) const {
    return runtime_.prepareFrame(context, sharedSets);
}

VulkanFxExecutor::Stats DayoFxRuntime::execute(NativeFxFrame& frame, CommandList& commands,
                                               const FxExecutionResources& resources) const {
    auto nativeResources = resources;
    const auto existingResolver = resources.resolveTypedResource;
    const auto providers = providers_;
    nativeResources.resolveTypedResource = [this, existingResolver, providers, &frame](std::string_view name)
        -> std::optional<FxExecutionResources::TypedResource> {
        if (existingResolver) {
            const auto binding = existingResolver(name);
            if (binding.has_value())
                return binding;
        }
        if (const auto texture = runtime_.resources().resolveTexture(name); texture.has_value())
            return FxExecutionResources::TypedResource{.texture = *texture};
        if (const auto buffer = runtime_.resources().resolveBuffer(name); buffer.has_value())
            return FxExecutionResources::TypedResource{.buffer = *buffer};
        if (const auto sampler = runtime_.resources().resolveSampler(name); sampler.has_value())
            return FxExecutionResources::TypedResource{.sampler = *sampler};
        for (auto* provider : providers) {
            if (provider == nullptr || !provider->supports(name))
                continue;
            const auto binding = provider->resolve(name, frame.context);
            if (binding.valid())
                return FxExecutionResources::TypedResource{.texture = binding.texture,
                                                           .buffer = binding.buffer,
                                                           .sampler = binding.sampler,
                                                           .accelerationStructure = binding.accelerationStructure};
        }
        return std::nullopt;
    };
    const auto existingBefore = resources.beforePass;
    nativeResources.beforePass = [existingBefore, providers](const fx::FxDispatch& dispatch, CommandList& list) {
        if (existingBefore)
            existingBefore(dispatch, list);
        for (auto* provider : providers)
            if (provider != nullptr)
                provider->beforePass(dispatch, list);
    };
    const auto existingAfter = resources.afterPass;
    nativeResources.afterPass = [existingAfter, providers](const fx::FxDispatch& dispatch, CommandList& list) {
        for (auto* provider : providers)
            if (provider != nullptr)
                provider->afterPass(dispatch, list);
        if (existingAfter)
            existingAfter(dispatch, list);
    };
    return runtime_.execute(frame, commands, nativeResources);
}

std::optional<NativeFrameOutput> DayoFxRuntime::output(const NativeFxFrame& frame) const {
    return runtime_.output(frame);
}

} // namespace dayo::graphics
