#include "graphics/native_renderer.hpp"

#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace dayo::graphics {
namespace {

void appendReason(std::ostringstream& output, std::string_view reason) {
    if (reason.empty())
        return;
    if (output.tellp() > 0)
        output << ", ";
    output << reason;
}

} // namespace

std::string missingEffectFeatures(const DeviceCapabilities& capabilities, const fx::FxRequiredFeatures& required) {
    std::ostringstream output;
    if (required.descriptorIndexing && !capabilities.descriptorIndexing)
        appendReason(output, "descriptorIndexing");
    if (required.accelerationStructure && !capabilities.accelerationStructure)
        appendReason(output, "VK_KHR_acceleration_structure");
    if (required.rayQuery && !capabilities.rayQuery)
        appendReason(output, "VK_KHR_ray_query");
    if (required.rayTracingPipeline && !capabilities.rayTracingPipeline)
        appendReason(output, "VK_KHR_ray_tracing_pipeline");
    if (required.fragmentShaderBarycentric && !capabilities.fragmentShaderBarycentric)
        appendReason(output, "VK_KHR_fragment_shader_barycentric");
    return output.str();
}

NativeRendererStatus decideNativeRenderer(const DeviceCapabilities& capabilities, RendererKind requested,
                                          const fx::FxRequiredFeatures& required) {
    NativeRendererStatus result{.requested = requested, .active = requested};
    if (requested == RendererKind::preview)
        return result;

    std::ostringstream reason;
    appendReason(reason, capabilities.missingFeatures(requested));
    appendReason(reason, missingEffectFeatures(capabilities, required));
    if (reason.tellp() > 0) {
        result.active = RendererKind::preview;
        result.reason = reason.str();
    }
    return result;
}

NativeRendererStatus decideNativeRendererForInitialization(const DeviceCapabilities& capabilities,
                                                           RendererKind requested,
                                                           const fx::FxRequiredFeatures& required) {
    NativeRendererStatus result{.requested = requested, .active = requested};
    if (requested == RendererKind::preview)
        return result;

    std::ostringstream reason;
    appendReason(reason, capabilities.missingHardwareFeatures(requested));
    appendReason(reason, missingEffectFeatures(capabilities, required));
    if (reason.tellp() > 0) {
        result.active = RendererKind::preview;
        result.reason = reason.str();
    }
    return result;
}

NativeRendererCoordinator::GenericEffectRuntime::~GenericEffectRuntime() {
    runtime.reset();
    if (device != nullptr) {
        for (const auto set : frameSets) {
            if (!set.valid())
                continue;
            try {
                device->destroyDescriptorSetEx(set);
            } catch (...) {
            }
        }
    }
}

NativeRendererStatus NativeRendererCoordinator::prepare(Device& device, RendererKind requested,
                                                        const core::EffectGraph& graph) {
    return prepare(device, requested, fx::FxCompiler{}.compile(graph));
}

NativeRendererStatus NativeRendererCoordinator::prepare(Device& device, RendererKind requested, fx::FxProgram program) {
    reset();
    device_ = &device;
    status_ = decideNativeRendererForInitialization(device.capabilities(), requested, fx::requiredFeatures(program));
    if (status_.fellBack() || requested == RendererKind::preview)
        return status_;

    std::string error;
    bool initialized = false;
    switch (requested) {
    case RendererKind::subayai:
        initialized = subayai_.initialize(device, std::move(program), &error);
        if (initialized) {
            subayai_.setEnvironmentBackend(environmentBackend_);
            subayai_.setSceneFrameRuntime(sceneFrameRuntime_);
        }
        break;
    case RendererKind::bdpt:
        initialized = bdpt_.initialize(device, std::move(program), &error);
        if (initialized)
            bdpt_.setSceneFrameRuntime(sceneFrameRuntime_);
        break;
    case RendererKind::preview:
        break;
    }
    if (!initialized) {
        status_.active = RendererKind::preview;
        status_.reason = error.empty() ? "native renderer initialization failed" : std::move(error);
        return status_;
    }
    status_.nativeReady = true;
    return status_;
}

void NativeRendererCoordinator::setEnvironmentBackend(IEnvironmentBackend* backend) noexcept {
    if (environmentBackend_ == backend)
        return;
    environmentService_.clear();
    environmentBackend_ = backend;
    environmentService_ = EnvironmentService(backend);
    subayai_.setEnvironmentBackend(backend);
}

void NativeRendererCoordinator::setSceneFrameRuntime(NativeSceneFrameRuntime* runtime) noexcept {
    sceneFrameRuntime_ = runtime;
    subayai_.setSceneFrameRuntime(runtime);
    bdpt_.setSceneFrameRuntime(runtime);
}

void NativeRendererCoordinator::setHostResourceBindings(const NativeSceneResourceBindings& bindings) noexcept {
    hostBindings_ = bindings;
    hostResourceProvider_.emplace(bindings);
    sceneHostProvider_.setProvider(*hostResourceProvider_);
    subayai_.setExternalResourceProvider(&sceneHostProvider_);
    bdpt_.setExternalResourceProvider(&sceneHostProvider_);
}

void NativeRendererCoordinator::setEffectStack(const core::SceneEffectStack& effects) {
    deformEffects_ = effects.deform;
    postprocessEffects_ = effects.postprocess;
    deformRuntimes_.clear();
    postprocessRuntimes_.clear();
    deformerResources_.clear();
}

void NativeRendererCoordinator::setEffectSchedule(std::span<const fx::ScheduledFx> schedule) {
    const auto nameFor = [](const core::SceneEffectInstance& effect) {
        const auto stem = effect.source.stem().string();
        return stem.empty() ? "effect-" + std::to_string(effect.id) : stem;
    };
    const auto reorder = [&](auto& effects, auto& runtimes, const auto& acceptsStage) {
        std::vector<core::SceneEffectInstance> ordered;
        std::vector<std::size_t> order;
        for (const auto& item : schedule) {
            if (!acceptsStage(item.stage))
                continue;
            const auto found = std::ranges::find_if(effects, [&](const auto& effect) {
                return item.effectId != 0 ? effect.id == item.effectId : nameFor(effect) == item.name;
            });
            if (found == effects.end())
                continue;
            const auto index = static_cast<std::size_t>(std::distance(effects.begin(), found));
            if (std::ranges::find(order, index) != order.end())
                continue;
            ordered.push_back(*found);
            order.push_back(index);
        }
        const auto changed = ordered.size() != effects.size() ||
                             !std::ranges::equal(ordered, effects, [](const auto& left, const auto& right) {
                                 return left.id == right.id;
                             });
        if (!changed)
            return false;
        effects = std::move(ordered);
        runtimes.clear();
        return true;
    };
    const auto deformChanged =
        reorder(deformEffects_, deformRuntimes_, [](fx::FrameStage stage) { return stage == fx::FrameStage::deform; });
    static_cast<void>(reorder(postprocessEffects_, postprocessRuntimes_, [](fx::FrameStage stage) {
        return stage == fx::FrameStage::postPre || stage == fx::FrameStage::postPost;
    }));
    if (deformChanged)
        deformerResources_.clear();
}

void NativeRendererCoordinator::setControllerDeclarations(std::span<const core::EffectController> declarations) {
    const auto same =
        std::ranges::equal(controllerDeclarations_, declarations, [](const auto& left, const auto& right) {
            return left.name == right.name && left.controllerName == right.controllerName && left.item == right.item &&
                   left.type == right.type;
        });
    if (same)
        return;
    controllerDeclarations_.assign(declarations.begin(), declarations.end());
    subayai_.setControllerDeclarations(controllerDeclarations_);
    bdpt_.setControllerDeclarations(controllerDeclarations_);
    deformRuntimes_.clear();
    postprocessRuntimes_.clear();
    deformerResources_.clear();
}

std::optional<NativeFrameOutput> NativeRendererCoordinator::executeGenericEffects(
    std::span<const core::SceneEffectInstance> effects, GenericRuntimeList& runtimes, CommandList& commands,
    const fx::FxFrameContext& context, const FxExecutionResources& resources, bool publishToScreen) {
    if (effects.empty())
        return std::nullopt;
    if (device_ == nullptr)
        throw std::logic_error("generic Dayo FX execution requires a device");
    if (sceneFrameRuntime_ == nullptr || !sceneFrameRuntime_->descriptorSetsReady())
        throw std::logic_error("generic Dayo FX execution requires synchronized scene descriptor sets");
    if (runtimes.size() != effects.size())
        runtimes.resize(effects.size());

    const auto layouts = sceneFrameRuntime_->layouts();
    const auto descriptorSets = sceneFrameRuntime_->descriptorSets();
    std::optional<NativeFrameOutput> lastOutput;
    for (std::size_t index = 0; index < effects.size(); ++index) {
        const NativeEffectModel* ownerModel = nullptr;
        auto effectContext = context;
        if (!publishToScreen) {
            if (!effects[index].controllerModel.has_value())
                throw std::runtime_error("deform FX has no controller model owner");
            const auto model = std::ranges::find_if(resources.effectModels, [&](const NativeEffectModel& candidate) {
                return candidate.modelId == *effects[index].controllerModel;
            });
            if (model == resources.effectModels.end() || !resources.updateEffectPassConstants)
                throw std::runtime_error("deform FX owner has no native model pass constants");
            ownerModel = &*model;
            effectContext = makeDeformFxFrameContext(context, *ownerModel, effects[index].graph.meshCloneCount);
        }
        auto& entry = runtimes[index];
        if (!entry) {
            auto program = fx::FxCompiler{}.compile(effects[index].graph);
            std::string error;
            entry = std::make_unique<GenericEffectRuntime>();
            entry->device = device_;
            if (!entry->globalVariables.initialize(*device_, program.globalVarSize, &error))
                throw std::runtime_error(error.empty() ? "generic Dayo FX global buffer initialization failed" : error);
            if (!entry->controller.initialize(*device_, effects[index].graph.controllers))
                throw std::runtime_error("generic Dayo FX controller buffer initialization failed");
            entry->block.emplace(entry->controller.layout());
            entry->runtime.addProvider(sceneHostProvider_);
            fx::FxNativeShaderSourceOptions sourceOptions;
            sourceOptions.controllerDeclarations = effects[index].graph.controllers;
            if (!entry->runtime.initializeForFrame(*device_, std::move(program), fx::FxShaderCompiler{}, effectContext,
                                                   layouts, &error, descriptorSets, std::move(sourceOptions)))
                throw std::runtime_error(error.empty() ? "generic Dayo FX initialization failed" : error);
        } else {
            std::string error;
            if (!entry->runtime.refresh(effectContext, &error))
                throw std::runtime_error(error.empty() ? "generic Dayo FX refresh failed" : error);
        }

        if (!effects[index].graph.controllers.empty()) {
            if (evaluationSnapshot_ == nullptr)
                throw std::runtime_error("generic Dayo FX controller evaluation is unavailable");
            std::string error;
            const auto owner = effects[index].controllerModel.value_or(0);
            if (!resolveNativeControllerBlock(*entry->block, effects[index].graph.controllers, *evaluationSnapshot_,
                                              owner, &error))
                throw std::runtime_error(error.empty() ? "generic Dayo FX controller synchronization failed" : error);
        }
        std::string controllerError;
        if (!entry->controller.sync(*device_, entry->block->bytes(), &controllerError))
            throw std::runtime_error(controllerError.empty() ? "generic Dayo FX controller upload failed"
                                                             : controllerError);
        if (!hostBindings_.viewConstants.valid() || !hostBindings_.controllerConstants.valid())
            throw std::runtime_error("generic Dayo FX host frame bindings are incomplete");
        auto effectBindings = hostBindings_;
        effectBindings.controllerConstants = entry->controller.buffer();
        // MikuMikuDayo 1.30 appends globalCB after ControllerCB on passes
        // which declare controllers. Keep the effect-local allocation alive
        // with the runtime and bind it at b2 in that same case.
        if (!effects[index].graph.controllers.empty() && entry->globalVariables.buffer().valid())
            effectBindings.globalConstants = entry->globalVariables.buffer();
        auto frameBindings = nativeSceneFrameDescriptorBindings(effectBindings);
        const auto slot = device_->currentFrameSlot() % kNativeFramesInFlight;
        auto& frameSet = entry->frameSets[slot];
        if (frameSet.valid())
            device_->updateDescriptorSetEx(frameSet, frameBindings);
        else
            frameSet = device_->allocateDescriptorSetEx(layouts[0], frameBindings);
        if (!frameSet.valid())
            throw std::runtime_error("generic Dayo FX frame descriptor set allocation failed");
        std::vector<handles::DescriptorSetHandle> effectSets(descriptorSets.begin(), descriptorSets.end());
        effectSets[0] = frameSet;
        auto frame = entry->runtime.prepareFrame(effectContext, effectSets);
        auto stageResources = resources;
        // Deform/postprocess graphs own their fullscreen or compute dispatches;
        // material indexed draws belong only to the renderer graph.
        stageResources.sceneDraws = {};
        stageResources.rasterControllerModel.reset();
        stageResources.updatePassConstants = {};
        if (ownerModel != nullptr)
            resources.updateEffectPassConstants(commands, *ownerModel);
        if (!stageResources.defaultColorTarget.valid() && hostResourceProvider_.has_value()) {
            if (const auto output = hostResourceProvider_->resolve(DayoSemantic::RTOutput);
                output.has_value() && output->texture.valid())
                stageResources.defaultColorTarget = output->texture;
        }
        static_cast<void>(entry->runtime.execute(frame, commands, stageResources));
        if (ownerModel != nullptr && effects[index].controllerModel.has_value()) {
            deformerResources_.publish(*effects[index].controllerModel, effects[index].id,
                                       entry->runtime.nativeRuntime().resources().store());
        }
        auto output = entry->runtime.output(frame);
        if (!output.has_value() && stageResources.defaultColorTarget.valid()) {
            output = NativeFrameOutput{.texture = stageResources.defaultColorTarget,
                                       .extent = {context.renderWidth, context.renderHeight, 1},
                                       .format = PixelFormat::rgba16Float};
        }
        if (!output.has_value())
            continue;
        lastOutput = output;
        if (!publishToScreen)
            continue;

        if (!hostResourceProvider_.has_value())
            throw std::logic_error("postprocess stack has no Dayo host resource provider");
        const auto screen = hostResourceProvider_->resolve(DayoSemantic::ScreenTexture);
        if (!screen.has_value() || !screen->texture.valid())
            throw std::logic_error("postprocess stack requires a real ScreenTexture binding");
        if (output->format != PixelFormat::rgba16Float || output->extent.width != context.renderWidth ||
            output->extent.height != context.renderHeight || output->extent.depth != 1)
            throw std::logic_error("postprocess output is incompatible with the ScreenTexture chain target");
        if (output->texture != screen->texture) {
            commands.transferBarrierEx();
            commands.copyTextureEx(output->texture, screen->texture);
        }
    }
    return lastOutput;
}

bool NativeRendererCoordinator::updateEnvironment(const EnvironmentDesc& description) {
    return environmentService_.update(description);
}

std::optional<NativeFrameOutput> NativeRendererCoordinator::recordFrame(
    CommandList& commands, const fx::FxFrameContext& context, core::DirtyFlag dirty,
    std::span<const core::MaterialParameterBlock> materials, std::span<const AliasEntry> lightSampling,
    const EnvironmentGpuResult& environment, const FxExecutionResources& resources, NativeFrameExecution execution) {
    if (!status_.nativeReady)
        return std::nullopt;
    if (execution.sampleCount == 0 || execution.sampleIndex >= execution.sampleCount)
        throw std::invalid_argument("native frame sample index/count is invalid");
    if (execution.sampleCount == 1)
        outputSamples_.cancel();
    environmentService_.record(commands);
    static_cast<void>(executeGenericEffects(deformEffects_, deformRuntimes_, commands, context, resources, false));
    const auto& activeEnvironment =
        environment.skybox.valid() || environment.cubemap.valid() || environment.prefiltered.valid()
            ? environment
            : environmentService_.gpuResult();
    std::optional<NativeFrameOutput> rendererOutput;
    switch (status_.active) {
    case RendererKind::subayai: {
        auto frame = subayai_.prepareFrame(context, materials, lightSampling, activeEnvironment);
        const auto stats = subayai_.execute(frame, commands, resources);
        static_cast<void>(stats);
        rendererOutput = subayai_.output(frame);
        break;
    }
    case RendererKind::bdpt: {
        auto frame = bdpt_.prepareFrame(context, dirty, lightSampling);
        const auto stats = bdpt_.execute(frame, commands, resources);
        static_cast<void>(stats);
        rendererOutput = bdpt_.output(frame);
        break;
    }
    case RendererKind::preview:
        break;
    }
    if (!rendererOutput.has_value())
        return std::nullopt;
    if (execution.sampleCount > 1) {
        const auto resolved =
            outputSamples_.addSample(*device_, commands, *rendererOutput, execution.sampleIndex, execution.sampleCount);
        if (!resolved.has_value())
            return rendererOutput;
        rendererOutput = resolved;
    }
    if (postprocessEffects_.empty())
        return rendererOutput;

    if (!hostResourceProvider_.has_value())
        throw std::logic_error("postprocess stack has no Dayo host resource provider");
    const auto screen = hostResourceProvider_->resolve(DayoSemantic::ScreenTexture);
    if (!screen.has_value() || !screen->texture.valid())
        throw std::logic_error("postprocess stack requires a real ScreenTexture binding");
    if (rendererOutput->format != PixelFormat::rgba16Float || rendererOutput->extent.width != context.renderWidth ||
        rendererOutput->extent.height != context.renderHeight || rendererOutput->extent.depth != 1)
        throw std::logic_error("renderer output is incompatible with the ScreenTexture chain target");
    if (rendererOutput->texture != screen->texture) {
        commands.transferBarrierEx();
        commands.copyTextureEx(rendererOutput->texture, screen->texture);
    }
    if (const auto postOutput =
            executeGenericEffects(postprocessEffects_, postprocessRuntimes_, commands, context, resources, true);
        postOutput.has_value())
        return postOutput;
    return rendererOutput;
}

void NativeRendererCoordinator::reset() noexcept {
    deformRuntimes_.clear();
    postprocessRuntimes_.clear();
    deformerResources_.clear();
    outputSamples_.reset();
    deformEffects_.clear();
    postprocessEffects_.clear();
    bdpt_.reset();
    subayai_.reset();
    status_ = {};
    device_ = nullptr;
}

const fx::FxProgram* NativeRendererCoordinator::program() const noexcept {
    if (!status_.nativeReady)
        return nullptr;
    if (status_.active == RendererKind::subayai)
        return subayai_.program();
    if (status_.active == RendererKind::bdpt)
        return bdpt_.program();
    return nullptr;
}

} // namespace dayo::graphics
