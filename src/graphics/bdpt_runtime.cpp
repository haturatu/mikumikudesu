#include "graphics/bdpt_runtime.hpp"

#include <stdexcept>
#include <utility>

namespace dayo::graphics {

bool BdptRuntime::initialize(Device& device, fx::FxProgram program, std::string* error) {
    if (error != nullptr)
        error->clear();
    reset();
    if (!device.capabilities().hardwareSupportsBdpt()) {
        if (error != nullptr)
            *error = device.capabilities().missingFeatures(RendererKind::bdpt);
        return false;
    }
    const auto required = fx::requiredFeatures(program);
    if (!required.accelerationStructure || !required.rayTracingPipeline) {
        if (error != nullptr)
            *error = "BDPT graph does not declare the required acceleration-structure and RT-pipeline features";
        return false;
    }
    if (required.rayQuery && !device.capabilities().rayQuery) {
        if (error != nullptr)
            *error = "BDPT graph requires ray query support";
        return false;
    }
    device_ = &device;
    program_ = std::move(program);
    ready_ = true;
    return true;
}

void BdptRuntime::reset() noexcept {
    if (device_ != nullptr)
        accumulation_.releaseGpuResources(*device_);
    device_ = nullptr;
    program_ = {};
    accumulation_ = {};
    ready_ = false;
}

bool BdptRuntime::ensureResources(std::uint32_t width, std::uint32_t height, std::string* error) {
    if (!ready_ || device_ == nullptr)
        throw std::logic_error("BDPT runtime is not initialized");
    return accumulation_.ensureGpuResources(*device_, width, height, 32, error);
}

BdptFrame BdptRuntime::prepareFrame(const fx::FxFrameContext& context, core::DirtyFlag dirty) {
    if (!ready_ || device_ == nullptr)
        throw std::logic_error("BDPT runtime is not initialized");
    std::string error;
    if (!ensureResources(context.renderWidth, context.renderHeight, &error))
        throw std::runtime_error(error.empty() ? "BDPT GPU resources are unavailable" : error);
    BdptFrame frame;
    frame.context = context;
    frame.plan = fx::FxCompiler{}.plan(program_, context);
    frame.clearAccumulation = accumulation_.beginFrame(dirty);
    frame.sampleIndex = accumulation_.sampleIndex();
    frame.gpu = accumulation_.gpuResources();
    return frame;
}

VulkanFxExecutor::Stats BdptRuntime::execute(BdptFrame& frame, CommandList& commands,
                                             const FxExecutionResources& resources) const {
    if (!ready_ || device_ == nullptr)
        throw std::logic_error("BDPT runtime is not initialized");
    return VulkanFxExecutor{*device_}.execute(frame.plan, commands, frame.context, resources);
}

} // namespace dayo::graphics
