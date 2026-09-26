#include "graphics/native_fx_runtime.hpp"

#include "graphics/fx_material_gpu_runtime.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string message) {
    if (error != nullptr)
        *error = std::move(message);
}

[[nodiscard]] bool sameResourceContext(const fx::FxFrameContext& left, const fx::FxFrameContext& right) noexcept {
    // Frame/sample/camera/light values are per-dispatch state. They are
    // already supplied to prepareFrame() and must not rebuild persistent FX
    // textures, descriptor sets, shader modules, or pipelines every frame.
    // Only values that can change a declared resource extent belong here.
    return left.renderWidth == right.renderWidth && left.renderHeight == right.renderHeight &&
           left.modelIndex == right.modelIndex && left.vertexCount == right.vertexCount &&
           left.totalMaterial == right.totalMaterial && left.cloneCount == right.cloneCount &&
           left.clonedVertexCount == right.clonedVertexCount;
}

} // namespace

NativeFxRuntime::~NativeFxRuntime() {
    reset();
}

bool NativeFxRuntime::initialize(Device& device, fx::FxProgram program, const fx::FxShaderCompiler& compiler,
                                 std::span<const handles::DescriptorSetLayoutHandle> sharedLayouts, std::string* error,
                                 std::span<const handles::DescriptorSetHandle> sharedDescriptorSets,
                                 fx::FxNativeShaderSourceOptions sourceOptions,
                                 const FxMaterialGpuRuntime* materialRuntime,
                                 const FxMaterialRuntimeInitializer& initializeMaterialRuntime) {
    const auto defaultContext = fx::makeFxFrameContext(0.0F, 0, 1, 1, 0, 0, 1, 1, 1, program.meshCloneCount);
    return initializeForFrame(device, std::move(program), compiler, defaultContext, sharedLayouts, error,
                              sharedDescriptorSets, std::move(sourceOptions), materialRuntime,
                              initializeMaterialRuntime);
}

bool NativeFxRuntime::initializeForFrame(
    Device& device, fx::FxProgram program, const fx::FxShaderCompiler& compiler, const fx::FxFrameContext& context,
    std::span<const handles::DescriptorSetLayoutHandle> sharedLayouts, std::string* error,
    std::span<const handles::DescriptorSetHandle> sharedDescriptorSets, fx::FxNativeShaderSourceOptions sourceOptions,
    const FxMaterialGpuRuntime* materialRuntime, const FxMaterialRuntimeInitializer& initializeMaterialRuntime) {
    if (error != nullptr)
        error->clear();
    reset();
    device_ = &device;
    program_ = std::move(program);
    compiler_ = compiler;
    sharedLayouts_.assign(sharedLayouts.begin(), sharedLayouts.end());
    sharedDescriptorSets_.assign(sharedDescriptorSets.begin(), sharedDescriptorSets.end());
    sourceOptions_ = std::move(sourceOptions);
    materialRuntime_ = materialRuntime;
    configured_ = true;
    try {
        if (!sharedDescriptorSets_.empty() && sharedDescriptorSets_.size() != sharedLayouts_.size())
            throw std::invalid_argument("native FX shared descriptor sets must match shared layouts");
        if (std::any_of(sharedDescriptorSets_.begin(), sharedDescriptorSets_.end(),
                        [](const auto set) { return !set.valid(); }))
            throw std::invalid_argument("native FX shared descriptor sets contain an invalid handle");
        if (!buildForContext(context, error, initializeMaterialRuntime))
            throw std::runtime_error(error != nullptr && !error->empty() ? *error : "FX runtime initialization failed");
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
    resourceContext_ = context;
    ready_ = true;
    return true;
}

bool NativeFxRuntime::refresh(const fx::FxFrameContext& context, std::string* error,
                              const FxMaterialRuntimeInitializer& initializeMaterialRuntime) {
    if (error != nullptr)
        error->clear();
    if (device_ == nullptr || !configured_) {
        setError(error, "native FX runtime is not initialized");
        return false;
    }
    if (ready_ && resourceContext_.has_value() && sameResourceContext(*resourceContext_, context))
        return true;

    ready_ = false;
    releaseGpuState();
    if (!buildForContext(context, error, initializeMaterialRuntime))
        return false;
    resourceContext_ = context;
    ready_ = true;
    return true;
}

bool NativeFxRuntime::buildForContext(const fx::FxFrameContext& context, std::string* error,
                                      const FxMaterialRuntimeInitializer& initializeMaterialRuntime) {
    try {
        resourceSetIndex_ = static_cast<std::uint32_t>(sharedLayouts_.size());
        const auto materialInitializer =
            initializeMaterialRuntime
                ? FxMaterialRuntimeInitializer{[this, &initializeMaterialRuntime](const FxResourceStore& store,
                                                                                  std::string* materialError) {
                      materialRuntime_ = initializeMaterialRuntime(store, materialError);
                      return materialRuntime_;
                  }}
                : FxMaterialRuntimeInitializer{};
        if (!resources_.initialize(*device_, program_, context, error, resourceSetIndex_, materialRuntime_,
                                   materialInitializer))
            throw std::runtime_error(error != nullptr && !error->empty() ? *error
                                                                         : "FX resource initialization failed");

        const auto framePlan = fx::FxCompiler{}.plan(program_, context, &resources_);

        for (const auto layout : sharedLayouts_) {
            if (!layout.valid())
                throw std::invalid_argument("native FX pipeline layout contains an invalid shared descriptor layout");
        }
        for (const auto& dispatch : program_.passes) {
            std::vector<handles::DescriptorSetLayoutHandle> setLayouts(sharedLayouts_.begin(), sharedLayouts_.end());
            for (const auto& [setIndex, layout] : resources_.descriptorLayoutsFor(dispatch)) {
                if (setIndex < sharedLayouts_.size())
                    throw std::invalid_argument("native FX resource descriptor overlaps a shared descriptor set");
                if (setLayouts.size() <= setIndex)
                    setLayouts.resize(static_cast<std::size_t>(setIndex) + 1U);
                if (setLayouts[setIndex].valid())
                    throw std::invalid_argument("native FX pass has multiple layouts for one descriptor set");
                setLayouts[setIndex] = layout;
            }
            if (std::any_of(setLayouts.begin(), setLayouts.end(), [](const auto layout) { return !layout.valid(); }))
                throw std::invalid_argument("native FX pipeline descriptor set layout indices contain a gap");
            const auto layout = device_->createPipelineLayoutEx({.setLayouts = std::move(setLayouts)});
            if (!layout.valid())
                throw std::runtime_error("native FX pass pipeline layout allocation returned an invalid handle: " +
                                         dispatch.name);
            passPipelineLayouts_.emplace(dispatch.name, layout);
            if (!pipelineLayout_.valid())
                pipelineLayout_ = layout;
        }
        if (!pipelines_.build(
                *device_, program_, framePlan, compiler_,
                [this](const fx::FxDispatch& dispatch) -> std::optional<handles::PipelineLayoutHandle> {
                    const auto found = passPipelineLayouts_.find(dispatch.name);
                    if (found == passPipelineLayouts_.end())
                        return std::nullopt;
                    return found->second;
                },
                error, resourceSetIndex_, sourceOptions_))
            throw std::runtime_error(error != nullptr && !error->empty() ? *error
                                                                         : "FX pipeline initialization failed");
    } catch (const std::exception& exception) {
        if (error == nullptr || error->empty())
            setError(error, exception.what());
        releaseGpuState();
        return false;
    } catch (...) {
        setError(error, "FX runtime build failed");
        releaseGpuState();
        return false;
    }
    return true;
}

void NativeFxRuntime::releaseGpuState() noexcept {
    pipelines_.reset();
    if (device_ != nullptr) {
        for (const auto& [name, layout] : passPipelineLayouts_) {
            static_cast<void>(name);
            if (!layout.valid())
                continue;
            try {
                device_->destroyPipelineLayoutEx(layout);
            } catch (...) {
            }
        }
    }
    passPipelineLayouts_.clear();
    pipelineLayout_ = {};
    resources_.reset();
    resourceSetIndex_ = 0;
}

void NativeFxRuntime::reset() noexcept {
    releaseGpuState();
    program_ = {};
    compiler_ = fx::FxShaderCompiler{};
    sharedLayouts_.clear();
    sharedDescriptorSets_.clear();
    sourceOptions_ = {};
    materialRuntime_ = nullptr;
    device_ = nullptr;
    resourceContext_.reset();
    configured_ = false;
    ready_ = false;
}

std::optional<NativeFrameOutput> NativeFxRuntime::output(const NativeFxFrame& frame) const {
    const auto resolved = resources_.resolveOutputTexture(frame.plan.ordered);
    if (!resolved.has_value() || !resolved->valid())
        return std::nullopt;
    return NativeFrameOutput{.texture = resolved->handle, .extent = resolved->extent, .format = resolved->format};
}

NativeFxFrame NativeFxRuntime::prepareFrame(const fx::FxFrameContext& context,
                                            std::span<const handles::DescriptorSetHandle> sharedSets) const {
    if (!ready_)
        throw std::logic_error("native FX runtime is not initialized");
    NativeFxFrame frame;
    frame.context = context;
    frame.plan = fx::FxCompiler{}.plan(program_, context, &resources_);
    if (sharedSets.empty())
        frame.sharedDescriptorSets = sharedDescriptorSets_;
    else
        frame.sharedDescriptorSets.assign(sharedSets.begin(), sharedSets.end());
    if (frame.sharedDescriptorSets.size() != sharedLayouts_.size())
        throw std::invalid_argument("native FX frame shared descriptor sets must match shared layouts");
    if (std::any_of(frame.sharedDescriptorSets.begin(), frame.sharedDescriptorSets.end(),
                    [](const auto set) { return !set.valid(); }))
        throw std::invalid_argument("native FX frame shared descriptor sets contain an invalid handle");
    return frame;
}

VulkanFxExecutor::Stats NativeFxRuntime::execute(NativeFxFrame& frame, CommandList& commands,
                                                 const FxExecutionResources& resources) const {
    if (!ready_ || device_ == nullptr)
        throw std::logic_error("native FX runtime is not initialized");

    auto nativeResources = resources;
    if (!nativeResources.resolveTypedResource) {
        nativeResources.resolveTypedResource =
            [this](std::string_view name) -> std::optional<FxExecutionResources::TypedResource> {
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

    if (!program_.passes.empty() || !frame.sharedDescriptorSets.empty()) {
        const auto existingSets = nativeResources.resolveDescriptorSets;
        const auto existingSingle = nativeResources.resolveDescriptorSet;
        const auto sharedSets = frame.sharedDescriptorSets;
        nativeResources.resolveDescriptorSets = [this, existingSets, existingSingle,
                                                 sharedSets](const fx::FxDispatch& dispatch) {
            std::vector<FxExecutionResources::TypedDescriptorSetBinding> result;
            if (existingSets) {
                result = existingSets(dispatch);
            } else if (existingSingle) {
                const auto shared = existingSingle(dispatch);
                if (shared.has_value())
                    result.push_back({*shared, 0});
            }
            const auto hasIndex = [&result](std::uint32_t index) {
                return std::any_of(result.begin(), result.end(),
                                   [index](const auto& binding) { return binding.setIndex == index; });
            };
            for (std::size_t index = 0; index < sharedSets.size(); ++index) {
                if (!hasIndex(static_cast<std::uint32_t>(index)))
                    result.push_back({sharedSets[index], static_cast<std::uint32_t>(index)});
            }
            const auto ownedSets = resources_.descriptorSetsFor(dispatch);
            for (const auto& owned : ownedSets) {
                std::erase_if(result, [&owned](const auto& binding) { return binding.setIndex == owned.setIndex; });
                result.push_back({owned.set, owned.setIndex});
            }
            return result;
        };
        nativeResources.resolveDescriptorSet = {};
    }
    return VulkanFxExecutor{*device_}.execute(frame.plan, commands, frame.context, nativeResources);
}

} // namespace dayo::graphics
