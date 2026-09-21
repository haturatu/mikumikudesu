#include "graphics/native_renderer.hpp"

#include <sstream>
#include <ranges>
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
    environmentBackend_ = backend;
    subayai_.setEnvironmentBackend(backend);
}

void NativeRendererCoordinator::setSceneFrameRuntime(NativeSceneFrameRuntime* runtime) noexcept {
    sceneFrameRuntime_ = runtime;
    subayai_.setSceneFrameRuntime(runtime);
    bdpt_.setSceneFrameRuntime(runtime);
}

void NativeRendererCoordinator::setHostResourceBindings(const NativeSceneResourceBindings& bindings) noexcept {
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
}

void NativeRendererCoordinator::setControllerDeclarations(std::span<const core::EffectController> declarations) {
    const auto same = std::ranges::equal(controllerDeclarations_, declarations, [](const auto& left, const auto& right) {
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
        auto& runtime = runtimes[index];
        if (!runtime) {
            auto program = fx::FxCompiler{}.compile(effects[index].graph);
            runtime = std::make_unique<DayoFxRuntime>();
            runtime->addProvider(sceneHostProvider_);
            std::string error;
            fx::FxNativeShaderSourceOptions sourceOptions;
            sourceOptions.controllerDeclarations = controllerDeclarations_;
            if (!runtime->initializeForFrame(*device_, std::move(program), fx::FxShaderCompiler{}, context, layouts,
                                             &error, descriptorSets, std::move(sourceOptions)))
                throw std::runtime_error(error.empty() ? "generic Dayo FX initialization failed" : error);
        } else {
            std::string error;
            if (!runtime->refresh(context, &error))
                throw std::runtime_error(error.empty() ? "generic Dayo FX refresh failed" : error);
        }

        auto frame = runtime->prepareFrame(context, descriptorSets);
        if (evaluationSnapshot_ != nullptr && !effects[index].graph.controllers.empty()) {
            std::string error;
            const auto owner = effects[index].controllerModel.value_or(context.currentModel);
            if (!sceneFrameRuntime_->syncControllers(effects[index].graph.controllers, *evaluationSnapshot_, owner,
                                                     &error))
                throw std::runtime_error(error.empty() ? "generic Dayo FX controller synchronization failed" : error);
        }
        auto stageResources = resources;
        // Deform/postprocess graphs own their fullscreen or compute dispatches;
        // material indexed draws belong only to the renderer graph.
        stageResources.sceneDraws = {};
        stageResources.rasterControllerModel.reset();
        stageResources.updatePassConstants = {};
        static_cast<void>(runtime->execute(frame, commands, stageResources));
        const auto output = runtime->output(frame);
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
    if (auto* runtime = subayai())
        return runtime->updateEnvironment(description);
    return false;
}

std::optional<NativeFrameOutput>
NativeRendererCoordinator::recordFrame(CommandList& commands, const fx::FxFrameContext& context, core::DirtyFlag dirty,
                                       std::span<const core::MaterialParameterBlock> materials,
                                       std::span<const AliasEntry> lightSampling,
                                       const EnvironmentGpuResult& environment, const FxExecutionResources& resources) {
    if (!status_.nativeReady)
        return std::nullopt;
    static_cast<void>(executeGenericEffects(deformEffects_, deformRuntimes_, commands, context, resources, false));
    std::optional<NativeFrameOutput> rendererOutput;
    switch (status_.active) {
    case RendererKind::subayai: {
        auto frame = subayai_.prepareFrame(context, materials, lightSampling, environment);
        subayai_.recordEnvironment(commands);
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
    if (const auto postOutput = executeGenericEffects(postprocessEffects_, postprocessRuntimes_, commands, context,
                                                      resources, true);
        postOutput.has_value())
        return postOutput;
    return rendererOutput;
}

void NativeRendererCoordinator::reset() noexcept {
    deformRuntimes_.clear();
    postprocessRuntimes_.clear();
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
