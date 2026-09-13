#include "graphics/native_renderer.hpp"

#include <sstream>
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

std::string missingEffectFeatures(const DeviceCapabilities& capabilities,
                                  const fx::FxRequiredFeatures& required) {
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
    status_ = decideNativeRendererForInitialization(device.capabilities(), requested, fx::requiredFeatures(program));
    if (status_.fellBack() || requested == RendererKind::preview)
        return status_;

    std::string error;
    bool initialized = false;
    switch (requested) {
    case RendererKind::subayai:
        initialized = subayai_.initialize(device, std::move(program), &error);
        if (initialized)
            subayai_.setEnvironmentBackend(environmentBackend_);
        break;
    case RendererKind::bdpt:
        initialized = bdpt_.initialize(device, std::move(program), &error);
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

bool NativeRendererCoordinator::updateEnvironment(const EnvironmentDesc& description) {
    if (auto* runtime = subayai())
        return runtime->updateEnvironment(description);
    return false;
}

std::optional<NativeFrameOutput>
NativeRendererCoordinator::recordFrame(CommandList& commands, const fx::FxFrameContext& context,
                                       core::DirtyFlag dirty,
                                       std::span<const core::MaterialParameterBlock> materials,
                                       std::span<const AliasEntry> lightSampling,
                                       const EnvironmentGpuResult& environment) {
    if (!status_.nativeReady)
        return std::nullopt;
    switch (status_.active) {
    case RendererKind::subayai: {
        auto frame = subayai_.prepareFrame(context, materials, lightSampling, environment);
        subayai_.recordEnvironment(commands);
        const auto stats = subayai_.execute(frame, commands);
        static_cast<void>(stats);
        return subayai_.output(frame);
    }
    case RendererKind::bdpt: {
        auto frame = bdpt_.prepareFrame(context, dirty);
        const auto stats = bdpt_.execute(frame, commands);
        static_cast<void>(stats);
        return bdpt_.output(frame);
    }
    case RendererKind::preview:
        return std::nullopt;
    }
    return std::nullopt;
}

void NativeRendererCoordinator::reset() noexcept {
    bdpt_.reset();
    subayai_.reset();
    status_ = {};
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
